//===- ALowerToLLVM.cpp - `a` dialect -> `llvm` dialect lowering ----*- C++-*-===//
//
// Part of the A project. See docs/mlir-architecture.md.
//
// Phase-1 lowering: reproduces the boxed-i64 semantics emitted by
// src/astra/frontend/mlir/mlir_emit.cpp.
//
// Op shapes (matched against include/A/AOps.td; matched by name so this TU
// compiles before the generated dialect headers exist):
//      a.func          : sym_name + function_type (FunctionType) attrs, one
//                        region; entry block args = params. Lowers to the
//                        boxed ABI `llvm.func @name(%argv: !llvm.ptr) -> i64`
//                        with argv[i+1] unpacking (mirrors mlir_emit.cpp:841).
//      a.call          : callee FlatSymbolRefAttr; operands packed into an
//                        i64 argv array (argv[0] = kAvalNone env slot) and
//                        called via the same ABI.
//      a.runtime.call   : callee StrAttr naming an `a_*` C runtime fn;
//                        direct llvm.call with inferred i64/ptr signature.
//      a.const          : kind enum (int/float/bool/str/none) + ivalue/
//                        fvalue/str payload attrs -> tagged-i64 aval
//                        constant, a_i64n call, bit-cast f64, private
//                        !llvm.array global + a_str_new, or kAvalNone.
//      a.alloca/load/store/gep/addressof : direct llvm.* equivalents.
//      a.return         : Optional<AVAL> operand -> llvm.return i64
//                        (tagged-none when absent).
//      a.fib.spawn/join/main, a.array.get/set : fixed-shape runtime calls.
//      a.if             : legacy sketch op kept for compatibility; the landed
//                        dialect has no a.if (structured flow is a.for/
//                        a.while/a.condition/a.yield -> emitError in phase 1).
//      a.struct.getfield/setfield : with a `field` StrAttr, lowers to
//                        llvm.call @a_getattr/@a_setattr with a private
//                        `__a_cstr<N>` NUL-terminated global for the field
//                        name. Without `field` (index-only) -> emitError:
//                        the runtime has no index-based accessor.
//
// INTEGRATION POINT: ops are matched by name string (lowerOp()). Once the
// generated dialect headers are wired into the build, replace the string
// dispatch with typed accessors (mlir::a::FuncOp etc.) and the `kind` attr
// print-parse in aConstKind() with the generated enum getter.
//
//===----------------------------------------------------------------------===//

#include "ALowerToLLVM.hpp"

#include "../pipeline.hpp" // for a::registerADialect (dynamic dialect reg)

#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlow.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Index/IR/IndexDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/Vector/IR/VectorOps.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/DialectRegistry.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/IR/Visitors.h"
#include "mlir/Pass/Pass.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace a {
namespace {

using mlir::Block;
using mlir::BlockArgument;
using mlir::IntegerType;
using mlir::LLVM::AllocaOp;
using mlir::LLVM::AddOp;
using mlir::LLVM::AddressOfOp;
using mlir::LLVM::BrOp;
using mlir::LLVM::CallOp;
using mlir::LLVM::CondBrOp;
using mlir::LLVM::ConstantOp;
using mlir::LLVM::GEPOp;
using mlir::LLVM::GlobalOp;
using mlir::LLVM::ICmpOp;
using mlir::LLVM::InsertValueOp;
using mlir::LLVM::IntToPtrOp;
using mlir::LLVM::LoadOp;
using mlir::LLVM::LLVMFuncOp;
using mlir::LLVM::LLVMFunctionType;
using mlir::LLVM::LLVMArrayType;
using mlir::LLVM::LLVMFixedVectorType;
using mlir::LLVM::LLVMScalableVectorType;
using mlir::LLVM::LLVMPointerType;
using mlir::LLVM::PtrToIntOp;
using mlir::LLVM::ReturnOp;
using mlir::LLVM::SelectOp;
using mlir::LLVM::StoreOp;
using mlir::LLVM::LLVMVoidType;
using mlir::LLVM::UndefOp;
using mlir::LLVM::ZeroOp;
using mlir::Location;
using mlir::LogicalResult;
using mlir::MLIRContext;
using mlir::ModuleOp;
using mlir::Operation;
using mlir::OperationState;
using mlir::OpBuilder;
using mlir::OpResult;
using mlir::Region;
using mlir::StringAttr;
using mlir::SymbolRefAttr;
using mlir::FlatSymbolRefAttr;
using mlir::Type;
using mlir::Value;
using mlir::success;
using mlir::failure;
using llvm::ArrayRef;
using llvm::SmallVector;
using llvm::SmallVectorImpl;
using mlir::ValueRange;

/// Dialect namespace of the A dialect (see docs/mlir-architecture.md).
constexpr llvm::StringLiteral kADialectName = "a";

//===----------------------------------------------------------------------===//
// Type lowering
//===----------------------------------------------------------------------===//

/// Lower a single type to an LLVM-dialect-compatible type.
/// Returns failure() when the type has no phase-1 lowering; the caller is
/// responsible for emitting the diagnostic.
LogicalResult lowerType(Type type, Type &result) {
  MLIRContext *ctx = type.getContext();

  // Types already in the LLVM dialect pass through unchanged (covers
  // !llvm.ptr/array/vector/struct/function types without enumerating them).
  if (type.getDialect().getNamespace() == "llvm") {
    result = type;
    return success();
  }

  // Signless/semi/sign integers -> signless integer of same width.
  if (auto intTy = llvm::dyn_cast<IntegerType>(type)) {
    result = IntegerType::get(ctx, intTy.getWidth());
    return success();
  }

  // MLIR float types are valid LLVM-dialect types.
  if (llvm::isa<mlir::Float16Type, mlir::Float32Type, mlir::Float64Type,
                mlir::Float80Type, mlir::Float128Type>(type)) {
    result = type;
    return success();
  }

  // index has no LLVM-dialect equivalent; phase-1 aval payload is i64.
  if (llvm::isa<mlir::IndexType>(type)) {
    result = IntegerType::get(ctx, 64);
    return success();
  }

  // `a` dialect (and unknown) types: phase-1 boxed lowering.
  //   !a.ptr            -> !llvm.ptr   (raw pointer payload)
  //   !a.bool           -> i1          (reserved; phase-1 aval payload is i64)
  //   everything else   -> i64         (boxed aval / index payload)
  // The printed spelling is matched on substrings because this pass
  // intentionally avoids the generated dialect headers -- see INTEGRATION
  // POINTS. Once `mlir::a` type classes are available, replace with
  // llvm::isa<mlir::a::APtrType> etc.
  std::string spelling;
  llvm::raw_string_ostream os(spelling);
  type.print(os);
  os.flush();
  const std::string &s = spelling;
  auto contains = [&s](llvm::StringRef sub) { return s.find(sub.str()) != std::string::npos; };

  if (contains("ptr") || contains("pointer")) {
    result = LLVMPointerType::get(ctx);
    return success();
  }
  if (contains("bool")) {
    result = IntegerType::get(ctx, 1);
    return success();
  }
  // !a.aval, !a.struct<...>, !a.array<N>, !a.enum<...>, !a.none, index-like
  // and any other phase-1 boxed value: i64 payload.
  result = IntegerType::get(ctx, 64);
  return success();
}

LogicalResult lowerTypeOrError(Operation *op, Type type, Type &result,
                               llvm::StringRef what) {
  if (succeeded(lowerType(type, result)))
    return success();
  op->emitError("a-lower-to-llvm: cannot lower ")
      << what << " type `" << type << "` to the LLVM dialect";
  return failure();
}

LogicalResult lowerTypes(Operation *op, mlir::TypeRange types,
                         SmallVectorImpl<Type> &result) {
  for (Type t : types) {
    Type lt;
    if (failed(lowerTypeOrError(op, t, lt, "value")))
      return failure();
    result.push_back(lt);
  }
  return success();
}

bool isLLVMPointer(Type t) {
  return llvm::isa<LLVMPointerType>(t);
}

//===----------------------------------------------------------------------===//
// Boxed-aval constants
//
// Mirror src/astra/frontend/mlir/mlir_emit.cpp and
// compiler/astra/runtime/a_runtime.h so the lowered module is bit-identical
// to what the text emitter produces today.
//===----------------------------------------------------------------------===//

constexpr std::int64_t kAvalTag = -281474976710656LL;  // 0xFFFF000000000000
constexpr std::int64_t kInlineLim = INT64_C(1) << 46;
constexpr std::uint64_t kAvalPayloadMask = 0x0000FFFFFFFFFFFFULL;
constexpr std::int64_t kAvalNone = kAvalTag | 0x2;
constexpr std::int64_t kAvalNaNCanon = 9221120237041090560LL;
// Aval bool small constants (a_runtime.h A_TRUE/A_FALSE). Used when an
// unboxed i1/!a.bool crosses the boxed argv ABI boundary.
constexpr std::int64_t kAvalTrue = kAvalTag | 0x6;
constexpr std::int64_t kAvalFalse = kAvalTag | 0x4;

/// Tagged inline-int aval encoding (a_mkint for |v| < 2^46).
std::int64_t avalIntEncode(std::int64_t v) {
  std::uint64_t p = ((std::uint64_t)v << 1) | 1ULL;
  p &= kAvalPayloadMask;
  return (std::int64_t)(kAvalTag | (std::int64_t)p);
}

/// i64 constant op helper.
Value emitI64Const(OpBuilder &b, Location loc, std::int64_t v) {
  MLIRContext *ctx = loc.getContext();
  Type i64Ty = IntegerType::get(ctx, 64);
  return b.create<ConstantOp>(loc, i64Ty,
                              mlir::IntegerAttr::get(i64Ty, v));
}

/// `llvm.getelementptr %base[%idx]` with i64 element/index types.
Value emitGepI64(OpBuilder &b, Location loc, Value base, Value idx) {
  MLIRContext *ctx = loc.getContext();
  Type i64Ty = IntegerType::get(ctx, 64);
  Type ptrTy = LLVMPointerType::get(ctx);
  return b.create<GEPOp>(loc, ptrTy, i64Ty, base, ValueRange{idx});
}

Value emitLoadI64(OpBuilder &b, Location loc, Value addr, unsigned align = 0) {
  MLIRContext *ctx = loc.getContext();
  return b.create<LoadOp>(loc, IntegerType::get(ctx, 64), addr, align);
}

void emitStoreI64(OpBuilder &b, Location loc, Value addr, Value val,
                  unsigned align = 0) {
  b.create<StoreOp>(loc, val, addr, align);
}

// Defined below; used by lowerFunc (bool ABI) and the struct-field cstr
// globals respectively.
Value emitAvalBoolBox(OpBuilder &b, Location loc, Value i1v);
Value emitAvalBoolUnbox(OpBuilder &b, Location loc, Value avalI64);
void emitByteArrayGlobal(ModuleOp module, Location loc,
                         llvm::StringRef gname,
                         llvm::ArrayRef<std::uint8_t> bytes);

//===----------------------------------------------------------------------===//
// Region / block helpers
//===----------------------------------------------------------------------===//

/// Rebuild every block in `region` so that block arguments carry
/// LLVM-lowered types, remapping all intra-region branches. Fills
/// `oldToNew` with old block -> new block. The old blocks are erased.
LogicalResult rebuildRegionWithLoweredArgTypes(
    Region &region, llvm::DenseMap<Block *, Block *> &oldToNew) {
  SmallVector<Block *> oldBlocks;
  oldBlocks.reserve(region.getBlocks().size());
  for (Block &bb : region.getBlocks())
    oldBlocks.push_back(&bb);

  // Pass 1: create new blocks (each inserted right after its old
  // counterpart), move the ops, and re-point block-argument uses.
  for (Block *oldBB : oldBlocks) {
    auto *newBB = new Block;
    newBB->insertAfter(oldBB);

    for (BlockArgument arg : oldBB->getArguments()) {
      Type lt;
      if (failed(lowerType(arg.getType(), lt))) {
        mlir::emitError(region.getLoc(),
                        "a-lower-to-llvm: cannot lower block argument type `")
            << arg.getType() << "`";
        return failure();
      }
      newBB->addArgument(lt, arg.getLoc());
    }

    newBB->getOperations().splice(newBB->end(), oldBB->getOperations());

    const unsigned nArgs = oldBB->getNumArguments();
    for (unsigned i = 0; i < nArgs; ++i)
      oldBB->getArgument(i).replaceAllUsesWith(newBB->getArgument(i));

    oldToNew[oldBB] = newBB;
  }

  // Pass 2: retarget terminator successors at the new blocks in place, then
  // drop the old (now empty) blocks. Rewriting the successor BlockOperand via
  // setSuccessor avoids rebuilding the op: operands (including forwarded
  // successor operands) and attributes are preserved verbatim.
  //
  // Retargeting must complete for EVERY new block before ANY old block is
  // erased: an old block that is still a successor target of a not-yet-
  // retargeted terminator would be freed while a BlockOperand still
  // references it (DenseMap order makes this a coin flip -> heap corruption).
  for (auto &[oldBB, newBB] : oldToNew) {
    for (Operation &op : newBB->getOperations()) {
      if (op.getNumSuccessors() == 0)
        continue;
      if (op.getNumRegions() != 0) {
        op.emitError(
            "a-lower-to-llvm: op with regions and successors inside a lowered "
            "region is not supported");
        return failure();
      }
      for (unsigned i = 0; i < op.getNumSuccessors(); ++i) {
        Block *succ = op.getSuccessor(i);
        auto it = oldToNew.find(succ);
        if (it == oldToNew.end()) {
          op.emitError(
              "a-lower-to-llvm: branch target escapes the lowered region");
          return failure();
        }
        op.setSuccessor(it->second, i);
      }
    }
  }
  for (auto &[oldBB, newBB] : oldToNew) {
    oldBB->erase();
  }
  return success();
}

/// Split `parent` immediately after `op`: ops following `op` (including the
/// parent terminator) move into a fresh block returned in `contBB`.
Block *splitBlockAfter(Operation *op, Block *&contBB) {
  Block *parent = op->getBlock();
  contBB = new Block;
  contBB->insertAfter(parent);
  auto it = std::next(mlir::Block::iterator(op));
  contBB->getOperations().splice(contBB->end(), parent->getOperations(), it,
                                 parent->end());
  return parent;
}

//===----------------------------------------------------------------------===//
// Op lowering helpers
//===----------------------------------------------------------------------===//

/// Extract the callee symbol name from the attributes an `a.call`-family op
/// is expected to carry. See INTEGRATION POINTS for the assumed attr names.
llvm::StringRef getCalleeNameAttr(Operation *op, std::string &storage) {
  for (llvm::StringRef attrName :
       {"callee", "sym_name", "function", "name", "func", "target"}) {
    if (auto sa = op->getAttrOfType<StringAttr>(attrName))
      return sa.getValue();
    if (auto fsra = op->getAttrOfType<FlatSymbolRefAttr>(attrName))
      return fsra.getValue();
    if (auto sra = op->getAttrOfType<SymbolRefAttr>(attrName)) {
      // Nested references "mod.func": use the leaf; cross-module calls are
      // resolved by the emitter before this pass in phase 1.
      storage = sra.getLeafReference().str();
      return storage;
    }
  }
  return {};
}

/// Ensure an `llvm.func` declaration for `name` with the given signature
/// exists in `module`. If a symbol with that name already exists:
///  - llvm.func      -> nothing to do.
///  - a.func         -> it will be lowered later in this pass; the llvm.call
///                      will resolve once it is.
///  - anything else  -> error.
LogicalResult ensureLLVMFuncDecl(ModuleOp module, llvm::StringRef name,
                                 ArrayRef<Type> argTypes,
                                 ArrayRef<Type> resultTypes, Location loc) {
  Operation *sym = mlir::SymbolTable::lookupSymbolIn(module.getOperation(),
                                                   name);
  if (sym) {
    if (sym->getName().getDialect() &&
        sym->getName().getDialect()->getNamespace() == "llvm" &&
        sym->getName().getStringRef() == "llvm.func")
      return success();
    if (sym->getName().getDialect() &&
        sym->getName().getDialect()->getNamespace() == kADialectName &&
        sym->getName().getStringRef() == "a.func")
      return success(); // lowered later in this same pass
    sym->emitError("a-lower-to-llvm: call target `")
        << name << "` is not a function";
    return failure();
  }

  MLIRContext *ctx = module.getContext();
  Type retTy = resultTypes.empty()
                   ? Type(LLVMVoidType::get(ctx))
                   : resultTypes[0];
  if (resultTypes.size() > 1) {
    // LLVM functions have a single result; multi-value calls are not
    // representable in phase 1.
    mlir::emitError(loc, "a-lower-to-llvm: call to `")
        << name << "` has " << resultTypes.size()
        << " results; only 0 or 1 supported";
    return failure();
  }
  LLVMFunctionType fty = LLVMFunctionType::get(retTy, argTypes);

  OpBuilder b(module.getOperation());
  b.setInsertionPointToStart(&module.getBodyRegion().front());
  b.create<LLVMFuncOp>(loc, name, fty);
  return success();
}

/// Ensure `v` can be used as an operand of an llvm op created at `op`:
/// llvm dialect ops require operands defined in the same region. When `v` is
/// defined in an outer region (e.g. a `llvm.mlir.constant` hoisted to module
/// level by canonicalize) and its def is ConstantLike, the def is cloned just
/// before `op`. Other outside-region defs are rejected.
LogicalResult ensureOperandInRegion(Operation *op, Value &v) {
  if (v.getParentRegion() == op->getParentRegion())
    return success();
  auto res = llvm::dyn_cast<OpResult>(v);
  Operation *def = res ? res.getOwner() : nullptr;
  if (!def || !def->hasTrait<mlir::OpTrait::ConstantLike>()) {
    op->emitError("a-lower-to-llvm: operand of type `")
        << v.getType() << "` is defined outside the region and is not a "
           "constant; cannot lower";
    return failure();
  }
  OpBuilder b(op);
  Operation *cloned = b.clone(*def);
  v = cloned->getResult(res.getResultNumber());
  return success();
}

/// Coerce `cond` to i1 for llvm.cond_br: integers wider than i1 are compared
/// against zero; non-integers are passed through (the llvm.cond_br verifier
/// will reject truly invalid types).
Value ensureI1Cond(OpBuilder &b, Location loc, Value cond) {
  Type t = cond.getType();
  if (auto it = llvm::dyn_cast<IntegerType>(t)) {
    if (it.getWidth() == 1)
      return cond;
    Type i1 = IntegerType::get(t.getContext(), 1);
    Value zero = b.create<ConstantOp>(loc, t, b.getIntegerAttr(t, 0));
    return b.create<ICmpOp>(
        loc, i1,
        mlir::LLVM::ICmpPredicateAttr::get(t.getContext(),
                                           mlir::LLVM::ICmpPredicate::ne),
        cond, zero);
  }
  return cond;
}

//===----------------------------------------------------------------------===//
// Individual op lowerings
//===----------------------------------------------------------------------===//

/// Phase-1 A-function ABI (mirrors mlir_emit.cpp:841): every `a.func`
/// lowers to `llvm.func @name(%argv: !llvm.ptr) -> i64`. The body unpacks
/// parameter i from `argv[i+1]` (argv[0] is the env slot) and returns the
/// boxed aval payload as i64.
LogicalResult lowerFunc(Operation *op) {
  ModuleOp module = op->getParentOfType<ModuleOp>();
  if (!module) {
    op->emitError("a-lower-to-llvm: a.func outside of a module");
    return failure();
  }
  if (op->getNumRegions() == 0) {
    op->emitError("a-lower-to-llvm: a.func has no body region");
    return failure();
  }
  Region &srcRegion = op->getRegion(0);

  // Name: `a.func` carries `sym_name` (SymbolNameAttr).
  StringAttr nameAttr;
  for (llvm::StringRef attrName : {"sym_name", "name", "function_name"}) {
    nameAttr = op->getAttrOfType<StringAttr>(attrName);
    if (nameAttr)
      break;
  }
  if (!nameAttr) {
    op->emitError("a-lower-to-llvm: a.func is missing a symbol name attribute");
    return failure();
  }

  MLIRContext *ctx = op->getContext();
  Type i64Ty = IntegerType::get(ctx, 64);
  Type ptrTy = LLVMPointerType::get(ctx);

  // Only the entry block's arguments need lowering (they are the A-level
  // parameters). All other blocks were already lowered by the per-op
  // lowerings (a.for/a.while/etc.) which rebuild their own regions.
  SmallVector<Type> paramTypes;
  if (!srcRegion.empty()) {
    Block &entry = srcRegion.front();
    for (BlockArgument arg : entry.getArguments()) {
      Type lt;
      if (failed(lowerType(arg.getType(), lt))) {
        mlir::emitError(srcRegion.getLoc(),
                        "a-lower-to-llvm: cannot lower block argument type `")
            << arg.getType() << "`";
        return failure();
      }
      paramTypes.push_back(lt);
    }
  }

  // Every llvm.return operand must be the i64 aval payload; a.return was
  // lowered earlier in post-order and emits either the operand or the
  // tagged-none constant.
  for (Block &bb : srcRegion.getBlocks()) {
    for (Operation &inner : bb.getOperations()) {
      if (inner.getName().getStringRef() != "llvm.return")
        continue;
      for (Value v : inner.getOperands()) {
        auto it = llvm::dyn_cast<IntegerType>(v.getType());
        if (!it || it.getWidth() != 64) {
          op->emitError("a-lower-to-llvm: a.func `")
              << nameAttr.getValue()
              << "` returns non-i64 value of type `" << v.getType() << "`";
          return failure();
        }
      }
    }
  }

  // Fixed ABI signature: (ptr) -> i64.
  SmallVector<Type> ftyArgTypes{ptrTy};
  LLVMFunctionType llvmFty = LLVMFunctionType::get(i64Ty, ftyArgTypes);

  OpBuilder b(op);
  LLVMFuncOp newFunc =
      b.create<LLVMFuncOp>(op->getLoc(), nameAttr.getValue(), llvmFty);

  if (srcRegion.empty()) {
    // External declaration: no body to move.
    op->erase();
    return success();
  }

  newFunc->getRegion(0).takeBody(srcRegion);
  Block &entry = newFunc->getRegion(0).front();

  // Insert the argv pointer as the single function parameter, then unpack
  // each A-level parameter from argv[i+1] and erase the placeholder args.
  entry.insertArgument(static_cast<unsigned>(0), ptrTy, op->getLoc());
  Value argv = entry.getArgument(0);

  OpBuilder pb(&entry, entry.begin());
  for (unsigned i = 0; i < paramTypes.size(); ++i) {
    BlockArgument oldArg = entry.getArgument(i + 1);
    Type lt = paramTypes[i];
    Location aloc = oldArg.getLoc();

    Value idx = emitI64Const(pb, aloc, (std::int64_t)i + 1);
    Value slot = emitGepI64(pb, aloc, argv, idx);
    Value raw = emitLoadI64(pb, aloc, slot);

    Value v = raw;
    if (llvm::isa<LLVMPointerType>(lt)) {
      v = pb.create<IntToPtrOp>(aloc, lt, raw);
    } else if (auto it = llvm::dyn_cast<IntegerType>(lt)) {
      if (it.getWidth() == 1) {
        // !a.bool / i1 parameter: the argv slot carries the aval bool word
        // (A_TRUE/A_FALSE); decode to raw i1 for the body.
        v = emitAvalBoolUnbox(pb, aloc, raw);
      } else if (it.getWidth() != 64) {
        op->emitError("a-lower-to-llvm: a.func `")
            << nameAttr.getValue() << "` parameter " << i
            << " lowers to unsupported type `" << lt << "`";
        return failure();
      }
      // i64 aval payloads pass through unchanged.
    } else {
      op->emitError("a-lower-to-llvm: a.func `")
          << nameAttr.getValue() << "` parameter " << i
          << " lowers to unsupported type `" << lt << "`";
      return failure();
    }
    oldArg.replaceAllUsesWith(v);
  }
  if (!paramTypes.empty())
    entry.eraseArguments(1, paramTypes.size());

  op->erase();
  (void)module;
  return success();
}

/// Ensure an `llvm.func` declaration for a runtime/ABI function exists and
/// emit `llvm.call` + result RAUW. `argTypes`/`resultTypes` are the lowered
/// LLVM-level signature; `callArgs` are the already-lowered operand Values.
LogicalResult emitRuntimeCallOp(Operation *op, llvm::StringRef callee,
                                ArrayRef<Type> argTypes,
                                ArrayRef<Type> resultTypes,
                                ArrayRef<Value> callArgs) {
  ModuleOp module = op->getParentOfType<ModuleOp>();
  if (!module) {
    op->emitError("a-lower-to-llvm: call outside of a module");
    return failure();
  }
  if (failed(ensureLLVMFuncDecl(module, callee, argTypes, resultTypes,
                                op->getLoc())))
    return failure();

  // Call operands must be defined in the same region as the llvm.call;
  // re-materialize hoisted constants inside the function.
  SmallVector<Value> regionArgs(callArgs.begin(), callArgs.end());
  for (Value &arg : regionArgs) {
    if (failed(ensureOperandInRegion(op, arg)))
      return failure();
  }

  OpBuilder b(op);
  auto call = b.create<CallOp>(op->getLoc(), resultTypes, callee, regionArgs);
  const unsigned n = std::min<unsigned>(op->getNumResults(), call.getNumResults());
  for (unsigned i = 0; i < n; ++i)
    op->getResult(i).replaceAllUsesWith(call->getResult(i));
  op->erase();
  return success();
}

/// Box a raw i1 bool as the aval bool word (A_TRUE/A_FALSE) for an argv
/// slot. `!a.bool`/`i1` values cross the boxed ABI in this tagged form so
/// they round-trip through a.to_aval/a.from_aval unchanged.
Value emitAvalBoolBox(OpBuilder &b, Location loc, Value i1v) {
  MLIRContext *ctx = loc.getContext();
  Type i64Ty = IntegerType::get(ctx, 64);
  Value t = b.create<ConstantOp>(loc, i64Ty,
                                 mlir::IntegerAttr::get(i64Ty, kAvalTrue));
  Value f = b.create<ConstantOp>(loc, i64Ty,
                                 mlir::IntegerAttr::get(i64Ty, kAvalFalse));
  return b.create<SelectOp>(loc, i64Ty, i1v, t, f);
}

/// Decode an aval bool word (i64) back to raw i1: true iff it equals
/// A_TRUE (A_FALSE and any other word decode to false; callers that need a
/// trap use a.from_aval instead).
Value emitAvalBoolUnbox(OpBuilder &b, Location loc, Value avalI64) {
  MLIRContext *ctx = loc.getContext();
  Type i1Ty = IntegerType::get(ctx, 1);
  Type i64Ty = IntegerType::get(ctx, 64);
  Value t = b.create<ConstantOp>(loc, i64Ty,
                                 mlir::IntegerAttr::get(i64Ty, kAvalTrue));
  return b.create<ICmpOp>(
      loc, i1Ty,
      mlir::LLVM::ICmpPredicateAttr::get(ctx, mlir::LLVM::ICmpPredicate::eq),
      avalI64, t);
}

/// Lower an operand value to the i64 aval payload expected inside an argv
/// slot: i64 passes through, pointers are ptrtoint'd, i1 is boxed as the
/// aval bool word, anything else is an error (phase-1 boxed values are
/// always i64).
LogicalResult coerceToI64Aval(Operation *op, Value v, Value &out) {
  if (failed(ensureOperandInRegion(op, v)))
    return failure();
  Type t = v.getType();
  if (auto it = llvm::dyn_cast<IntegerType>(t)) {
    if (it.getWidth() == 64) {
      out = v;
      return success();
    }
    if (it.getWidth() == 1) {
      // !a.bool / i1 param or argument: box as the tagged aval bool word so
      // the callee's i1 unpack (icmp eq A_TRUE) recovers it exactly.
      OpBuilder b(op);
      out = emitAvalBoolBox(b, op->getLoc(), v);
      return success();
    }
  } else if (llvm::isa<LLVMPointerType>(t)) {
    OpBuilder b(op);
    out = b.create<PtrToIntOp>(op->getLoc(), IntegerType::get(op->getContext(), 64), v);
    return success();
  }
  op->emitError("a-lower-to-llvm: cannot pack call argument of type `")
      << t << "` into the i64 argv array";
  return failure();
}

/// `a.call` -> argv-array ABI call to a lowered `a.func`:
///   %argv = llvm.alloca (n+1) x i64
///   argv[0] = kAvalNone (env slot)
///   argv[i+1] = arg_i
///   %r = llvm.call @callee(%argv) : (!llvm.ptr) -> i64
LogicalResult lowerACall(Operation *op) {
  std::string nameStorage;
  llvm::StringRef callee = getCalleeNameAttr(op, nameStorage);
  if (callee.empty()) {
    op->emitError("a-lower-to-llvm: a.call has no `callee` attribute");
    return failure();
  }

  MLIRContext *ctx = op->getContext();
  Type i64Ty = IntegerType::get(ctx, 64);
  Type ptrTy = LLVMPointerType::get(ctx);
  Location loc = op->getLoc();

  // Callee declaration uses the fixed A-function ABI.
  SmallVector<Type> declArgTypes{ptrTy};
  SmallVector<Type> declResultTypes{i64Ty};
  ModuleOp module = op->getParentOfType<ModuleOp>();
  if (!module) {
    op->emitError("a-lower-to-llvm: a.call outside of a module");
    return failure();
  }
  if (failed(ensureLLVMFuncDecl(module, callee, declArgTypes, declResultTypes,
                                loc)))
    return failure();

  const unsigned nArgs = op->getNumOperands();
  OpBuilder b(op);

  // Allocate and initialize the argv array.
  Value size = emitI64Const(b, loc, (std::int64_t)nArgs + 1);
  Value argv = b.create<AllocaOp>(loc, ptrTy, i64Ty, size, 0);
  Value slot0 = emitGepI64(b, loc, argv, emitI64Const(b, loc, 0));
  emitStoreI64(b, loc, slot0, emitI64Const(b, loc, kAvalNone));
  for (unsigned i = 0; i < nArgs; ++i) {
    Value packed;
    if (failed(coerceToI64Aval(op, op->getOperand(i), packed)))
      return failure();
    Value slot = emitGepI64(b, loc, argv, emitI64Const(b, loc, (std::int64_t)i + 1));
    emitStoreI64(b, loc, slot, packed);
  }

  auto call = b.create<CallOp>(loc, declResultTypes, callee,
                               SmallVector<Value>{argv});

  // Results: phase-1 A functions return at most one boxed aval (i64). A
  // pointer-typed result is inttoptr'd back to !llvm.ptr.
  if (op->getNumResults() > 1) {
    op->emitError("a-lower-to-llvm: a.call `")
        << callee << "` has " << op->getNumResults()
        << " results; only 0 or 1 supported in phase 1";
    return failure();
  }
  if (op->getNumResults() == 1) {
    Type lt;
    if (failed(lowerTypeOrError(op, op->getResult(0).getType(), lt,
                                "call result")))
      return failure();
    Value r = call->getResult(0);
    if (llvm::isa<LLVMPointerType>(lt)) {
      r = b.create<IntToPtrOp>(loc, lt, r);
    } else {
      auto it = llvm::dyn_cast<IntegerType>(lt);
      if (it && it.getWidth() == 1) {
        // !a.bool / i1 result: decode the aval bool word to raw i1.
        r = emitAvalBoolUnbox(b, loc, r);
      } else if (!it || it.getWidth() != 64) {
        op->emitError("a-lower-to-llvm: a.call `")
            << callee << "` result lowers to unsupported type `" << lt << "`";
        return failure();
      }
    }
    op->getResult(0).replaceAllUsesWith(r);
  }
  op->erase();
  return success();
}

/// `a.runtime.call` -> direct `llvm.call` to an `a_*` C runtime function.
/// The signature is inferred from the lowered operand/result types (the
/// optional `signature` attr is documentation for the verifier; phase-1
/// runtime functions follow the i64/!llvm.ptr ABI).
LogicalResult lowerRuntimeCall(Operation *op) {
  std::string nameStorage;
  llvm::StringRef callee = getCalleeNameAttr(op, nameStorage);
  if (callee.empty()) {
    op->emitError("a-lower-to-llvm: a.runtime.call has no `callee` attribute");
    return failure();
  }

  SmallVector<Type> loweredResults;
  if (failed(lowerTypes(op, op->getResultTypes(), loweredResults)))
    return failure();
  SmallVector<Type> loweredArgs;
  for (Value arg : op->getOperands()) {
    Type lt;
    if (failed(lowerTypeOrError(op, arg.getType(), lt, "call argument")))
      return failure();
    loweredArgs.push_back(lt);
  }

  // Operand/result types are already LLVM types at this point (producers
  // were lowered first in post-order); pass them through unchanged.
  SmallVector<Value> callArgs(op->getOperands().begin(), op->getOperands().end());
  return emitRuntimeCallOp(op, callee, loweredArgs, loweredResults, callArgs);
}

//===----------------------------------------------------------------------===//
// a.fib.* / a.array.* -> fixed runtime calls
//===----------------------------------------------------------------------===//

/// fib.spawn(fnval:i64, nargs:i64, args:i64-ptr-payload) ->
///   a_fib_spawn_handle(i64, i64, !llvm.ptr) -> i64
LogicalResult lowerFibSpawn(Operation *op) {
  MLIRContext *ctx = op->getContext();
  Type i64Ty = IntegerType::get(ctx, 64);
  Type ptrTy = LLVMPointerType::get(ctx);
  if (op->getNumOperands() != 3 || op->getNumResults() != 1) {
    op->emitError("a-lower-to-llvm: malformed a.fib.spawn");
    return failure();
  }
  OpBuilder b(op);
  Value argsPtr = op->getOperand(2);
  if (llvm::isa<IntegerType>(argsPtr.getType())) {
    argsPtr = b.create<IntToPtrOp>(op->getLoc(), ptrTy, argsPtr);
  } else if (!llvm::isa<LLVMPointerType>(argsPtr.getType())) {
    op->emitError("a-lower-to-llvm: a.fib.spawn args operand has type `")
        << argsPtr.getType() << "`";
    return failure();
  }
  SmallVector<Type> argTypes{i64Ty, i64Ty, ptrTy};
  SmallVector<Type> resTypes{i64Ty};
  SmallVector<Value> callArgs{op->getOperand(0), op->getOperand(1), argsPtr};
  return emitRuntimeCallOp(op, "a_fib_spawn_handle", argTypes, resTypes,
                           callArgs);
}

/// fib.join(handle:i64) -> a_fib_join_handle(i64) -> i64
LogicalResult lowerFibJoin(Operation *op) {
  MLIRContext *ctx = op->getContext();
  Type i64Ty = IntegerType::get(ctx, 64);
  if (op->getNumOperands() != 1 || op->getNumResults() != 1) {
    op->emitError("a-lower-to-llvm: malformed a.fib.join");
    return failure();
  }
  SmallVector<Type> argTypes{i64Ty};
  SmallVector<Type> resTypes{i64Ty};
  SmallVector<Value> callArgs{op->getOperand(0)};
  return emitRuntimeCallOp(op, "a_fib_join_handle", argTypes, resTypes,
                           callArgs);
}

/// fib.main(fnptr:i64, argc:i64, argv:!a.ptr, env:i64) ->
///   a_fib_main(i64 entry, i64 env, i64 n, !llvm.ptr args) -> i64
/// Note the C runtime parameter order (entry, env, n, args) differs from the
/// op's operand order (fnptr, argc, argv, env).
LogicalResult lowerFibMain(Operation *op) {
  MLIRContext *ctx = op->getContext();
  Type i64Ty = IntegerType::get(ctx, 64);
  Type ptrTy = LLVMPointerType::get(ctx);
  if (op->getNumOperands() != 4 || op->getNumResults() != 1) {
    op->emitError("a-lower-to-llvm: malformed a.fib.main");
    return failure();
  }
  SmallVector<Type> argTypes{i64Ty, i64Ty, i64Ty, ptrTy};
  SmallVector<Type> resTypes{i64Ty};
  SmallVector<Value> callArgs{op->getOperand(0), op->getOperand(3),
                              op->getOperand(1), op->getOperand(2)};
  return emitRuntimeCallOp(op, "a_fib_main", argTypes, resTypes, callArgs);
}

/// array.get(array:i64, index:i64) -> a_getitem(i64, i64) -> i64
LogicalResult lowerArrayGet(Operation *op) {
  MLIRContext *ctx = op->getContext();
  Type i64Ty = IntegerType::get(ctx, 64);
  if (op->getNumOperands() != 2 || op->getNumResults() != 1) {
    op->emitError("a-lower-to-llvm: malformed a.array.get");
    return failure();
  }
  SmallVector<Type> argTypes{i64Ty, i64Ty};
  SmallVector<Type> resTypes{i64Ty};
  SmallVector<Value> callArgs{op->getOperand(0), op->getOperand(1)};
  return emitRuntimeCallOp(op, "a_getitem", argTypes, resTypes, callArgs);
}

/// array.set(array:i64, index:i64, value:i64) -> a_setitem(i64,i64,i64)->i64
LogicalResult lowerArraySet(Operation *op) {
  MLIRContext *ctx = op->getContext();
  Type i64Ty = IntegerType::get(ctx, 64);
  if (op->getNumOperands() != 3 || op->getNumResults() != 1) {
    op->emitError("a-lower-to-llvm: malformed a.array.set");
    return failure();
  }
  SmallVector<Type> argTypes{i64Ty, i64Ty, i64Ty};
  SmallVector<Type> resTypes{i64Ty};
  SmallVector<Value> callArgs{op->getOperand(0), op->getOperand(1),
                              op->getOperand(2)};
  return emitRuntimeCallOp(op, "a_setitem", argTypes, resTypes, callArgs);
}

//===----------------------------------------------------------------------===//
// a.struct.getfield / a.struct.setfield -> a_getattr / a_setattr
//
// The ops carry an optional `field` StrAttr naming the field. When present we
// lower to the runtime calls
//   a_getattr(i64 obj, !llvm.ptr name) -> i64
//   a_setattr(i64 obj, !llvm.ptr name, i64 v) -> i64
// where `name` is a private NUL-terminated `!llvm.array<N x i8>` global
// (`__a_cstr<N>`, deduplicated by field-name string). When `field` is absent
// the op is index-only and stays unsupported: the runtime has no
// index-based accessor and !a.struct<"Name"> carries no field table.
//===----------------------------------------------------------------------===//

/// Cache mapping field-name text -> `__a_cstr<N>` global symbol, so repeated
/// accesses to the same field share one private global per module run.
using CStrGlobalCache = llvm::StringMap<std::string>;

/// Return the symbol name of a private NUL-terminated cstr global holding
/// `text`, creating it at module top level on first use.
std::string getOrCreateCStrGlobal(Operation *op, llvm::StringRef text,
                                  CStrGlobalCache &cache) {
  auto it = cache.find(text);
  if (it != cache.end())
    return it->second;

  ModuleOp module = op->getParentOfType<ModuleOp>();
  Location loc = op->getLoc();

  static std::atomic<unsigned> cstrGlobalCounter{0};
  std::string gname;
  do {
    gname = "__a_cstr" + std::to_string(cstrGlobalCounter.fetch_add(1));
  } while (mlir::SymbolTable::lookupSymbolIn(module.getOperation(), gname));

  std::vector<std::uint8_t> bytes(text.size() + 1);
  for (std::size_t i = 0; i < text.size(); ++i)
    bytes[i] = (std::uint8_t)text[i];
  bytes[text.size()] = 0;
  emitByteArrayGlobal(module, loc, gname, bytes);

  cache[text] = gname;
  return gname;
}

/// Read the optional `field` StrAttr; emits an error and returns false when
/// absent or empty (index-only access is unsupported).
bool requireFieldAttr(Operation *op, llvm::StringRef &field) {
  auto sa = op->getAttrOfType<StringAttr>("field");
  if (!sa || sa.getValue().empty()) {
    op->emitError("a-lower-to-llvm: `")
        << op->getName().getStringRef()
        << "` requires a non-empty `field` attribute; index-only struct "
           "access is unsupported (the runtime takes a field name)";
    return false;
  }
  field = sa.getValue();
  return true;
}

/// Shared lowering for a.struct.getfield/setfield. `isSet` selects the
/// a_setattr 3-arg call; otherwise a_getattr 2-arg.
LogicalResult lowerStructField(Operation *op, CStrGlobalCache &cstrs,
                               bool isSet) {
  llvm::StringRef field;
  if (!requireFieldAttr(op, field))
    return failure();

  MLIRContext *ctx = op->getContext();
  Type i64Ty = IntegerType::get(ctx, 64);
  Type ptrTy = LLVMPointerType::get(ctx);
  Location loc = op->getLoc();
  ModuleOp module = op->getParentOfType<ModuleOp>();
  if (!module) {
    op->emitError("a-lower-to-llvm: struct field access outside of a module");
    return failure();
  }

  // Operand sanity: getfield(obj,index), setfield(obj,index,value). The
  // runtime keys on the field name, so `index` is carried but unused.
  const unsigned nOps = isSet ? 3 : 2;
  if (op->getNumOperands() != nOps || op->getNumResults() != 1) {
    op->emitError("a-lower-to-llvm: malformed `")
        << op->getName().getStringRef() << "`";
    return failure();
  }

  // Result must be the boxed i64 aval payload.
  Type resTy;
  if (failed(lowerTypeOrError(op, op->getResult(0).getType(), resTy,
                              "struct field result")))
    return failure();
  auto resInt = llvm::dyn_cast<IntegerType>(resTy);
  if (!resInt || resInt.getWidth() != 64) {
    op->emitError("a-lower-to-llvm: `")
        << op->getName().getStringRef()
        << "` result lowers to `" << resTy << "`; expected i64 aval payload";
    return failure();
  }

  // Pack operands into the i64 aval ABI (obj always; value for setfield).
  Value obj;
  if (failed(coerceToI64Aval(op, op->getOperand(0), obj)))
    return failure();

  OpBuilder b(op);
  std::string gname = getOrCreateCStrGlobal(op, field, cstrs);
  Value namePtr = b.create<AddressOfOp>(loc, ptrTy, gname);

  SmallVector<Type> resTypes{i64Ty};
  SmallVector<Value> callArgs;
  if (isSet) {
    Value val;
    if (failed(coerceToI64Aval(op, op->getOperand(2), val)))
      return failure();
    if (failed(ensureLLVMFuncDecl(module, "a_setattr",
                                  SmallVector<Type>{i64Ty, ptrTy, i64Ty},
                                  resTypes, loc)))
      return failure();
    callArgs = {obj, namePtr, val};
    auto call = b.create<CallOp>(loc, resTypes, "a_setattr", callArgs);
    op->getResult(0).replaceAllUsesWith(call->getResult(0));
  } else {
    if (failed(ensureLLVMFuncDecl(module, "a_getattr",
                                  SmallVector<Type>{i64Ty, ptrTy}, resTypes,
                                  loc)))
      return failure();
    callArgs = {obj, namePtr};
    auto call = b.create<CallOp>(loc, resTypes, "a_getattr", callArgs);
    op->getResult(0).replaceAllUsesWith(call->getResult(0));
  }
  op->erase();
  return success();
}

LogicalResult lowerStructGetField(Operation *op, CStrGlobalCache &cstrs) {
  return lowerStructField(op, cstrs, /*isSet=*/false);
}

LogicalResult lowerStructSetField(Operation *op, CStrGlobalCache &cstrs) {
  return lowerStructField(op, cstrs, /*isSet=*/true);
}

//===----------------------------------------------------------------------===//
// a.gep / a.addressof
//===----------------------------------------------------------------------===//

/// gep(ptr:!a.ptr, index:i64) -> llvm.getelementptr ptr[idx] : i64 elements.
LogicalResult lowerGep(Operation *op) {
  if (op->getNumOperands() != 2 || op->getNumResults() != 1) {
    op->emitError("a-lower-to-llvm: malformed a.gep");
    return failure();
  }
  Type resTy;
  if (failed(lowerTypeOrError(op, op->getResult(0).getType(), resTy,
                              "gep result")))
    return failure();
  OpBuilder b(op);
  Location loc = op->getLoc();
  Value ptrOp = op->getOperand(0);
  Value idxOp = op->getOperand(1);
  if (failed(ensureOperandInRegion(op, ptrOp)) ||
      failed(ensureOperandInRegion(op, idxOp)))
    return failure();
  Value gep = emitGepI64(b, loc, ptrOp, idxOp);
  if (gep.getType() != resTy) {
    // resTy should be !llvm.ptr; if the result type lowered differently,
    // keep the gep result anyway only when types match.
    op->emitError("a-lower-to-llvm: a.gep result type `")
        << resTy << "` does not match lowered pointer type";
    return failure();
  }
  op->getResult(0).replaceAllUsesWith(gep);
  op->erase();
  return success();
}

/// addressof(@sym) -> llvm.mlir.addressof @sym : !llvm.ptr
LogicalResult lowerAddressof(Operation *op) {
  if (op->getNumResults() != 1) {
    op->emitError("a-lower-to-llvm: malformed a.addressof");
    return failure();
  }
  auto fsra = op->getAttrOfType<FlatSymbolRefAttr>("sym_name");
  if (!fsra) {
    op->emitError("a-lower-to-llvm: a.addressof missing `sym_name` attribute");
    return failure();
  }
  Type resTy;
  if (failed(lowerTypeOrError(op, op->getResult(0).getType(), resTy,
                              "addressof result")))
    return failure();
  OpBuilder b(op);
  auto addr = b.create<AddressOfOp>(op->getLoc(), resTy, fsra.getValue());
  op->getResult(0).replaceAllUsesWith(addr);
  op->erase();
  return success();
}

/// Read the `kind` enum attr of `a.const`. The attr is an `a`-dialect
/// EnumAttr (`#a.kind<int>` etc.); without the generated dialect headers we
/// recover the case by printing the attribute and matching the case name.
/// Returns 0=int,1=float,2=bool,3=str,4=none, or -1 when unreadable.
int aConstKind(Operation *op) {
  mlir::Attribute kindAttr = op->getAttr("kind");
  if (!kindAttr)
    return -1;
  if (auto ia = llvm::dyn_cast<mlir::IntegerAttr>(kindAttr))
    return (int)ia.getInt();
  std::string text;
  llvm::raw_string_ostream os(text);
  kindAttr.print(os);
  os.flush();
  // Order matters: "none"/"str"/"bool"/"float" are unambiguous; "int" is a
  // substring of nothing else that can appear here.
  if (text.find("none") != std::string::npos) return 4;
  if (text.find("str") != std::string::npos) return 3;
  if (text.find("bool") != std::string::npos) return 2;
  if (text.find("float") != std::string::npos) return 1;
  if (text.find("int") != std::string::npos) return 0;
  return -1;
}

/// Emit `llvm.mlir.constant` of an i64 aval payload.
Value emitAvalConst(OpBuilder &b, Location loc, std::int64_t avalBits) {
  MLIRContext *ctx = loc.getContext();
  Type i64Ty = IntegerType::get(ctx, 64);
  return b.create<ConstantOp>(loc, i64Ty,
                              mlir::IntegerAttr::get(i64Ty, avalBits));
}

/// Emit a call to `a_i64n(i64) -> i64` for out-of-inline-range ints.
LogicalResult emitI64NCall(Operation *op, Value v, Value &out) {
  ModuleOp module = op->getParentOfType<ModuleOp>();
  if (!module) {
    op->emitError("a-lower-to-llvm: a.const outside of a module");
    return failure();
  }
  MLIRContext *ctx = op->getContext();
  Type i64Ty = IntegerType::get(ctx, 64);
  SmallVector<Type> argTypes{i64Ty};
  SmallVector<Type> resTypes{i64Ty};
  SmallVector<Value> callArgs{v};
  // Reuse emitRuntimeCallOp machinery minus the RAUW/erase: inline it here.
  if (failed(ensureLLVMFuncDecl(module, "a_i64n", argTypes, resTypes,
                                op->getLoc())))
    return failure();
  OpBuilder b(op);
  auto call = b.create<CallOp>(op->getLoc(), resTypes, "a_i64n", callArgs);
  out = call->getResult(0);
  return success();
}

/// `a.const "str" {str = "..."}` -> private `!llvm.array<len+1 x i8>` global
/// + `llvm.mlir.addressof` + `a_str_new(ptr, len)`.
/// Emit a private `!llvm.array<N x i8>` global at module top level holding
/// `bytes` verbatim (caller includes any trailing NUL). The initializer is a
/// region (undef + llvm.insertvalue chain + llvm.return), matching what
/// mlir_emit.cpp prints for string/cstr literals.
void emitByteArrayGlobal(ModuleOp module, Location loc, llvm::StringRef gname,
                         llvm::ArrayRef<std::uint8_t> bytes) {
  MLIRContext *ctx = module.getContext();
  Type i8Ty = IntegerType::get(ctx, 8);
  Type arrTy = LLVMArrayType::get(ctx, i8Ty, (int64_t)bytes.size());
  OpBuilder mb(module.getOperation());
  mb.setInsertionPointToStart(&module.getBodyRegion().front());
  auto global = mb.create<GlobalOp>(loc, arrTy, /*isConstant=*/true,
                                    mlir::LLVM::Linkage::Private, gname,
                                    mlir::Attribute());
  Region &init = global.getInitializer();
  Block *bb = init.empty() ? nullptr : &init.front();
  if (!bb) {
    bb = new Block;
    init.push_back(bb);
  }
  // Fresh initializer: drop any ops the builder pre-populated (e.g. an
  // implicit terminator) before emitting ours.
  bb->getOperations().clear();
  OpBuilder ib = OpBuilder::atBlockEnd(bb);
  Value acc = ib.create<UndefOp>(loc, arrTy);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    Value cv = ib.create<ConstantOp>(loc, i8Ty,
                                     mlir::IntegerAttr::get(i8Ty, bytes[i]));
    acc = ib.create<InsertValueOp>(loc, arrTy, acc, cv,
                                   llvm::ArrayRef<int64_t>{(int64_t)i});
  }
  ib.create<ReturnOp>(loc, ValueRange{acc});
}

///
/// The global initializer is a region (undef + llvm.insertvalue chain +
/// llvm.return), matching what mlir_emit.cpp prints for string literals.
LogicalResult lowerConstStr(Operation *op, llvm::StringRef text,
                            Value &out) {
  ModuleOp module = op->getParentOfType<ModuleOp>();
  if (!module) {
    op->emitError("a-lower-to-llvm: a.const outside of a module");
    return failure();
  }
  MLIRContext *ctx = op->getContext();
  Location loc = op->getLoc();
  Type i64Ty = IntegerType::get(ctx, 64);
  Type ptrTy = LLVMPointerType::get(ctx);
  const std::size_t len = text.size();
  const std::size_t total = len + 1;  // trailing NUL

  // Unique private global name.
  static std::atomic<unsigned> strGlobalCounter{0};
  std::string gname;
  do {
    gname = "__a_str" + std::to_string(strGlobalCounter.fetch_add(1));
  } while (mlir::SymbolTable::lookupSymbolIn(module.getOperation(), gname));

  // Emit the global at module top level.
  {
    std::vector<std::uint8_t> bytes(total);
    for (std::size_t i = 0; i < len; ++i)
      bytes[i] = (std::uint8_t)text[i];
    bytes[len] = 0;
    emitByteArrayGlobal(module, loc, gname, bytes);
  }

  // In the function: addressof + a_str_new(ptr, len).
  OpBuilder b(op);
  Value addr = b.create<AddressOfOp>(loc, ptrTy, gname);
  SmallVector<Type> argTypes{ptrTy, i64Ty};
  SmallVector<Type> resTypes{i64Ty};
  SmallVector<Value> callArgs{addr, emitI64Const(b, loc, (std::int64_t)len)};
  if (failed(ensureLLVMFuncDecl(module, "a_str_new", argTypes, resTypes, loc)))
    return failure();
  auto call = b.create<CallOp>(loc, resTypes, "a_str_new", callArgs);
  out = call->getResult(0);
  return success();
}

LogicalResult lowerConst(Operation *op) {
  if (op->getNumResults() != 1) {
    op->emitError("a-lower-to-llvm: a.const must have exactly one result");
    return failure();
  }
  Type resTy;
  if (failed(lowerTypeOrError(op, op->getResult(0).getType(), resTy,
                              "constant")))
    return failure();
  auto resInt = llvm::dyn_cast<IntegerType>(resTy);
  if (!resInt || resInt.getWidth() != 64) {
    op->emitError("a-lower-to-llvm: a.const result lowers to `")
        << resTy << "`; expected i64 aval payload";
    return failure();
  }

  int kind = aConstKind(op);
  if (kind < 0) {
    op->emitError("a-lower-to-llvm: a.const has missing/unknown `kind` attr");
    return failure();
  }

  OpBuilder b(op);
  Location loc = op->getLoc();
  Value result;

  switch (kind) {
  case 4: {  // none -> tagged-none aval
    result = emitAvalConst(b, loc, kAvalNone);
    break;
  }
  case 0:   // int
  case 2: { // bool: boxed as tagged int per phase-1 ABI
    auto iv = op->getAttrOfType<mlir::IntegerAttr>("ivalue");
    if (!iv) {
      op->emitError("a-lower-to-llvm: a.const kind int/bool requires `ivalue`");
      return failure();
    }
    std::int64_t v = iv.getInt();
    if (v >= -kInlineLim && v < kInlineLim) {
      result = emitAvalConst(b, loc, avalIntEncode(v));
    } else {
      Value raw = emitI64Const(b, loc, v);
      if (failed(emitI64NCall(op, raw, result)))
        return failure();
    }
    break;
  }
  case 1: {  // float: f64 bit pattern as i64 aval (NaN canonicalized)
    auto fv = op->getAttrOfType<mlir::FloatAttr>("fvalue");
    if (!fv) {
      op->emitError("a-lower-to-llvm: a.const kind float requires `fvalue`");
      return failure();
    }
    double d = fv.getValueAsDouble();
    std::int64_t bits;
    if (d != d) {
      bits = kAvalNaNCanon;
    } else {
      std::uint64_t u;
      std::memcpy(&u, &d, 8);
      bits = (std::int64_t)u;
    }
    result = emitAvalConst(b, loc, bits);
    break;
  }
  case 3: {  // str -> private global + a_str_new
    auto sv = op->getAttrOfType<mlir::StringAttr>("str");
    if (!sv) {
      op->emitError("a-lower-to-llvm: a.const kind str requires `str` attr");
      return failure();
    }
    if (failed(lowerConstStr(op, sv.getValue(), result)))
      return failure();
    break;
  }
  default:
    op->emitError("a-lower-to-llvm: unknown a.const kind ") << kind;
    return failure();
  }

  op->getResult(0).replaceAllUsesWith(result);
  op->erase();
  return success();
}

unsigned getUnsignedAttrOrZero(Operation *op, llvm::StringRef name) {
  if (auto ia = op->getAttrOfType<mlir::IntegerAttr>(name))
    return static_cast<unsigned>(ia.getInt());
  return 0;
}

LogicalResult lowerAlloca(Operation *op) {
  if (op->getNumResults() != 1) {
    op->emitError("a-lower-to-llvm: a.alloca must have exactly one result");
    return failure();
  }
  Type resTy;
  if (failed(lowerTypeOrError(op, op->getResult(0).getType(), resTy,
                              "alloca result")))
    return failure();

  MLIRContext *ctx = op->getContext();
  // Element type: explicit attr wins, else default to the i64 aval payload.
  Type elemTy = IntegerType::get(ctx, 64);
  for (llvm::StringRef attrName : {"element_type", "elem_type", "type"}) {
    if (auto ta = op->getAttrOfType<mlir::TypeAttr>(attrName)) {
      Type lt;
      if (failed(lowerTypeOrError(op, ta.getValue(), lt, "alloca element")))
        return failure();
      elemTy = lt;
      break;
    }
  }

  OpBuilder b(op);
  Location loc = op->getLoc();
  Value size;
  if (!op->getOperands().empty()) {
    size = op->getOperand(0);
    // llvm.alloca requires operands defined in the same region.
    if (failed(ensureOperandInRegion(op, size)))
      return failure();
  } else {
    size = b.create<ConstantOp>(loc, IntegerType::get(ctx, 64),
                                b.getI64IntegerAttr(1));
  }
  unsigned align = getUnsignedAttrOrZero(op, "alignment");
  Value slot = b.create<AllocaOp>(loc, resTy, elemTy, size, align);
  op->getResult(0).replaceAllUsesWith(slot);
  op->erase();
  return success();
}

LogicalResult lowerLoad(Operation *op) {
  if (op->getNumResults() != 1 || op->getNumOperands() < 1) {
    op->emitError("a-lower-to-llvm: malformed a.load");
    return failure();
  }
  Type resTy;
  if (failed(lowerTypeOrError(op, op->getResult(0).getType(), resTy,
                              "load result")))
    return failure();
  OpBuilder b(op);
  Value ptrOp = op->getOperand(0);
  if (failed(ensureOperandInRegion(op, ptrOp)))
    return failure();
  Value loaded = b.create<LoadOp>(op->getLoc(), resTy, ptrOp,
                                  getUnsignedAttrOrZero(op, "alignment"));
  op->getResult(0).replaceAllUsesWith(loaded);
  op->erase();
  return success();
}

LogicalResult lowerStore(Operation *op) {
  if (op->getNumOperands() < 2) {
    op->emitError("a-lower-to-llvm: malformed a.store");
    return failure();
  }
  // Operand order is (value, addr) like llvm.store; detect a swapped
  // (addr, value) layout by which operand lowers to a pointer.
  Value v0 = op->getOperand(0);
  Value v1 = op->getOperand(1);
  Type t0, t1;
  bool p0 = succeeded(lowerType(v0.getType(), t0)) && isLLVMPointer(t0);
  bool p1 = succeeded(lowerType(v1.getType(), t1)) && isLLVMPointer(t1);
  Value value = v0, addr = v1;
  if (p0 && !p1) {
    value = v1;
    addr = v0;
  }
  if (failed(ensureOperandInRegion(op, value)) ||
      failed(ensureOperandInRegion(op, addr)))
    return failure();
  OpBuilder b(op);
  b.create<StoreOp>(op->getLoc(), value, addr,
                        getUnsignedAttrOrZero(op, "alignment"));
  op->erase();
  return success();
}

LogicalResult lowerReturn(Operation *op) {
  // a.return: Optional<AVAL> operand. No operand -> tagged-none aval;
  // one operand -> its i64 payload. The enclosing llvm.func returns i64.
  OpBuilder b(op);
  Location loc = op->getLoc();

  if (op->getNumOperands() == 0) {
    Value none = emitI64Const(b, loc, kAvalNone);
    b.create<ReturnOp>(loc, ValueRange{none});
  } else if (op->getNumOperands() == 1) {
    Value v = op->getOperand(0);
    if (failed(ensureOperandInRegion(op, v)))
      return failure();
    auto it = llvm::dyn_cast<IntegerType>(v.getType());
    if (it && it.getWidth() == 1) {
      // !a.bool / i1 return value: box as the aval bool word so the i64
      // llvm.func result stays a valid aval payload.
      v = emitAvalBoolBox(b, loc, v);
    } else if (!it || it.getWidth() != 64) {
      op->emitError("a-lower-to-llvm: a.return operand has type `")
          << v.getType() << "`; expected i64 aval payload";
      return failure();
    }
    b.create<ReturnOp>(loc, ValueRange{v});
  } else {
    op->emitError("a-lower-to-llvm: a.return with ")
        << op->getNumOperands() << " operands; only 0 or 1 supported";
    return failure();
  }
  op->erase();
  return success();
}

/// Successor-based a.if (cf.cond_br convention): operand 0 is the condition,
/// remaining operands are [true dest args..., false dest args...] split by
/// destination block argument counts.
LogicalResult lowerIfSuccessor(Operation *op) {
  if (op->getNumSuccessors() != 2) {
    op->emitError(
        "a-lower-to-llvm: successor-based a.if requires exactly 2 successors");
    return failure();
  }
  if (op->getNumResults() != 0) {
    op->emitError("a-lower-to-llvm: a.if with results is not supported");
    return failure();
  }
  if (op->getNumOperands() < 1) {
    op->emitError("a-lower-to-llvm: a.if missing condition operand");
    return failure();
  }

  Block *trueDest = op->getSuccessor(0);
  Block *falseDest = op->getSuccessor(1);
  const unsigned nTrue = trueDest->getNumArguments();
  const unsigned nOps = op->getNumOperands();
  // operands: [cond, trueArgs(nTrue), falseArgs(rest)]
  if (nOps < 1 + nTrue) {
    op->emitError("a-lower-to-llvm: a.if operand count does not match "
                   "destination block arguments");
    return failure();
  }

  OpBuilder b(op);
  Location loc = op->getLoc();
  Value cond = ensureI1Cond(b, loc, op->getOperand(0));

  // operands: [cond, trueArgs(nTrue), falseArgs(rest)]
  SmallVector<Value> trueVals, falseVals;
  for (unsigned i = 0; i < nTrue; ++i)
    trueVals.push_back(op->getOperand(1 + i));
  for (unsigned i = 1 + nTrue; i < nOps; ++i)
    falseVals.push_back(op->getOperand(i));

  b.create<CondBrOp>(loc, cond, trueDest, trueVals, falseDest, falseVals);
  op->erase();
  return success();
}

/// Region-based a.if (scf.if-like): move each arm region's blocks into the
/// parent function region and select the arm entry with llvm.cond_br.
///
/// LIMITATION (phase 1): arms keep whatever terminator the emitter produced.
/// Arms that must merge into the post-if continuation need the emitter to
/// produce explicit branches; a dedicated merge/phi lowering is future work.
LogicalResult lowerIfRegion(Operation *op) {
  if (op->getNumResults() != 0) {
    op->emitError("a-lower-to-llvm: region-based a.if with results is not "
                   "supported in phase 1");
    return failure();
  }
  if (op->getNumRegions() < 1 || op->getNumRegions() > 2) {
    op->emitError("a-lower-to-llvm: a.if must have 1 (then) or 2 (then/else) "
                   "regions");
    return failure();
  }
  if (op->getNumOperands() < 1) {
    op->emitError("a-lower-to-llvm: a.if missing condition operand");
    return failure();
  }

  Block *parent = op->getBlock();
  Region *parentRegion = parent->getParent();

  // Split the parent block: ops after a.if become the continuation block.
  Block *contBB = nullptr;
  splitBlockAfter(op, contBB);

  OpBuilder b(op);
  Location loc = op->getLoc();
  Value cond = ensureI1Cond(b, loc, op->getOperand(0));

  // Lower each arm region into fresh blocks inserted after `parent`, in
  // order. Arm entry = first block of the moved region.
  SmallVector<Block *> armEntries;
  for (unsigned r = 0; r < op->getNumRegions(); ++r) {
    Region &armRegion = op->getRegion(r);
    if (armRegion.empty()) {
      armEntries.push_back(nullptr);
      continue;
    }
    // Region block-arg types were already lowered by the pre-pass in
    // runOnOperation; armRegion now contains the lowered blocks.
    // armRegion now contains only the new blocks, in order.
    Block *entry = &armRegion.front();
    armEntries.push_back(entry);
    // Move all blocks of armRegion into parentRegion right after `parent`
    // (append at end of insertion point tracking: insert each before
    // contBB to keep program order then/else/cont).
    auto &blocks = armRegion.getBlocks();
    while (!blocks.empty()) {
      Block *bb = &blocks.front();
      bb->moveBefore(contBB);
    }
  }

  Block *thenBB = armEntries.size() > 0 ? armEntries[0] : nullptr;
  Block *elseBB = armEntries.size() > 1 ? armEntries[1] : nullptr;

  // If an arm region was empty, route it straight to the continuation.
  Block *trueDest = thenBB ? thenBB : contBB;
  Block *falseDest = elseBB ? elseBB : contBB;

  b.create<CondBrOp>(loc, cond, trueDest, ValueRange(), falseDest,
                      ValueRange());
  op->erase();
  (void)parentRegion;
  return success();
}

LogicalResult lowerIf(Operation *op) {
  if (op->getNumRegions() > 0)
    return lowerIfRegion(op);
  return lowerIfSuccessor(op);
}

//===----------------------------------------------------------------------===//
// a.for / a.while -> llvm dialect control flow
//
// `a.yield` and `a.condition` carry no lowering of their own: they are
// region terminators consumed by the parent `a.for`/`a.while` lowering, which
// rewrites them into llvm.br / llvm.cond_br. lowerOp() maps them to a no-op.
//
// Loop shape produced for `a.for` (step must be > 0, matching the phase-1
// emitter's `for i in [lo, hi) step st`):
//
//   entry:  br ^check(lo, inits...)
//   check(iv, carried...):
//     %c = llvm.icmp ult %iv, %hi
//     llvm.cond_br %c, ^body(%iv, %carried...), ^exit(%carried...)
//   body:   <lowered body ops>; a.yield -> llvm.br ^check(%iv + %step, yielded...)
//   exit(carried...): llvm.br ^cont
//
// `index`-typed bounds are materialized as i64 constants (phase-1 producers
// are `arith.constant ... : index`); non-constant index values are rejected.
//===----------------------------------------------------------------------===//

/// Materialize `v` as an i64 value usable inside the llvm dialect. i64 passes
/// through (after the same-region check); `index` values are rebuilt as i64
/// constants from ConstantLike producers or index_cast unwraps.
LogicalResult materializeAsI64(Operation *op, Value v, Value &out) {
  if (failed(ensureOperandInRegion(op, v)))
    return failure();
  Type t = v.getType();
  if (auto it = llvm::dyn_cast<IntegerType>(t)) {
    if (it.getWidth() == 64) {
      out = v;
      return success();
    }
  }
  if (!llvm::isa<mlir::IndexType>(t)) {
    op->emitError("a-lower-to-llvm: expected an i64 or `index` value, got `")
        << t << "`";
    return failure();
  }
  MLIRContext *ctx = op->getContext();
  Type i64Ty = IntegerType::get(ctx, 64);
  Operation *def = v.getDefiningOp();
  // `arith.index_cast`/`index_castui` unwrap: the i64 source is the value.
  if (def && def->getNumOperands() == 1 &&
      (def->getName().getStringRef() == "arith.index_cast" ||
       def->getName().getStringRef() == "arith.index_castui")) {
    Value src = def->getOperand(0);
    if (auto sit = llvm::dyn_cast<IntegerType>(src.getType());
        sit && sit.getWidth() == 64) {
      out = src;
      return success();
    }
  }
  if (def && def->hasTrait<mlir::OpTrait::ConstantLike>()) {
    if (auto ia = def->getAttrOfType<mlir::IntegerAttr>("value")) {
      OpBuilder b(op);
      out = b.create<ConstantOp>(op->getLoc(), i64Ty,
                                 mlir::IntegerAttr::get(i64Ty, ia.getInt()));
      return success();
    }
  }
  op->emitError("a-lower-to-llvm: `index` value is not a constant; phase-1 "
                 "a.for/a.while bounds must be constant-foldable");
  return failure();
}

/// Find the single block in `blocks` whose terminator has the given `a`
/// dialect op name ("a.yield" / "a.condition").
Block *findTerminatorBlock(ArrayRef<Block *> blocks, llvm::StringRef name) {
  Block *found = nullptr;
  for (Block *bb : blocks) {
    Operation *term = bb->getTerminator();
    if (term && term->getName().getStringRef() == name) {
      if (found) {
        // Multiple exits are not expected; the verifiers enforce one.
        return nullptr;
      }
      found = bb;
    }
  }
  return found;
}

LogicalResult lowerFor(Operation *op) {
  if (op->getNumRegions() != 1 || op->getRegion(0).empty() ||
      op->getNumOperands() < 3) {
    op->emitError("a-lower-to-llvm: malformed a.for");
    return failure();
  }
  Region &body = op->getRegion(0);
  MLIRContext *ctx = op->getContext();
  Type i64Ty = IntegerType::get(ctx, 64);
  Type i1Ty = IntegerType::get(ctx, 1);
  Location loc = op->getLoc();

  // Region block-arg types were already lowered by the pre-pass in
  // runOnOperation; bodyBlocks now holds the lowered blocks.
  SmallVector<Block *> bodyBlocks;
  for (Block &bb : body.getBlocks())
    bodyBlocks.push_back(&bb);
  Block *bodyEntry = bodyBlocks.front();
  unsigned nInit = op->getNumOperands() - 3;
  if (bodyEntry->getNumArguments() != nInit + 1) {
    op->emitError("a-lower-to-llvm: a.for body entry has ")
        << bodyEntry->getNumArguments() << " arguments, expected "
        << nInit + 1;
    return failure();
  }

  // Bounds -> i64 (index constants materialized, i64 passthrough).
  Value lo, hi, st;
  if (failed(materializeAsI64(op, op->getOperand(0), lo)) ||
      failed(materializeAsI64(op, op->getOperand(1), hi)) ||
      failed(materializeAsI64(op, op->getOperand(2), st)))
    return failure();

  // Locate the a.yield latch inside the (possibly multi-block) body.
  Block *yieldBB = findTerminatorBlock(bodyBlocks, "a.yield");
  if (!yieldBB) {
    op->emitError("a-lower-to-llvm: a.for body has no a.yield terminator");
    return failure();
  }
  Operation *yield = yieldBB->getTerminator();
  if (yield->getNumOperands() != nInit) {
    op->emitError("a-lower-to-llvm: a.yield has ")
        << yield->getNumOperands() << " operands but a.for carries " << nInit;
    return failure();
  }

  Block *parent = op->getBlock();
  Region *parentRegion = parent->getParent();
  Block *contBB = nullptr;
  splitBlockAfter(op, contBB);
  Block *checkBB = new Block;
  Block *exitBB = new Block;
  checkBB->insertBefore(contBB);
  exitBB->insertBefore(contBB);
  // check(iv, carried...): iv is i64, carried types come from the rebuilt
  // body entry arguments (indices 1..n).
  checkBB->addArgument(i64Ty, loc);
  for (unsigned i = 1; i < bodyEntry->getNumArguments(); ++i)
    checkBB->addArgument(bodyEntry->getArgument(i).getType(), loc);
  // exit(carried...): lowered result types.
  SmallVector<Type> exitTys;
  for (OpResult r : op->getResults()) {
    Type lt;
    if (failed(lowerTypeOrError(op, r.getType(), lt, "a.for result")))
      return failure();
    exitTys.push_back(lt);
    exitBB->addArgument(lt, loc);
  }
  if (exitTys.size() != nInit) {
    op->emitError("a-lower-to-llvm: a.for result count does not match "
                   "iter_args count");
    return failure();
  }
  // Move the body blocks into the parent region, preserving order.
  for (Block *bb : bodyBlocks)
    bb->moveBefore(contBB);
  // Entry edge: br ^check(lo, inits...).
  {
    OpBuilder b(op);
    SmallVector<Value> entryVals;
    entryVals.push_back(lo);
    for (unsigned i = 0; i < nInit; ++i) {
      Value init = op->getOperand(3 + i);
      if (failed(ensureOperandInRegion(op, init)))
        return failure();
      entryVals.push_back(init);
    }
    b.create<BrOp>(loc, entryVals, checkBB);
  }

  // Loop guard: iv < hi (unsigned, matching index semantics for lo >= 0).
  {
    OpBuilder tb = OpBuilder::atBlockEnd(checkBB);
    Value iv = checkBB->getArgument(0);
    Value cmp = tb.create<ICmpOp>(
        loc, i1Ty,
        mlir::LLVM::ICmpPredicateAttr::get(
            ctx, mlir::LLVM::ICmpPredicate::ult),
        iv, hi);
    SmallVector<Value> checkArgs(checkBB->getArguments().begin(),
                                 checkBB->getArguments().end());
    SmallVector<Value> exitVals(checkArgs.begin() + 1, checkArgs.end());
    tb.create<CondBrOp>(loc, cmp, bodyEntry, checkArgs, exitBB, exitVals);
  }

  // Exit edge -> continuation.
  {
    OpBuilder eb = OpBuilder::atBlockEnd(exitBB);
    eb.create<BrOp>(loc, ValueRange{}, contBB);
  }

  // Latch: replace a.yield with br ^check(iv + step, yielded...). The iv is
  // the body entry's first argument; it dominates the latch block.
  {
    OpBuilder yb(yield);
    Value iv = bodyEntry->getArgument(0);
    Value ivNext = yb.create<AddOp>(loc, i64Ty, iv, st);
    SmallVector<Value> backVals;
    backVals.push_back(ivNext);
    for (Value yv : yield->getOperands()) {
      Value v = yv;
      if (failed(ensureOperandInRegion(yield, v)))
        return failure();
      backVals.push_back(v);
    }
    yb.create<BrOp>(loc, backVals, checkBB);
    yield->erase();
  }

  // Results are the exit block arguments.
  for (unsigned i = 0; i < op->getNumResults(); ++i)
    op->getResult(i).replaceAllUsesWith(exitBB->getArgument(i));
  op->erase();
  (void)parentRegion;
  return success();
}

/// `a.while` -> llvm dialect loop:
///   entry:        br ^before(inits...)
///   before(args): <ops>; a.condition(%c, fwd...) ->
///                   llvm.cond_br %c, ^after(fwd...), ^exit(fwd...)
///   after(args):  <ops>; a.yield(next...) -> llvm.br ^before(next...)
///   exit(results): llvm.br ^cont
LogicalResult lowerWhile(Operation *op) {
  if (op->getNumRegions() != 2 || op->getRegion(0).empty() ||
      op->getRegion(1).empty()) {
    op->emitError("a-lower-to-llvm: malformed a.while");
    return failure();
  }
  Region &before = op->getRegion(0);
  Region &after = op->getRegion(1);
  Location loc = op->getLoc();

  // Region block-arg types were already lowered by the pre-pass in
  // runOnOperation.

  SmallVector<Block *> beforeBlocks, afterBlocks;
  for (Block &bb : before.getBlocks())
    beforeBlocks.push_back(&bb);
  for (Block &bb : after.getBlocks())
    afterBlocks.push_back(&bb);
  Block *beforeEntry = beforeBlocks.front();
  Block *afterEntry = afterBlocks.front();

  Block *condBB = findTerminatorBlock(beforeBlocks, "a.condition");
  Block *yieldBB = findTerminatorBlock(afterBlocks, "a.yield");
  if (!condBB || !yieldBB) {
    op->emitError("a-lower-to-llvm: a.while regions lack a.condition/a.yield "
                   "terminators");
    return failure();
  }
  Operation *cond = condBB->getTerminator();
  Operation *yield = yieldBB->getTerminator();
  if (cond->getNumOperands() < 1) {
    op->emitError("a-lower-to-llvm: malformed a.condition");
    return failure();
  }

  Block *parent = op->getBlock();
  Region *parentRegion = parent->getParent();
  Block *contBB = nullptr;
  splitBlockAfter(op, contBB);

  Block *exitBB = new Block;
  exitBB->insertBefore(contBB);
  SmallVector<Type> exitTys;
  for (OpResult r : op->getResults()) {
    Type lt;
    if (failed(lowerTypeOrError(op, r.getType(), lt, "a.while result")))
      return failure();
    exitTys.push_back(lt);
    exitBB->addArgument(lt, loc);
  }

  // Move both regions' blocks into the parent region.
  for (Block *bb : beforeBlocks)
    bb->moveBefore(contBB);
  for (Block *bb : afterBlocks)
    bb->moveBefore(contBB);

  // Entry edge: br ^before(inits...).
  {
    OpBuilder b(op);
    SmallVector<Value> entryVals;
    for (Value init : op->getOperands()) {
      Value v = init;
      if (failed(ensureOperandInRegion(op, v)))
        return failure();
      entryVals.push_back(v);
    }
    b.create<BrOp>(loc, entryVals, beforeEntry);
  }

  // a.condition -> llvm.cond_br: true -> after(fwd), false -> exit(fwd).
  {
    OpBuilder cb(cond);
    Value c = cond->getOperand(0);
    if (failed(ensureOperandInRegion(cond, c)))
      return failure();
    c = ensureI1Cond(cb, loc, c);
    SmallVector<Value> fwd;
    for (unsigned i = 1; i < cond->getNumOperands(); ++i) {
      Value v = cond->getOperand(i);
      if (failed(ensureOperandInRegion(cond, v)))
        return failure();
      fwd.push_back(v);
    }
    cb.create<CondBrOp>(loc, c, afterEntry, fwd, exitBB, fwd);
    cond->erase();
  }

  // a.yield -> llvm.br ^before(yielded...).
  {
    OpBuilder yb(yield);
    SmallVector<Value> backVals;
    for (Value yv : yield->getOperands()) {
      Value v = yv;
      if (failed(ensureOperandInRegion(yield, v)))
        return failure();
      backVals.push_back(v);
    }
    yb.create<BrOp>(loc, backVals, beforeEntry);
    yield->erase();
  }

  // Exit edge -> continuation.
  {
    OpBuilder eb = OpBuilder::atBlockEnd(exitBB);
    eb.create<BrOp>(loc, ValueRange{}, contBB);
  }

  for (unsigned i = 0; i < op->getNumResults(); ++i)
    op->getResult(i).replaceAllUsesWith(exitBB->getArgument(i));
  op->erase();
  (void)parentRegion;
  return success();
}

//===----------------------------------------------------------------------===//
// Pass driver
//===----------------------------------------------------------------------===//

LogicalResult lowerOp(Operation *op, CStrGlobalCache &cstrs) {
  llvm::StringRef name = op->getName().getStringRef();
  // Only touch ops in the `a` dialect.
  if (op->getDialect() == nullptr ||
      op->getDialect()->getNamespace() != kADialectName)
    return success();

  if (name == "a.func")
    return lowerFunc(op);
  if (name == "a.call")
    return lowerACall(op);
  if (name == "a.runtime.call")
    return lowerRuntimeCall(op);
  if (name == "a.const")
    return lowerConst(op);
  if (name == "a.alloca")
    return lowerAlloca(op);
  if (name == "a.load")
    return lowerLoad(op);
  if (name == "a.store")
    return lowerStore(op);
  if (name == "a.gep")
    return lowerGep(op);
  if (name == "a.addressof")
    return lowerAddressof(op);
  if (name == "a.return")
    return lowerReturn(op);
  if (name == "a.fib.spawn")
    return lowerFibSpawn(op);
  if (name == "a.fib.join")
    return lowerFibJoin(op);
  if (name == "a.fib.main")
    return lowerFibMain(op);
  if (name == "a.array.get")
    return lowerArrayGet(op);
  if (name == "a.array.set")
    return lowerArraySet(op);
  if (name == "a.struct.getfield")
    return lowerStructGetField(op, cstrs);
  if (name == "a.struct.setfield")
    return lowerStructSetField(op, cstrs);
  if (name == "a.if")
    return lowerIf(op);  // legacy sketch op; not in the landed dialect
  if (name == "a.for")
    return lowerFor(op);
  if (name == "a.while")
    return lowerWhile(op);
  if (name == "a.yield" || name == "a.condition") {
    // Region terminators consumed by the parent a.for/a.while lowering
    // (post-order guarantees the parent is lowered right after).
    return success();
  }

  // Phase-1 unsupported ops: fail gracefully.
  op->emitError("a-lower-to-llvm: unsupported `a` dialect op `")
      << name << "`";
  return failure();
}

} // namespace

void ALowerToLLVMPass::getDependentDialects(
    mlir::DialectRegistry &registry) const {
  registry.insert<mlir::LLVM::LLVMDialect, mlir::func::FuncDialect,
                 mlir::scf::SCFDialect, mlir::cf::ControlFlowDialect,
                 mlir::arith::ArithDialect, mlir::index::IndexDialect,
                 mlir::vector::VectorDialect>();
  // The `a` dialect is provided by the dialect workstream; register it
  // dynamically so this pass resolves once it lands (see pipeline.hpp).
  registry.insert(mlir::TypeID(), kADialectName,
                  [](MLIRContext *ctx) -> mlir::Dialect * {
                    registerADialect(*ctx);
                    return ctx->getLoadedDialect(kADialectName);
                  });
}

void ALowerToLLVMPass::runOnOperation() {
  ModuleOp module = getOperation();

  // Pre-pass: rebuild region block-argument types for a.for/a.while/a.if/a.func
  // so that ops nested inside those regions see lowered (i64/llvm) types when
  // they are processed in the main post-order loop below.
  module.walk<mlir::WalkOrder::PreOrder>([&](Operation *op) {
    if (op->getDialect() == nullptr ||
        op->getDialect()->getNamespace() != kADialectName)
      return mlir::WalkResult::advance();
    llvm::StringRef name = op->getName().getStringRef();
    if (name == "a.for" && op->getNumRegions() == 1 &&
        !op->getRegion(0).empty()) {
      llvm::DenseMap<Block *, Block *> oldToNew;
      (void)rebuildRegionWithLoweredArgTypes(op->getRegion(0), oldToNew);
    } else if (name == "a.while" && op->getNumRegions() == 2 &&
               !op->getRegion(0).empty() && !op->getRegion(1).empty()) {
      llvm::DenseMap<Block *, Block *> m0, m1;
      (void)rebuildRegionWithLoweredArgTypes(op->getRegion(0), m0);
      (void)rebuildRegionWithLoweredArgTypes(op->getRegion(1), m1);
    } else if (name == "a.if") {
      for (unsigned r = 0; r < op->getNumRegions(); ++r) {
        if (op->getRegion(r).empty())
          continue;
        llvm::DenseMap<Block *, Block *> oldToNew;
        (void)rebuildRegionWithLoweredArgTypes(op->getRegion(r), oldToNew);
      }
    } else if (name == "a.func" && op->getNumRegions() == 1 &&
               !op->getRegion(0).empty()) {
      llvm::DenseMap<Block *, Block *> oldToNew;
      (void)rebuildRegionWithLoweredArgTypes(op->getRegion(0), oldToNew);
    }
    return mlir::WalkResult::advance();
  });

  // Collect `a` ops in post-order so inner ops (e.g. a.call inside an a.if
  // region, a.return inside a.func) are lowered before their containers.
  // Operation pointers stay valid: lowering only erases the op being
  // lowered, never its already-lowered descendants.
  SmallVector<Operation *> aOps;
  module.walk<mlir::WalkOrder::PostOrder>([&](Operation *op) {
    if (op->getDialect() != nullptr &&
        op->getDialect()->getNamespace() == kADialectName)
      aOps.push_back(op);
    return mlir::WalkResult::advance();
  });

  CStrGlobalCache cstrGlobals;
  for (Operation *op : aOps) {
    if (failed(lowerOp(op, cstrGlobals)))
      signalPassFailure();
  }
}

std::unique_ptr<mlir::Pass> createALowerToLLVMPass() {
  return std::make_unique<ALowerToLLVMPass>();
}

} // namespace a
