//===- AOps.cpp - A dialect op implementations -------------*- C++ -*-===//
//
// Verifiers and interface-method implementations for the `a` dialect ops,
// plus the op/type registration glue. Lowering to the LLVM dialect is *not*
// implemented here; it lives in src/astra/mlir/passes/ (phase 2 of the
// mlir-architecture plan).
//
//===----------------------------------------------------------------------===//

#include "A/A.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/DialectImplementation.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Interfaces/FunctionInterfaces.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ErrorHandling.h"

namespace mlir {
namespace a {

//===----------------------------------------------------------------------===//
// Helpers
//===----------------------------------------------------------------------===//

namespace {

/// Returns true if `type` is one of the phase-1 boxed/pointer types.
bool isBoxedOrPtr(Type type) {
  return isa<AVALType, APtrType>(type);
}

} // namespace

//===----------------------------------------------------------------------===//
// a.const
//===----------------------------------------------------------------------===//

LogicalResult AConstOp::verify() {
  AConstKind kind = getKind();
  switch (kind) {
  case AConstKind::Int:
  case AConstKind::Bool:
    if (!getIvalueAttr())
      return emitOpError() << "requires an i64 `ivalue` for kind `"
                           << stringifyAConstKind(kind) << "`";
    if (getFvalueAttr() || getStrAttr())
      return emitOpError() << "only `ivalue` may be set for this kind";
    break;
  case AConstKind::Float:
    if (!getFvalueAttr())
      return emitOpError() << "requires an f64 `fvalue` for kind `float`";
    if (getIvalueAttr() || getStrAttr())
      return emitOpError() << "only `fvalue` may be set for kind `float`";
    break;
  case AConstKind::Str:
    if (!getStrAttr())
      return emitOpError() << "requires a `str` attribute for kind `str`";
    if (getIvalueAttr() || getFvalueAttr())
      return emitOpError() << "only `str` may be set for kind `str`";
    break;
  case AConstKind::None:
    if (getIvalueAttr() || getFvalueAttr() || getStrAttr())
      return emitOpError() << "kind `none` takes no payload";
    break;
  }
  return success();
}

//===----------------------------------------------------------------------===//
// a.func
//===----------------------------------------------------------------------===//

LogicalResult AFuncOp::verify() {
  // Exactly one region, possibly empty (external declaration).
  if (getOperation()->getNumRegions() != 1)
    return emitOpError() << "requires exactly one region";
  if (getBody().empty())
    return success(); // external declaration

  FunctionType fty = getFunctionType();
  Block &entry = getBody().front();

  // Block arguments must match the declared input types.
  if (entry.getNumArguments() != fty.getNumInputs())
    return emitOpError() << "body block has " << entry.getNumArguments()
                         << " arguments but function type expects "
                         << fty.getNumInputs();
  for (unsigned i = 0; i < entry.getNumArguments(); ++i) {
    if (entry.getArgument(i).getType() != fty.getInput(i))
      return emitOpError() << "body block argument " << i << " has type "
                           << entry.getArgument(i).getType()
                           << " but function type expects " << fty.getInput(i);
  }

  // Every terminator must be a.return (checked by the terminator trait);
  // result arity is checked in AReturnOp::verify against the parent.
  return success();
}

//===----------------------------------------------------------------------===//
// a.call
//===----------------------------------------------------------------------===//

LogicalResult ACallOp::verify() {
  // Resolve the callee symbol to its CallableOpInterface to compare
  // operand/result types against the declared function type.
  // Resolve the callee symbol via SymbolTable to an Operation, then use
  // CallableOpInterface for the types.
  Operation *calleeOp = SymbolTable::lookupNearestSymbolFrom(
      getOperation(), getCalleeAttr());
  auto callable = calleeOp ? dyn_cast<CallableOpInterface>(calleeOp)
                           : CallableOpInterface();
  if (!callable)
    return emitOpError() << "could not resolve callee";
  auto argTypes = callable.getArgumentTypes();
  auto resultTypes = callable.getResultTypes();

  if (getNumOperands() != argTypes.size())
    return emitOpError() << "expected " << argTypes.size()
                         << " operands to match callee signature, got "
                         << getNumOperands();
  for (unsigned i = 0; i < getNumOperands(); ++i) {
    if (getOperand(i).getType() != argTypes[i])
      return emitOpError() << "operand " << i << " has type "
                           << getOperand(i).getType() << " but callee expects "
                           << argTypes[i];
  }
  if (getNumResults() != resultTypes.size())
    return emitOpError() << "expected " << resultTypes.size()
                         << " results to match callee signature, got "
                         << getNumResults();
  for (unsigned i = 0; i < getNumResults(); ++i) {
    if (getResult(i).getType() != resultTypes[i])
      return emitOpError() << "result " << i << " has type "
                           << getResult(i).getType() << " but callee produces "
                           << resultTypes[i];
  }
  return success();
}

LogicalResult ACallOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
  // SymbolUserOpInterface hook: resolve `callee` through the collection's
  // nearest enclosing symbol table and require a callable target. Signature
  // compatibility is checked in verify().
  Operation *calleeOp =
      symbolTable.lookupNearestSymbolFrom(getOperation(), getCalleeAttr());
  if (!calleeOp)
    return emitOpError() << "could not resolve callee `" << getCallee() << "`";
  if (!isa<CallableOpInterface>(calleeOp))
    return emitOpError() << "callee `" << getCallee()
                         << "` does not implement CallableOpInterface";
  return success();
}

//===----------------------------------------------------------------------===//
// a.return
//===----------------------------------------------------------------------===//

LogicalResult AReturnOp::verify() {
  auto *parent = getOperation()->getParentOp();
  auto func = dyn_cast<AFuncOp>(parent);
  if (!func)
    return emitOpError() << "must be nested inside an a.func";
  if (func.getBody().empty())
    return emitOpError() << "cannot return from an external function";

  FunctionType fty = func.getFunctionType();
  if (fty.getNumResults() == 0) {
    if (getValue())
      return emitOpError() << "function returns no values";
    return success();
  }
  if (!getValue())
    return emitOpError() << "function result type is " << fty.getResult(0)
                         << " but no value was returned";
  if (getValue().getType() != fty.getResult(0))
    return emitOpError() << "returned value has type " << getValue().getType()
                         << " but function returns " << fty.getResult(0);
  return success();
}

//===----------------------------------------------------------------------===//
// a.yield
//===----------------------------------------------------------------------===//

LogicalResult AYieldOp::verify() {
  Operation *parent = getOperation()->getParentOp();
  Region *region = getOperation()->getParentRegion();
  if (auto forOp = dyn_cast<AForOp>(parent)) {
    // a.yield operands become the next iteration's iter_args, so their
    // types must match the init arg types carried by the loop.
    auto initArgs = forOp.getInitArgs();
    if (getNumOperands() != initArgs.size())
      return emitOpError() << "yield has " << getNumOperands()
                           << " operands but a.for carries "
                           << initArgs.size() << " iter_args";
    for (unsigned i = 0; i < getNumOperands(); ++i)
      if (getOperand(i).getType() != initArgs[i].getType())
        return emitOpError() << "yield operand " << i << " has type "
                             << getOperand(i).getType()
                             << " but iter_arg " << i << " has type "
                             << initArgs[i].getType();
    return success();
  }
  if (auto whileOp = dyn_cast<AWhileOp>(parent)) {
    // In the `after` region, yield operands become the next `before`
    // region args; in `before` (unreachable for yield) n/a. We verify
    // against the after-block argument types when nested there.
    if (region == &whileOp.getAfter()) {
      Block &afterEntry = whileOp.getAfter().front();
      // yield feeds back into `before` args == while inits count? scf.while
      // requires yield operand types == before-region arg types.
      Block &beforeEntry = whileOp.getBefore().front();
      if (getNumOperands() != beforeEntry.getNumArguments())
        return emitOpError()
               << "yield has " << getNumOperands()
               << " operands but the before region takes "
               << beforeEntry.getNumArguments() << " arguments";
      for (unsigned i = 0; i < getNumOperands(); ++i)
        if (getOperand(i).getType() != beforeEntry.getArgument(i).getType())
          return emitOpError() << "yield operand " << i << " type mismatch";
      (void)afterEntry;
      return success();
    }
    return emitOpError() << "a.yield is only valid in the `after` region of "
                            "a.while";
  }
  return emitOpError() << "a.yield must be nested in a.for or a.while";
}

//===----------------------------------------------------------------------===//
// a.for
//===----------------------------------------------------------------------===//

LogicalResult AForOp::verify() {
  if (getOperation()->getNumRegions() != 1)
    return emitOpError() << "requires exactly one region";
  Region &body = getBody();
  if (body.empty())
    return emitOpError() << "requires a non-empty body region";
  Block &entry = body.front();

  // Block args: induction var (index) + one per init arg.
  if (entry.getNumArguments() != getInitArgs().size() + 1)
    return emitOpError() << "body block has " << entry.getNumArguments()
                         << " arguments but expected "
                         << getInitArgs().size() + 1;
  if (entry.getArgument(0).getType() != IndexType::get(getContext()))
    return emitOpError() << "induction variable must have type `index`";
  unsigned i = 1;
  for (Value init : getInitArgs()) {
    if (entry.getArgument(i).getType() != init.getType())
      return emitOpError() << "iter_arg " << (i - 1)
                           << " block argument type "
                           << entry.getArgument(i).getType()
                           << " does not match init type " << init.getType();
    ++i;
  }

  // Results must match init arg types (final iter_args values).
  if (getNumResults() != getInitArgs().size())
    return emitOpError() << "expected " << getInitArgs().size()
                         << " results (final iter_args), got " << getNumResults();
  i = 0;
  for (OpResult res : getResults()) {
    if (res.getType() != getInitArgs()[i].getType())
      return emitOpError() << "result " << i << " type " << res.getType()
                           << " does not match iter_arg type "
                           << getInitArgs()[i].getType();
    ++i;
  }

  // The body must terminate in a.yield; operand count/types are verified
  // by AYieldOp::verify against getYieldedValues() (init arg types).
  Operation *terminator = entry.getTerminator();
  if (!terminator || !isa<AYieldOp>(terminator))
    return entry.empty()
               ? emitOpError() << "body block is empty"
               : emitOpError() << "body must terminate in a.yield, found "
                               << (terminator ? terminator->getName().getStringRef()
                                              : llvm::StringRef("no terminator"));
  return success();
}

//===----------------------------------------------------------------------===//
// a.while
//===----------------------------------------------------------------------===//

LogicalResult AWhileOp::verify() {
  if (getOperation()->getNumRegions() != 2)
    return emitOpError() << "requires exactly `before` and `after` regions";
  Region &before = getBefore();
  Region &after = getAfter();
  if (before.empty() || after.empty())
    return emitOpError() << "both regions must be non-empty";

  // before block args == init operand types.
  Block &beforeEntry = before.front();
  if (beforeEntry.getNumArguments() != getNumOperands())
    return emitOpError() << "before region has "
                         << beforeEntry.getNumArguments()
                         << " arguments but op has " << getNumOperands()
                         << " inits";
  for (unsigned i = 0; i < getNumOperands(); ++i)
    if (beforeEntry.getArgument(i).getType() != getOperand(i).getType())
      return emitOpError() << "before region argument " << i << " type "
                           << beforeEntry.getArgument(i).getType()
                           << " does not match init type "
                           << getOperand(i).getType();

  // before must end in a.condition.
  Operation *beforeTerm = beforeEntry.getTerminator();
  auto cond = beforeTerm ? dyn_cast<AConditionOp>(beforeTerm) : AConditionOp();
  if (!cond)
    return emitOpError() << "before region must terminate in a.condition";
  if (cond.getCondition().getType() != IntegerType::get(getContext(), 1))
    return emitOpError() << "a.condition operand 0 must have type `i1`";

  // a.condition trailing operands -> after block args (or while results).
  unsigned numForwarded = cond.getNumOperands() - 1;
  Block &afterEntry = after.front();
  if (afterEntry.getNumArguments() != numForwarded)
    return emitOpError() << "after region has "
                         << afterEntry.getNumArguments()
                         << " arguments but a.condition forwards "
                         << numForwarded;
  for (unsigned i = 0; i < numForwarded; ++i) {
    Type forwardedTy = cond.getOperand(i + 1).getType();
    if (afterEntry.getArgument(i).getType() != forwardedTy)
      return emitOpError() << "after region argument " << i << " type "
                           << afterEntry.getArgument(i).getType()
                           << " does not match forwarded type " << forwardedTy;
  }

  // When the condition is false the forwarded operands become results.
  if (getNumResults() != numForwarded)
    return emitOpError() << "expected " << numForwarded
                         << " results matching a.condition operands, got "
                         << getNumResults();
  for (unsigned i = 0; i < getNumResults(); ++i) {
    if (getResult(i).getType() != cond.getOperand(i + 1).getType())
      return emitOpError() << "result " << i << " type " << getResult(i).getType()
                           << " does not match a.condition operand type "
                           << cond.getOperand(i + 1).getType();
  }

  // after must terminate in a.yield (operand check in AYieldOp::verify).
  Operation *afterTerm = afterEntry.getTerminator();
  if (!afterTerm || !isa<AYieldOp>(afterTerm))
    return emitOpError() << "after region must terminate in a.yield";
  return success();
}

//===----------------------------------------------------------------------===//
// a.condition
//===----------------------------------------------------------------------===//

LogicalResult AConditionOp::verify() {
  auto *parent = getOperation()->getParentOp();
  auto whileOp = dyn_cast<AWhileOp>(parent);
  if (!whileOp || getOperation()->getParentRegion() != &whileOp.getBefore())
    return emitOpError()
           << "a.condition must terminate the `before` region of an a.while";
  // Operand-type consistency with the after region / results is verified by
  // AWhileOp::verify (which has the full picture).
  return success();
}

//===----------------------------------------------------------------------===//
// Memory ops
//===----------------------------------------------------------------------===//

LogicalResult AAllocaOp::verify() {
  // element_type must be a valid `a` or builtin type; any TypeAttr is OK in
  // phase 1, but reject obviously wrong kinds (e.g. label/type attr of a
  // function type is fine too — keep permissive).
  return success();
}

LogicalResult ALoadOp::verify() {
  if (!isa<APtrType>(getPtr().getType()))
    return emitOpError() << "pointer operand must have type !a.ptr, got "
                         << getPtr().getType();
  return success();
}

LogicalResult AStoreOp::verify() {
  if (!isBoxedOrPtr(getValue().getType()))
    return emitOpError() << "stored value must be !a.aval or !a.ptr, got "
                         << getValue().getType();
  if (!isa<APtrType>(getPtr().getType()))
    return emitOpError() << "pointer operand must have type !a.ptr, got "
                         << getPtr().getType();
  return success();
}

LogicalResult AGetElementPtrOp::verify() {
  if (!isa<APtrType>(getPtr().getType()))
    return emitOpError() << "pointer operand must have type !a.ptr, got "
                         << getPtr().getType();
  return success();
}

LogicalResult AAddressofOp::verify() { return success(); }

//===----------------------------------------------------------------------===//
// a.runtime.call
//===----------------------------------------------------------------------===//

LogicalResult ARuntimeCallOp::verify() {
  if (getCallee().empty())
    return emitOpError() << "callee name must not be empty";
  if (getSignatureAttr()) {
    // The signature is an opaque string for phase 1 ("(i64, i64) -> i64");
    // full checking happens in the lowering pass which parses it.
  }
  return success();
}

//===----------------------------------------------------------------------===//
// Fib ops
//===----------------------------------------------------------------------===//

LogicalResult AFibSpawnOp::verify() {
  if (!isa<AVALType>(getFibFn().getType()))
    return emitOpError() << "fib_fn must be !a.aval";
  if (!isa<IntegerType>(getNargs().getType()) ||
      cast<IntegerType>(getNargs().getType()).getWidth() != 64)
    return emitOpError() << "nargs must be i64";
  if (!isa<AVALType>(getArgs().getType()))
    return emitOpError() << "args must be !a.aval";
  return success();
}

LogicalResult AFibJoinOp::verify() {
  if (!isa<IntegerType>(getHandle().getType()) ||
      cast<IntegerType>(getHandle().getType()).getWidth() != 64)
    return emitOpError() << "handle must be i64";
  return success();
}

LogicalResult AFibMainOp::verify() {
  if (!isa<IntegerType>(getFnptr().getType()) ||
      cast<IntegerType>(getFnptr().getType()).getWidth() != 64)
    return emitOpError() << "fnptr must be i64";
  if (!isa<IntegerType>(getArgc().getType()) ||
      cast<IntegerType>(getArgc().getType()).getWidth() != 64)
    return emitOpError() << "argc must be i64";
  if (!isa<APtrType>(getArgv().getType()))
    return emitOpError() << "argv must be !a.ptr";
  if (!isa<AVALType>(getEnv().getType()))
    return emitOpError() << "env must be !a.aval";
  return success();
}

//===----------------------------------------------------------------------===//
// Aggregate access
//===----------------------------------------------------------------------===//

LogicalResult AStructGetFieldOp::verify() {
  if (!isa<AVALType>(getObj().getType()))
    return emitOpError() << "obj must be !a.aval";
  if (!isa<IntegerType>(getIndex().getType()) ||
      cast<IntegerType>(getIndex().getType()).getWidth() != 64)
    return emitOpError() << "field index must be i64";
  if (auto ty = getStructTypeAttr())
    if (!isa<AStructType>(ty.getValue()))
      return emitOpError() << "struct_type must be an !a.struct type";
  return success();
}

LogicalResult AStructSetFieldOp::verify() {
  if (!isa<AVALType>(getObj().getType()))
    return emitOpError() << "obj must be !a.aval";
  if (!isa<IntegerType>(getIndex().getType()) ||
      cast<IntegerType>(getIndex().getType()).getWidth() != 64)
    return emitOpError() << "field index must be i64";
  if (!isa<AVALType>(getValue().getType()))
    return emitOpError() << "value must be !a.aval";
  if (auto ty = getStructTypeAttr())
    if (!isa<AStructType>(ty.getValue()))
      return emitOpError() << "struct_type must be an !a.struct type";
  return success();
}

LogicalResult AArrayGetOp::verify() {
  if (!isa<AVALType>(getArray().getType()))
    return emitOpError() << "array must be !a.aval";
  if (!isa<AVALType>(getIndex().getType()))
    return emitOpError() << "index must be !a.aval (boxed index)";
  if (auto ty = getArrayTypeAttr())
    if (!isa<AArrayType>(ty.getValue()))
      return emitOpError() << "array_type must be an !a.array type";
  return success();
}

LogicalResult AArraySetOp::verify() {
  if (!isa<AVALType>(getArray().getType()))
    return emitOpError() << "array must be !a.aval";
  if (!isa<AVALType>(getIndex().getType()))
    return emitOpError() << "index must be !a.aval (boxed index)";
  if (!isa<AVALType>(getValue().getType()))
    return emitOpError() << "value must be !a.aval";
  if (auto ty = getArrayTypeAttr())
    if (!isa<AArrayType>(ty.getValue()))
      return emitOpError() << "array_type must be an !a.array type";
  return success();
}

//===----------------------------------------------------------------------===//
// RegionBranchOpInterface implementations
//===----------------------------------------------------------------------===//

// a.for: branching to `parent` yields the loop-carried results (fed by
// a.yield operands); branching to the body region enters the loop.
// `getEntrySuccessorOperands` (init args) is implemented below next to the
// other accessors.
void AForOp::getSuccessorRegions(RegionBranchPoint point,
                                 SmallVectorImpl<RegionSuccessor> &regions) {
  // Like `scf.for`: both the body region and the parent operation (zero-trip
  // exit / loop exit) are successors. From the body, `a.yield` either loops
  // back into the body or exits to the parent results.
  regions.push_back(RegionSuccessor(&getBody()));
  // A null region marks the parent operation as the successor.
  regions.push_back(RegionSuccessor(getResults()));
}

::mlir::OperandRange
AForOp::getEntrySuccessorOperands(RegionBranchPoint point) {
  // Entering the loop feeds the init args to the iter_args block arguments
  // (the induction variable is produced by the loop itself). Like
  // `scf.for`, the same operand range is reported regardless of whether the
  // entry successor is the body region or the parent (zero-iteration exit),
  // since the op results have the iter_arg types.
  return getODSOperands(3);
}

// a.while: entry branches to `before` (inits feed its block args) or, when
// the loop is skipped entirely, to the parent results. From `before`,
// a.condition branches to `after` (forwarded operands) or the parent
// (results). From `after`, a.yield branches back to `before` (next iteration
// args) or the parent.
void AWhileOp::getSuccessorRegions(RegionBranchPoint point,
                                   SmallVectorImpl<RegionSuccessor> &regions) {
  if (point.isParent()) {
    regions.push_back(RegionSuccessor(&getBefore()));
    regions.push_back(RegionSuccessor(getResults()));
    return;
  }
  if (point.getRegionOrNull() == &getBefore()) {
    regions.push_back(RegionSuccessor(&getAfter()));
    regions.push_back(RegionSuccessor(getResults()));
    return;
  }
  if (point.getRegionOrNull() == &getAfter()) {
    regions.push_back(RegionSuccessor(&getBefore()));
    regions.push_back(RegionSuccessor(getResults()));
    return;
  }
  llvm_unreachable("unknown region branch point");
}

::mlir::OperandRange
AWhileOp::getEntrySuccessorOperands(RegionBranchPoint point) {
  // All init operands are forwarded to the `before` region entry block.
  return getInits();
}

//===----------------------------------------------------------------------===//
// RegionBranchTerminatorOpInterface implementations
//===----------------------------------------------------------------------===//

::mlir::MutableOperandRange AYieldOp::getMutableSuccessorOperands(
    RegionBranchPoint point) {
  // All operands are forwarded to the parent op's continuation.
  return ::mlir::MutableOperandRange(*this);
}

::mlir::MutableOperandRange AConditionOp::getMutableSuccessorOperands(
    RegionBranchPoint point) {
  // Skip the leading condition operand; the rest are forwarded.
  return ::mlir::MutableOperandRange(*this, 1,
                                     getOperation()->getNumOperands() - 1);
}

//===----------------------------------------------------------------------===//
// a.for accessors (extraClassDeclaration)
//===----------------------------------------------------------------------===//

BlockArgument AForOp::getInductionVar() {
  return getBody().front().getArgument(0);
}

Value AForOp::getLowerBoundOperand() { return getOperand(0); }
Value AForOp::getUpperBoundOperand() { return getOperand(1); }
Value AForOp::getStepOperand() { return getOperand(2); }

ValueRange AForOp::getRegionIterArgs() {
  return getBody().front().getArguments().drop_front(1);
}

ValueRange AForOp::getYieldedValues() {
  // The yielded values of the single-block body: the a.yield operands.
  Operation *term = getBody().front().getTerminator();
  if (!term)
    return ValueRange();
  return term->getOperands();
}

void AForOp::setYieldedValues(ValueRange newValues) {
  Operation *term = getBody().front().getTerminator();
  if (!term)
    return;
  for (auto it : llvm::zip(term->getOpOperands(), newValues))
    std::get<0>(it).assign(std::get<1>(it));
}

//===----------------------------------------------------------------------===//
// ODS-generated definitions (mlir-tblgen -gen-op-defs output).
// The .cpp.inc opens `namespace mlir::a` itself, so it must be included at
// global scope.
//===----------------------------------------------------------------------===//

} // namespace a
} // namespace mlir

#define GET_OP_CLASSES
#include "A/AOps.cpp.inc"
