//===- ACanonicalize.cpp - `a` dialect canonicalization -*- C++ -*-===//
//
// Part of the A project. See docs/mlir-architecture.md.
//
// Implements the canonicalization/folding patterns for the `a` dialect.
// Everything here is deliberately conservative: a missed fold is fine, a
// wrong fold is not.
//
// Implemented patterns:
//
//   1. `a.to_aval(a.from_aval %aval)` -> `%aval`
//      Re-boxing an unboxed scalar round-trips to the same aval. Restricted
//      to integer (iN/i1) `a.from_aval` results: float re-boxing
//      canonicalizes NaN payloads, so the aval word is not preserved
//      bit-for-bit.
//
//   2. `a.from_aval(a.to_aval %x)` -> `%x`
//      Unboxing a freshly boxed scalar is the identity. Requires the
//      scalar type to round-trip exactly (same IntegerType on both sides);
//      float results are excluded for the same NaN reason as (1). The
//      `uvalue` attrs need not match: `a.from_aval`'s lowering ignores it
//      and `a.to_aval`'s only steers the encode-time range check, which
//      never changes the decoded value.
//
//   3. `a.from_aval(a.const "int" {ivalue = N})` -> `arith.constant N`
//      Folds the unbox of a literal int through the aval encoding. Only
//      fires when N fits the signed 46-bit inline-int range (|N| < 2^46),
//      where the lowering emits a pure tagged-i64 constant with no
//      `a_i64n`/`a_int_full` runtime calls. Result must be an integer
//      scalar of width >= 2: `a.const int` lowers to an int-tagged aval,
//      which `a.from_aval -> i1` would reject at runtime (bool avals are a
//      different encoding), so folding there would remove a trap.
//
//   4. `a.to_aval %aval` -> `%aval` (operand already `!a.aval`)
//      The verifier admits this identity and `a-lower-typed` forwards the
//      operand unchanged; fold it here instead.
//
//   5. `a.gep %p[0]` -> `%p`
//      Zero-offset GEP is the identity pointer. The index must be an
//      integer constant equal to zero (matches `arith.constant`,
//      `llvm.mlir.constant`, etc. via the ConstantLike matcher).
//
// Deliberately NOT folded (see task constraints):
//   - `a.runtime.call` (side effects unknown),
//   - `a.const` dedup (CSE already handles identical attrs),
//   - `a.call` DCE / dead `a.yield`/`a.condition` (separate DCE/lowering),
//   - `a.addressof` of const globals (needs symbol-table reasoning),
//   - `a.const "none"`/`str`/`float` through `a.from_aval` (none/str have
//     no scalar decode; float hits the NaN-canonicalization caveat).
//
//===----------------------------------------------------------------------===//

#include "ACanonicalize.hpp"

#include "A/A.h"

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/PassRegistry.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "llvm/ADT/APInt.h"

#include <cstdint>

namespace a {
namespace {

using mlir::IntegerType;
using mlir::LogicalResult;
using mlir::PatternRewriter;
using mlir::Type;
using llvm::APInt;

/// Signed range of the phase-1 inline-int aval encoding
/// (-2^46 <= v < 2^46); mirrors kInlineLim in ALowerToLLVM.cpp /
/// a_runtime.h. Outside this range `a.const int` lowers through
/// `a_i64n`/`a_int_full` runtime calls, which we do not fold.
constexpr std::int64_t kInlineIntLim = INT64_C(1) << 46;

//===----------------------------------------------------------------------===//
// Pattern 1: a.to_aval(a.from_aval %aval) -> %aval
//===----------------------------------------------------------------------===//

struct ToAvalOfFromAvalPattern
    : public mlir::OpRewritePattern<mlir::a::AToAvalOp> {
  using mlir::OpRewritePattern<mlir::a::AToAvalOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(mlir::a::AToAvalOp op,
                                PatternRewriter &rewriter) const override {
    auto from = op.getValue().getDefiningOp<mlir::a::AFromAvalOp>();
    if (!from)
      return rewriter.notifyMatchFailure(op,
                                         "operand is not defined by a.from_aval");
    // Integer-only: float unbox/rebox canonicalizes NaN payloads, so the
    // aval word would not round-trip bit-for-bit.
    if (!mlir::isa<IntegerType>(from.getResult().getType()))
      return rewriter.notifyMatchFailure(
          op, "a.from_aval result is not an integer scalar");
    rewriter.replaceOp(op, from.getValue());
    return mlir::success();
  }
};

//===----------------------------------------------------------------------===//
// Pattern 2: a.from_aval(a.to_aval %x) -> %x
//===----------------------------------------------------------------------===//

struct FromAvalOfToAvalPattern
    : public mlir::OpRewritePattern<mlir::a::AFromAvalOp> {
  using mlir::OpRewritePattern<mlir::a::AFromAvalOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(mlir::a::AFromAvalOp op,
                                PatternRewriter &rewriter) const override {
    auto to = op.getValue().getDefiningOp<mlir::a::AToAvalOp>();
    if (!to)
      return rewriter.notifyMatchFailure(op,
                                         "operand is not defined by a.to_aval");
    Type srcTy = to.getValue().getType();
    // The scalar must round-trip with the same type on both sides, and
    // must be an integer (floats hit the NaN canonicalization caveat).
    if (!mlir::isa<IntegerType>(srcTy) || srcTy != op.getResult().getType())
      return rewriter.notifyMatchFailure(
          op, "a.to_aval operand type does not match a.from_aval result");
    rewriter.replaceOp(op, to.getValue());
    return mlir::success();
  }
};

//===----------------------------------------------------------------------===//
// Pattern 3: a.from_aval(a.const "int" {ivalue = N}) -> arith.constant N
//===----------------------------------------------------------------------===//

struct FromAvalConstIntPattern
    : public mlir::OpRewritePattern<mlir::a::AFromAvalOp> {
  using mlir::OpRewritePattern<mlir::a::AFromAvalOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(mlir::a::AFromAvalOp op,
                                PatternRewriter &rewriter) const override {
    auto cst = op.getValue().getDefiningOp<mlir::a::AConstOp>();
    if (!cst)
      return rewriter.notifyMatchFailure(
          op, "operand is not defined by a.const");
    if (cst.getKind() != mlir::a::AConstKind::Int)
      return rewriter.notifyMatchFailure(op, "a.const kind is not `int`");
    auto iv = cst.getIvalueAttr();
    if (!iv)
      return rewriter.notifyMatchFailure(op, "a.const int has no `ivalue`");

    Type resTy = op.getResult().getType();
    auto intTy = mlir::dyn_cast<IntegerType>(resTy);
    // i1 excluded: a.const int lowers to an int-tagged aval which the bool
    // decode path would reject at runtime; folding would remove that trap.
    if (!intTy || intTy.getWidth() < 2 || intTy.getWidth() > 64)
      return rewriter.notifyMatchFailure(
          op, "a.from_aval result is not an integer scalar of width 2..64");

    std::int64_t v = iv.getInt();
    if (v < -kInlineIntLim || v >= kInlineIntLim)
      return rewriter.notifyMatchFailure(
          op, "ivalue outside the inline-int range; lowering uses runtime calls");

    // The inline decode yields the sign-extended 46-bit payload as i64,
    // then truncates to the result width: exactly `v` truncated to iN.
    // (ivalue is an i64 IntegerAttr; getInt() returns the sign-extended
    // int64_t.)
    APInt bits(64, static_cast<std::uint64_t>(v));
    APInt val = bits.sextOrTrunc(intTy.getWidth());
    auto attr = mlir::IntegerAttr::get(resTy, val);
    auto constant =
        rewriter.create<mlir::arith::ConstantOp>(op.getLoc(), resTy, attr);
    rewriter.replaceOp(op, constant.getResult());
    return mlir::success();
  }
};

//===----------------------------------------------------------------------===//
// Pattern 4: a.to_aval %aval -> %aval (operand already boxed)
//===----------------------------------------------------------------------===//

struct ToAvalIdentityPattern
    : public mlir::OpRewritePattern<mlir::a::AToAvalOp> {
  using mlir::OpRewritePattern<mlir::a::AToAvalOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(mlir::a::AToAvalOp op,
                                PatternRewriter &rewriter) const override {
    if (!mlir::isa<mlir::a::AVALType>(op.getValue().getType()))
      return rewriter.notifyMatchFailure(op, "operand is not !a.aval");
    rewriter.replaceOp(op, op.getValue());
    return mlir::success();
  }
};

//===----------------------------------------------------------------------===//
// Pattern 5: a.gep %p[0] -> %p
//===----------------------------------------------------------------------===//

struct ZeroIndexGepPattern
    : public mlir::OpRewritePattern<mlir::a::AGetElementPtrOp> {
  using mlir::OpRewritePattern<mlir::a::AGetElementPtrOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(mlir::a::AGetElementPtrOp op,
                                PatternRewriter &rewriter) const override {
    APInt index;
    if (!mlir::matchPattern(op.getIndex(), mlir::m_ConstantInt(&index)))
      return rewriter.notifyMatchFailure(op, "index is not a constant");
    if (!index.isZero())
      return rewriter.notifyMatchFailure(op, "index is not zero");
    rewriter.replaceOp(op, op.getPtr());
    return mlir::success();
  }
};

} // namespace

static void populateACanonicalizePatterns(mlir::RewritePatternSet &patterns) {
  patterns.add<ToAvalOfFromAvalPattern, FromAvalOfToAvalPattern,
               FromAvalConstIntPattern, ToAvalIdentityPattern,
               ZeroIndexGepPattern>(patterns.getContext());
}

void ACanonicalizePass::getDependentDialects(
    mlir::DialectRegistry &registry) const {
  // The const-fold pattern materializes `arith.constant`.
  registry.insert<mlir::arith::ArithDialect>();
}

void ACanonicalizePass::runOnOperation() {
  mlir::RewritePatternSet patterns(&getContext());
  populateACanonicalizePatterns(patterns);
  mlir::FrozenRewritePatternSet frozen(std::move(patterns));
  if (mlir::applyPatternsAndFoldGreedily(getOperation(), frozen).failed())
    signalPassFailure();
}

std::unique_ptr<mlir::Pass> createACanonicalizePass() {
  return std::make_unique<ACanonicalizePass>();
}

} // namespace a
