//===- ALowerTyped.cpp - a.to_aval/a.from_aval -> llvm lowering -*- C++ -*-===//
//
// Part of the A project. See docs/mlir-typed-values-design.md (M1).
//
// Implements the boxing boundary between M1 unboxed scalars (iN/fN/i1) and
// the phase-1 boxed `!a.aval` representation (tagged i64). The encodings are
// bit-identical to src/astra/frontend/mlir/mlir_emit.cpp
// (avalInt/avalDbl/avalBool, kAvalTag/kInlineLim) and
// src/astra/runtime/a_runtime.h (a_mkint, A_TAG, A_INT_LIM):
//
//   aval word      : uint64; top 16 bits == 0xFFFF marks a tagged value.
//   inline int     : A_TAG | (((v << 1) | 1) & 0x0000FFFFFFFFFFFF), valid
//                    for -2^46 <= v < 2^46 (the 46-bit payload sign-extends
//                    from bit 46 on decode -- see a_int_val).
//   boxed int      : llvm.call @a_i64n(i64) for out-of-inline-range i64.
//   double         : bit pattern of the f64 as i64 (untagged unless the bits
//                    collide with tag space; negative-NaN payloads are the
//                    documented NaN-boxing collision).
//   bool           : A_TAG|6 (true) / A_TAG|4 (false).
//
// `a.from_aval` is the *unchecked ABI unpack*: it is only emitted where Sema
// proves the dynamic type (call/load boundaries). Int unboxing keeps a fast
// inline-decode path and falls back to @a_int_full for heap-boxed (T_INT64)
// avals; float/bool tag mismatches lower to llvm.trap + llvm.unreachable.
// The checked mid-expression narrowing op (`a.unbox<T>`) is deferred to a
// later milestone.
//
//===----------------------------------------------------------------------===//

#include "ALowerTyped.hpp"

#include "../pipeline.hpp" // for a::registerADialect (dynamic dialect reg)

#include "A/A.h"

#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMIntrinsicOps.h.inc"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/DialectRegistry.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/IR/Visitors.h"
#include "llvm/ADT/SmallVector.h"

#include <cstdint>

namespace a {
namespace {

using mlir::Block;
using mlir::IntegerType;
using mlir::LLVM::AndOp;
using mlir::LLVM::BitcastOp;
using mlir::LLVM::BrOp;
using mlir::LLVM::CallOp;
using mlir::LLVM::CondBrOp;
using mlir::LLVM::ConstantOp;
using mlir::LLVM::FCmpOp;
using mlir::LLVM::FCmpPredicate;
using mlir::LLVM::FCmpPredicateAttr;
using mlir::LLVM::FPExtOp;
using mlir::LLVM::FPTruncOp;
using mlir::LLVM::ICmpOp;
using mlir::LLVM::ICmpPredicate;
using mlir::LLVM::ICmpPredicateAttr;
using mlir::LLVM::LLVMFuncOp;
using mlir::LLVM::LLVMFunctionType;
using mlir::LLVM::LShrOp;
using mlir::LLVM::OrOp;
using mlir::LLVM::SExtOp;
using mlir::LLVM::SelectOp;
using mlir::LLVM::ShlOp;
using mlir::LLVM::Trap;
using mlir::LLVM::TruncOp;
using mlir::LLVM::UnreachableOp;
using mlir::LLVM::ZExtOp;
using mlir::Location;
using mlir::LogicalResult;
using mlir::ModuleOp;
using mlir::Operation;
using mlir::OpBuilder;
using mlir::Type;
using mlir::Value;
using mlir::ValueRange;
using mlir::success;
using mlir::failure;
using llvm::ArrayRef;
using llvm::SmallVector;

/// Dialect namespace of the A dialect (see docs/mlir-architecture.md).
constexpr llvm::StringLiteral kADialectName = "a";

//===----------------------------------------------------------------------===//
// Aval ABI constants (mirror a_runtime.h / mlir_emit.cpp)
//===----------------------------------------------------------------------===//

constexpr std::int64_t kAvalTag = -281474976710656LL;  // 0xFFFF000000000000
constexpr std::uint64_t kAvalTagU = 0xFFFF000000000000ULL;
constexpr std::int64_t kInlineLim = INT64_C(1) << 46;
constexpr std::uint64_t kAvalPayloadMask = 0x0000FFFFFFFFFFFFULL;
constexpr std::uint64_t kAvalTrue = kAvalTagU | 0x6ULL;   // A_TRUE
constexpr std::uint64_t kAvalFalse = kAvalTagU | 0x4ULL;  // A_FALSE

IntegerType i64Ty(mlir::MLIRContext *ctx) { return IntegerType::get(ctx, 64); }
IntegerType i1Ty(mlir::MLIRContext *ctx) { return IntegerType::get(ctx, 1); }

Value emitI64Const(OpBuilder &b, Location loc, std::int64_t v) {
  return b.create<ConstantOp>(loc, i64Ty(loc.getContext()), v);
}

/// `llvm.icmp <pred> %lhs, %rhs` producing i1.
Value emitICmp(OpBuilder &b, Location loc, ICmpPredicate pred, Value lhs,
               Value rhs) {
  return b.create<ICmpOp>(loc, i1Ty(loc.getContext()),
                          ICmpPredicateAttr::get(loc.getContext(), pred),
                          lhs, rhs);
}

Value emitICmpEq(OpBuilder &b, Location loc, Value lhs, Value rhs) {
  return emitICmp(b, loc, ICmpPredicate::eq, lhs, rhs);
}

/// Ensure `llvm.func @<name>(i64) -> i64` is declared in `module`.
LogicalResult ensureRuntimeI64Decl(Operation *op, llvm::StringRef name) {
  ModuleOp module = op->getParentOfType<ModuleOp>();
  if (!module) {
    op->emitError("a-lower-typed: op outside of a module");
    return failure();
  }
  if (!mlir::SymbolTable::lookupSymbolIn(module.getOperation(), name)) {
    OpBuilder mb(module.getOperation());
    mb.setInsertionPointToStart(&module.getBodyRegion().front());
    mb.create<LLVMFuncOp>(op->getLoc(), name,
                          LLVMFunctionType::get(i64Ty(op->getContext()),
                                                ArrayRef<Type>{i64Ty(op->getContext())}));
  }
  return success();
}

/// `llvm.call @<name>(i64) -> i64` emitted at `b`'s position.
Value emitRuntimeI64CallAt(OpBuilder &b, Location loc, llvm::StringRef name,
                           Value v64) {
  auto call = b.create<CallOp>(loc, SmallVector<Type>{i64Ty(loc.getContext())},
                               name, ArrayRef<Value>{v64});
  return call.getResult();
}

/// `llvm.call @a_i64n(i64) -> i64`, declaring the runtime fn in the module
/// if needed. `a_i64n` is the full-range boxed-integer constructor
/// (a_runtime.c: `mkint_full`).
LogicalResult emitAI64nCall(Operation *op, Value v64, Value &out) {
  if (failed(ensureRuntimeI64Decl(op, "a_i64n")))
    return failure();
  OpBuilder b(op);
  out = emitRuntimeI64CallAt(b, op->getLoc(), "a_i64n", v64);
  return success();
}

/// Emit `llvm.trap; llvm.unreachable` at the builder's current position
/// (aval tag mismatch on a.from_aval).
void emitTrap(OpBuilder &b, Location loc) {
  b.create<Trap>(loc);
  b.create<UnreachableOp>(loc);
}

//===----------------------------------------------------------------------===//
// a.to_aval lowering
//===----------------------------------------------------------------------===//

/// Box an already-i64 value as an aval: inline tag encoding when
/// -2^46 <= v < 2^46 (signed) or 0 <= v < 2^46 (unsigned), else @a_i64n.
LogicalResult emitBoxI64(Operation *op, Value v64, bool isUnsigned, Value &out) {
  OpBuilder b(op);
  Location loc = op->getLoc();
  mlir::MLIRContext *ctx = op->getContext();
  Type i64 = i64Ty(ctx);
  Type i1 = i1Ty(ctx);

  Value loOk, hiOk;
  if (isUnsigned) {
    Value zero = emitI64Const(b, loc, 0);
    Value lim = emitI64Const(b, loc, kInlineLim);
    loOk = emitICmp(b, loc, ICmpPredicate::uge, v64, zero);
    hiOk = emitICmp(b, loc, ICmpPredicate::ult, v64, lim);
  } else {
    Value nlim = emitI64Const(b, loc, -kInlineLim);
    Value plim = emitI64Const(b, loc, kInlineLim);
    loOk = emitICmp(b, loc, ICmpPredicate::sge, v64, nlim);
    hiOk = emitICmp(b, loc, ICmpPredicate::slt, v64, plim);
  }
  Value inRange = b.create<AndOp>(loc, i1, loOk, hiOk);

  // Fast path: tagged inline-int encoding of v.
  //   payload = ((v << 1) | 1) & 0x0000FFFFFFFFFFFF
  //   aval    = 0xFFFF000000000000 | payload
  Value one = emitI64Const(b, loc, 1);
  Value shl = b.create<ShlOp>(loc, i64, v64, one);
  Value payload = b.create<OrOp>(loc, i64, shl, one);
  Value mask = emitI64Const(b, loc, (std::int64_t)kAvalPayloadMask);
  payload = b.create<AndOp>(loc, i64, payload, mask);
  Value tag = emitI64Const(b, loc, kAvalTag);
  Value inlineEnc = b.create<OrOp>(loc, i64, tag, payload);

  // Slow path: @a_i64n(v).
  Value boxed;
  if (failed(emitAI64nCall(op, v64, boxed)))
    return failure();

  out = b.create<SelectOp>(loc, i64, inRange, inlineEnc, boxed);
  return success();
}

/// Lower `a.to_aval` for an integer operand: extend to i64 then box.
LogicalResult lowerToAvalInt(mlir::a::AToAvalOp op, IntegerType intTy,
                             Value &out) {
  OpBuilder b(op);
  Location loc = op->getLoc();
  mlir::MLIRContext *ctx = op->getContext();
  unsigned width = intTy.getWidth();
  bool isUnsigned = op.getUvalue().has_value();

  Value v = op.getValue();
  if (width == 64) {
    return emitBoxI64(op, v, isUnsigned, out);
  }
  // Extend narrow ints to i64 before boxing. zext for unsigned, sext for
  // signed (the 46-bit payload sign-extends on decode, matching a_mkint).
  Value v64;
  if (isUnsigned) {
    v64 = b.create<ZExtOp>(loc, i64Ty(ctx), v);
  } else {
    v64 = b.create<SExtOp>(loc, i64Ty(ctx), v);
  }
  return emitBoxI64(op, v64, isUnsigned, out);
}

/// Lower `a.to_aval` for a float operand: normalize to f64 (fpext/fptrunc),
/// canonicalize NaN (mirroring runtime `mkdouble`: any NaN becomes the quiet
/// NaN 0x7FF8000000000000 so it cannot collide with the aval tag space),
/// then bitcast to i64 -- the aval double payload is the raw f64 bit pattern.
LogicalResult lowerToAvalFloat(mlir::a::AToAvalOp op, Type floatTy,
                               Value &out) {
  OpBuilder b(op);
  Location loc = op->getLoc();
  mlir::MLIRContext *ctx = op->getContext();
  Type f64Ty = mlir::Float64Type::get(ctx);

  Value v = op.getValue();
  unsigned width = cast<mlir::FloatType>(floatTy).getWidth();
  Value f64v;
  if (width == 64) {
    f64v = v;
  } else if (width < 64) {
    f64v = b.create<FPExtOp>(loc, f64Ty, v);
  } else {
    // Wider floats (f80/f128) cannot be bitcast to i64; the aval ABI only
    // carries f64 payloads, so narrow to f64 first.
    f64v = b.create<FPTruncOp>(loc, f64Ty, v);
  }

  // `fcmp uno %d, %d` is true iff %d is NaN; select the canonical qNaN.
  Value isNaN = b.create<FCmpOp>(loc, i1Ty(ctx), FCmpPredicate::uno, f64v,
                                  f64v);
  Value nanBits = emitI64Const(b, loc, 0x7FF8000000000000LL);
  Value canonNaN = b.create<BitcastOp>(loc, f64Ty, nanBits);
  Value boxed = b.create<SelectOp>(loc, f64Ty, isNaN, canonNaN, f64v);
  out = b.create<BitcastOp>(loc, i64Ty(ctx), boxed);
  return success();
}

/// Lower `a.to_aval` for an i1 (bool) operand: select A_TRUE/A_FALSE.
LogicalResult lowerToAvalBool(mlir::a::AToAvalOp op, Value &out) {
  OpBuilder b(op);
  Location loc = op->getLoc();
  mlir::MLIRContext *ctx = op->getContext();
  Type i64 = i64Ty(ctx);
  Value isTrue = emitICmpEq(b, loc, op.getValue(),
                            b.create<ConstantOp>(loc, i1Ty(ctx), 1));
  Value t = emitI64Const(b, loc, (std::int64_t)kAvalTrue);
  Value f = emitI64Const(b, loc, (std::int64_t)kAvalFalse);
  out = b.create<SelectOp>(loc, i64, isTrue, t, f);
  return success();
}

LogicalResult lowerToAval(mlir::a::AToAvalOp op) {
  Value out;
  Type srcTy = op.getValue().getType();
  // Already boxed: `a.to_aval` on `!a.aval` is the identity (the verifier
  // admits it; the canonicalizer folds the pair away when it can).
  if (mlir::isa<mlir::a::AVALType>(srcTy)) {
    out = op.getValue();
    op.getResult().replaceAllUsesWith(out);
    op.erase();
    return success();
  }
  LogicalResult res = success();
  if (auto intTy = mlir::dyn_cast<IntegerType>(srcTy)) {
    if (intTy.getWidth() == 1) {
      res = lowerToAvalBool(op, out);
    } else {
      res = lowerToAvalInt(op, intTy, out);
    }
  } else if (mlir::isa<mlir::FloatType>(srcTy)) {
    res = lowerToAvalFloat(op, srcTy, out);
  } else {
    op.emitError("a-lower-typed: unsupported a.to_aval operand type `")
        << srcTy << "`";
    return failure();
  }
  if (failed(res))
    return res;
  op.getResult().replaceAllUsesWith(out);
  op.erase();
  return success();
}

//===----------------------------------------------------------------------===//
// a.from_aval lowering
//===----------------------------------------------------------------------===//

/** Prelude state for a.from_aval lowering: after emitAvalTagCheckPrelude
 * runs, `orig` ends in `llvm.cond_br %ok, ^decode, ^else`, where `else` is
 * `trap` (llvm.trap+llvm.unreachable) unless `slow` is set -- for int
 * unboxing the else edge goes to `slow`, an empty block the caller fills
 * with an `a_int_full` call so heap-boxed (T_INT64) avals decode too.
 * `decode` is likewise an empty block the caller fills; `merge` is where
 * the decoded value is materialized for uses in the continuation. */
struct AvalCheck {
  Block *decode;
  Block *slow;  // non-null only for the int boxed path; else edge target
  Block *merge;
  Value aval;  // the !a.aval operand as i64 (defined in orig, dominates all)
};

/// Emit the tag check for `aval` (already the i64 word) and split control
/// flow into orig -> {decode -> merge, else}. `boxedIntPath` selects the
/// int slow path (call @a_int_full on tag mismatch) over llvm.trap.
/// Returns false on bad kind.
LogicalResult emitAvalTagCheckPrelude(Operation *op, Value v64, int kind,
                                      AvalCheck &chk, bool boxedIntPath = false) {
  OpBuilder b(op);
  Location loc = op->getLoc();
  mlir::MLIRContext *ctx = op->getContext();
  Type i64 = i64Ty(ctx);
  Type i1 = i1Ty(ctx);

  Value ok;
  if (kind == 0) {
    // Int: tagged (top 16 bits == 0xFFFF) and payload bit0 == 1.
    Value mask = emitI64Const(b, loc, (std::int64_t)kAvalTagU);
    Value tagBits = b.create<AndOp>(loc, i64, v64, mask);
    Value tagOk = emitICmpEq(b, loc, tagBits, emitI64Const(b, loc, kAvalTag));
    Value one = emitI64Const(b, loc, 1);
    Value bit0 = b.create<AndOp>(loc, i64, v64, one);
    Value intOk = emitICmpEq(b, loc, bit0, one);
    ok = b.create<AndOp>(loc, i1, tagOk, intOk);
  } else if (kind == 1) {
    // Float: untagged aval (top 16 bits clear) -- the raw f64 bit pattern.
    Value mask = emitI64Const(b, loc, (std::int64_t)kAvalTagU);
    Value tagBits = b.create<AndOp>(loc, i64, v64, mask);
    ok = emitICmp(b, loc, ICmpPredicate::eq, tagBits,
                  emitI64Const(b, loc, 0));
  } else {
    // Bool: exact A_TRUE / A_FALSE small constants.
    Value t = emitI64Const(b, loc, (std::int64_t)kAvalTrue);
    Value f = emitI64Const(b, loc, (std::int64_t)kAvalFalse);
    Value isT = emitICmpEq(b, loc, v64, t);
    Value isF = emitICmpEq(b, loc, v64, f);
    ok = b.create<OrOp>(loc, i1, isT, isF);
  }

  // Split the block at `op`: ops before it (the check) stay in `orig`,
  // `op` and everything after move to `cont`.
  Block *orig = op->getBlock();
  Block *cont = orig->splitBlock(op->getIterator());
  Block *decodeBB = new Block;
  Block *elseBB = new Block;
  Block *mergeBB = cont;  // reuse the continuation as the merge block

  // Block order: orig, decodeBB, elseBB, mergeBB (cont).
  decodeBB->insertAfter(orig);
  elseBB->insertAfter(decodeBB);

  OpBuilder termB = OpBuilder::atBlockEnd(orig);
  termB.create<CondBrOp>(loc, ok, decodeBB, ValueRange{}, elseBB,
                         ValueRange{});

  if (boxedIntPath) {
    // Int unboxing slow path: the aval may be a heap-boxed T_INT64 (what
    // @a_i64n returns for |v| >= 2^46). @a_int_full handles both inline and
    // boxed ints and panics on non-ints, so it replaces llvm.trap here.
    chk.slow = elseBB;
  } else {
    OpBuilder elseB = OpBuilder::atBlockBegin(elseBB);
    emitTrap(elseB, loc);
    chk.slow = nullptr;
  }

  // decodeBB/slow get their terminators once the caller finishes emitting
  // the decode bodies.
  chk.decode = decodeBB;
  chk.merge = mergeBB;
  chk.aval = v64;
  return success();
}

/// Decode an inline-int aval payload to i64: `sext((v & mask) >> 1)` from
/// 47 bits (a_int_val in a_runtime.h).
Value emitDecodeInlineInt(OpBuilder &b, Location loc, Value v) {
  mlir::MLIRContext *ctx = loc.getContext();
  Type i64 = i64Ty(ctx);
  Value mask = emitI64Const(b, loc, (std::int64_t)kAvalPayloadMask);
  Value payload = b.create<AndOp>(loc, i64, v, mask);
  Value one = emitI64Const(b, loc, 1);
  Value shifted = b.create<LShrOp>(loc, i64, payload, one);
  // Sign-extend from bit 46 (47-bit signed payload): sext i47 -> i64.
  Type i47 = IntegerType::get(ctx, 47);
  Value narrow = b.create<TruncOp>(loc, i47, shifted);
  return b.create<SExtOp>(loc, i64, narrow);
}

LogicalResult lowerFromAvalInt(mlir::a::AFromAvalOp op, IntegerType intTy,
                               Value &out) {
  OpBuilder b(op);
  Location loc = op->getLoc();
  mlir::MLIRContext *ctx = op->getContext();
  unsigned width = intTy.getWidth();

  if (width > 64) {
    op.emitError("a-lower-typed: cannot unbox aval to i")
        << width << " (wider than i64)";
    return failure();
  }

  AvalCheck chk;
  if (failed(emitAvalTagCheckPrelude(op, op.getValue(), 0, chk,
                                     /*boxedIntPath=*/true)))
    return failure();

  // Decode in chk.decode; the decoded i64 is passed to the merge block
  // through a block argument so it dominates all continuation uses. The
  // merge block has a single i64 argument fed by both predecessors.
  chk.merge->addArgument(i64Ty(ctx), loc);
  {
    OpBuilder db = OpBuilder::atBlockBegin(chk.decode);
    Value decoded = emitDecodeInlineInt(db, loc, chk.aval);
    db.create<BrOp>(loc, ValueRange{decoded}, nullptr, chk.merge);
  }
  {
    // Heap-boxed int (T_INT64): @a_int_full unboxes inline and boxed ints
    // and panics on anything else.
    if (failed(ensureRuntimeI64Decl(op, "a_int_full")))
      return failure();
    OpBuilder sb = OpBuilder::atBlockBegin(chk.slow);
    Value full = emitRuntimeI64CallAt(sb, loc, "a_int_full", chk.aval);
    sb.create<BrOp>(loc, ValueRange{full}, nullptr, chk.merge);
  }

  OpBuilder mb = OpBuilder::atBlockBegin(chk.merge);
  Value v = chk.merge->getArgument(chk.merge->getNumArguments() - 1);
  if (width == 64) {
    out = v;
  } else {
    // `decoded` is the sign-extended 46-bit payload as i64; truncating to
    // the target width yields the correct bits for both signed and
    // unsigned narrow ints (in-range values are non-negative).
    out = mb.create<TruncOp>(loc, intTy, v);
  }
  return success();
}

LogicalResult lowerFromAvalFloat(mlir::a::AFromAvalOp op, Type floatTy,
                                 Value &out) {
  Location loc = op->getLoc();
  mlir::MLIRContext *ctx = op->getContext();

  AvalCheck chk;
  if (failed(emitAvalTagCheckPrelude(op, op.getValue(), 1, chk)))
    return failure();

  OpBuilder db = OpBuilder::atBlockBegin(chk.decode);
  // Untagged aval word IS the f64 bit pattern.
  Value f64v = db.create<BitcastOp>(loc, mlir::Float64Type::get(ctx), chk.aval);
  unsigned width = cast<mlir::FloatType>(floatTy).getWidth();
  Value decoded = f64v;
  if (width == 64) {
    // pass through
  } else if (width < 64) {
    decoded = db.create<FPTruncOp>(loc, floatTy, f64v);
  } else {
    // f80/f128: fpext from f64 (lossy but well-defined; aval only stores
    // f64 payloads anyway).
    decoded = db.create<FPExtOp>(loc, floatTy, f64v);
  }

  // decoded has type floatTy; pass through merge block arg.
  chk.merge->addArgument(floatTy, loc);
  db.create<BrOp>(loc, ValueRange{decoded}, nullptr, chk.merge);

  out = chk.merge->getArgument(chk.merge->getNumArguments() - 1);
  return success();
}

LogicalResult lowerFromAvalBool(mlir::a::AFromAvalOp op, Value &out) {
  OpBuilder b(op);
  Location loc = op->getLoc();
  mlir::MLIRContext *ctx = op->getContext();
  Type i1 = i1Ty(ctx);

  AvalCheck chk;
  if (failed(emitAvalTagCheckPrelude(op, op.getValue(), 2, chk)))
    return failure();

  OpBuilder db = OpBuilder::atBlockBegin(chk.decode);
  // v == A_TRUE ? 1 : 0  (the tag check already restricted to the two
  // constants, so this select is exact).
  Value t = emitI64Const(db, loc, (std::int64_t)kAvalTrue);
  Value isT = emitICmpEq(db, loc, chk.aval, t);
  Value decoded = db.create<SelectOp>(
      loc, i1, isT, db.create<ConstantOp>(loc, i1, 1),
      db.create<ConstantOp>(loc, i1, 0));

  chk.merge->addArgument(i1, loc);
  db.create<BrOp>(loc, ValueRange{decoded}, nullptr, chk.merge);

  out = chk.merge->getArgument(chk.merge->getNumArguments() - 1);
  return success();
}

LogicalResult lowerFromAval(mlir::a::AFromAvalOp op) {
  Value out;
  Type dstTy = op.getResult().getType();
  LogicalResult res = success();
  if (auto intTy = mlir::dyn_cast<IntegerType>(dstTy)) {
    if (intTy.getWidth() == 1) {
      res = lowerFromAvalBool(op, out);
    } else {
      res = lowerFromAvalInt(op, intTy, out);
    }
  } else if (mlir::isa<mlir::FloatType>(dstTy)) {
    res = lowerFromAvalFloat(op, dstTy, out);
  } else {
    op.emitError("a-lower-typed: unsupported a.from_aval result type `")
        << dstTy << "`";
    return failure();
  }
  if (failed(res))
    return res;
  // `out` is a merge-block argument; it dominates all continuation uses.
  op.getResult().replaceAllUsesWith(out);
  op.erase();
  return success();
}

} // namespace

void ALowerTypedPass::getDependentDialects(
    mlir::DialectRegistry &registry) const {
  registry.insert<mlir::LLVM::LLVMDialect>();
  // The `a` dialect is provided by the dialect workstream; register it
  // dynamically so this pass resolves once it lands (see pipeline.hpp).
  registry.insert(mlir::TypeID(), kADialectName,
                  [](mlir::MLIRContext *ctx) -> mlir::Dialect * {
                    registerADialect(*ctx);
                    return ctx->getLoadedDialect(kADialectName);
                  });
}

void ALowerTypedPass::runOnOperation() {
  ModuleOp module = getOperation();

  // Collect `a.to_aval`/`a.from_aval` ops; lowering only erases the op being
  // lowered plus inserts new blocks, so collected pointers stay valid.
  SmallVector<mlir::Operation *> ops;
  module.walk<mlir::WalkOrder::PostOrder>([&](Operation *op) {
    if (mlir::isa<mlir::a::AToAvalOp, mlir::a::AFromAvalOp>(op))
      ops.push_back(op);
    return mlir::WalkResult::advance();
  });

  for (Operation *op : ops) {
    LogicalResult res = success();
    if (auto toAval = mlir::dyn_cast<mlir::a::AToAvalOp>(op)) {
      res = lowerToAval(toAval);
    } else {
      res = lowerFromAval(mlir::cast<mlir::a::AFromAvalOp>(op));
    }
    if (failed(res))
      signalPassFailure();
  }
}

std::unique_ptr<mlir::Pass> createALowerTypedPass() {
  return std::make_unique<ALowerTypedPass>();
}

} // namespace a
