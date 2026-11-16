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
#include "mlir/IR/OpImplementation.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Interfaces/FunctionImplementation.h"
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

/// Returns true if `type` is an M1 unboxed scalar: any integer width up to
/// i64 (i1 booleans included), or any float width up to f64. The aval ABI
/// payload is a single i64 word, so wider scalars have no boxing encoding.
bool isM1Scalar(Type type) {
  if (auto intTy = dyn_cast<IntegerType>(type))
    return intTy.getWidth() <= 64;
  if (auto floatTy = dyn_cast<FloatType>(type))
    return floatTy.getWidth() <= 64;
  return false;
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
// a.func custom assembly (func.func-style)
//
//   a.func @name(%arg0: !a.aval, %arg1: i64) -> !a.aval {attrs} { body }
//
// The signature prints the function-type input/output types; argument names
// are included only when the body is present (they are the entry block
// argument names). sym_name and function_type are consumed by the format;
// sym_visibility/arg_attrs/res_attrs stay in the attribute dict.
//===----------------------------------------------------------------------===//

void AFuncOp::print(::mlir::OpAsmPrinter &p) {
  // Delegate to the shared function-op printer: emits the visibility
  // keyword, `@name`, the `(args) -> results` signature (with `%argN`
  // names when a body is present), the `attributes {}` dict, and the body
  // region with entry-block args bound to the signature.
  ::mlir::function_interface_impl::printFunctionOp(
      p, ::mlir::FunctionOpInterface(getOperation()),
      /*isVariadic=*/false, "function_type",
      ::mlir::StringAttr::get(getContext(), "arg_attrs"),
      ::mlir::StringAttr::get(getContext(), "res_attrs"));
}

::mlir::ParseResult AFuncOp::parse(::mlir::OpAsmParser &parser,
                                   ::mlir::OperationState &result) {
  return ::mlir::function_interface_impl::parseFunctionOp(
      parser, result, /*allowVariadic=*/false,
      ::mlir::StringAttr::get(result.getContext(), "function_type"),
      [](mlir::Builder &builder, llvm::ArrayRef<mlir::Type> inputs,
         llvm::ArrayRef<mlir::Type> results,
         mlir::function_interface_impl::VariadicFlag,
         std::string &) -> mlir::Type {
        return mlir::FunctionType::get(builder.getContext(), inputs,
                                       results);
      },
      ::mlir::StringAttr::get(result.getContext(), "arg_attrs"),
      ::mlir::StringAttr::get(result.getContext(), "res_attrs"));
}

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
// a.for custom assembly (scf.for-style)
//
//   %r = a.for %lo to %hi step %st iter_args(%a, %b) : index to index
//     step index, !a.aval, !a.aval -> !a.aval, !a.aval { ^bb0(...): ... }
//
// iter_args are bare operand values; the carried names are the body block
// arguments. Result types equal the iter_args types (enforced by verify).
//===----------------------------------------------------------------------===//

namespace {

/// Parse the `:` `type` `to` `type` `step` `type` signature prefix shared by
/// `a.for` (init types follow as a comma-separated list; results via `->`).
/// Returns the three bound types via `boundTys`.
ParseResult parseBoundsSignature(::mlir::OpAsmParser &parser,
                                 ::llvm::SmallVectorImpl<::mlir::Type> &boundTys) {
  for (unsigned i = 0; i < 3; ++i) {
    ::mlir::Type ty;
    if (failed(parser.parseType(ty)))
      return failure();
    boundTys.push_back(ty);
    if (i == 0) {
      if (failed(parser.parseKeyword("to")))
        return failure();
    } else if (i == 1) {
      if (failed(parser.parseKeyword("step")))
        return failure();
    }
    // After the step type, init types are separated by commas handled by
    // the caller.
  }
  return success();
}

} // namespace

void AForOp::print(::mlir::OpAsmPrinter &p) {
  auto &os = p.getStream();
  p.printOperand(getLowerBound());
  os << " to ";
  p.printOperand(getUpperBound());
  os << " step ";
  p.printOperand(getStep());
  auto initArgs = getInitArgs();
  if (!initArgs.empty()) {
    os << " iter_args(";
    p.printOperands(initArgs);
    os << ")";
  }
  os << " : " << getLowerBound().getType() << " to "
     << getUpperBound().getType() << " step " << getStep().getType();
  for (Value init : initArgs)
    os << ", " << init.getType();
  // Result types always match the init arg types; print them for clarity.
  auto results = getResults();
  if (!results.empty()) {
    os << " -> ";
    bool first = true;
    for (OpResult r : results) {
      if (!first)
        os << ", ";
      first = false;
      p.printType(r.getType());
    }
  }
  p.printRegion(getBody());
  // Attributes print after the region with the `attributes` keyword (the
  // func.func/scf.while convention) so the region's `{` is never confused
  // with an attribute dictionary during parsing.
  p.printOptionalAttrDictWithKeyword(getOperation()->getAttrs());
}

::mlir::ParseResult AForOp::parse(::mlir::OpAsmParser &parser,
                                  ::mlir::OperationState &result) {
  ::mlir::SmallVector<::mlir::OpAsmParser::UnresolvedOperand> operands;
  if (failed(parser.parseOperand(operands.emplace_back())))
    return failure();
  if (failed(parser.parseKeyword("to")))
    return failure();
  if (failed(parser.parseOperand(operands.emplace_back())))
    return failure();
  if (failed(parser.parseKeyword("step")))
    return failure();
  if (failed(parser.parseOperand(operands.emplace_back())))
    return failure();

  // Optional `iter_args(operands...)`.
  if (succeeded(parser.parseOptionalKeyword("iter_args"))) {
    if (failed(parser.parseLParen()))
      return failure();
    if (failed(parser.parseOptionalRParen())) {
      while (true) {
        if (failed(parser.parseOperand(operands.emplace_back())))
          return failure();
        if (succeeded(parser.parseOptionalRParen()))
          break;
        if (failed(parser.parseComma()))
          return failure();
      }
    }
  }

  // `:` then bound types, then `, type` for each iter_arg.
  if (failed(parser.parseColon()))
    return failure();
  ::mlir::SmallVector<::mlir::Type> boundTys, initTypes, resultTypes;
  if (failed(parseBoundsSignature(parser, boundTys)))
    return failure();
  unsigned nInit = operands.size() - 3;
  while (initTypes.size() < nInit) {
    if (failed(parser.parseComma()))
      return failure();
    ::mlir::Type ty;
    if (failed(parser.parseType(ty)))
      return failure();
    initTypes.push_back(ty);
  }
  // Optional `-> type, ...` results; defaults to the init types since the
  // verifier requires results == iter_arg types.
  if (failed(parser.parseOptionalArrowTypeList(resultTypes)))
    return failure();
  if (resultTypes.empty())
    resultTypes = initTypes;
  else if (resultTypes.size() != nInit)
    return parser.emitError(parser.getCurrentLocation(),
                            "expected ")
           << nInit << " result types, got " << resultTypes.size();

  // Resolve operands: the three bounds are `index`, iter_args take the
  // parsed init types.
  ::mlir::SmallVector<::mlir::Type> operandTypes(boundTys.begin(),
                                               boundTys.end());
  operandTypes.append(initTypes.begin(), initTypes.end());
  ::mlir::SmallVector<::mlir::Value> values;
  if (failed(parser.resolveOperands(
          operands, operandTypes, parser.getCurrentLocation(), values)))
    return failure();

  result.addOperands(values);
  result.addTypes(resultTypes);

  // Region entry args: induction var (index) + one per iter_arg. Passing
  // them to parseRegion types the entry block; the text supplies names.
  ::mlir::SmallVector<::mlir::OpAsmParser::Argument> regionArgs;
  {
    ::mlir::OpAsmParser::Argument ivArg;
    ivArg.type = ::mlir::IndexType::get(parser.getBuilder().getContext());
    regionArgs.push_back(ivArg);
    for (::mlir::Type ty : initTypes) {
      ::mlir::OpAsmParser::Argument a;
      a.type = ty;
      regionArgs.push_back(a);
    }
  }
  auto body = std::make_unique<::mlir::Region>();
  if (failed(parser.parseRegion(*body, regionArgs)))
    return failure();
  result.addRegion(std::move(body));
  // `attributes { ... }` may follow the region.
  ::mlir::NamedAttrList attrs;
  if (failed(parser.parseOptionalAttrDictWithKeyword(attrs)))
    return failure();
  result.addAttributes(attrs);
  return success();
}

//===----------------------------------------------------------------------===//
// a.while custom assembly (scf.while-style)
//
//   %r = a.while (%init, ...) : (!a.aval, ...) -> !a.aval, ... {
//     ^bb0(...): ... a.condition(%c) (%x : !a.aval) } do {
//     ^bb0(...): ... a.yield %next : !a.aval }
//===----------------------------------------------------------------------===//

void AWhileOp::print(::mlir::OpAsmPrinter &p) {
  auto &os = p.getStream();
  auto inits = getInits();
  os << "(";
  p.printOperands(inits);
  os << ") : (";
  bool firstTy = true;
  for (Value v : inits) {
    if (!firstTy)
      os << ", ";
    firstTy = false;
    p.printType(v.getType());
  }
  os << ")";
  auto results = getResults();
  if (!results.empty()) {
    os << " -> ";
    bool first = true;
    for (OpResult r : results) {
      if (!first)
        os << ", ";
      first = false;
      p.printType(r.getType());
    }
  }
  os << " ";
  p.printRegion(getBefore());
  os << " do ";
  p.printRegion(getAfter());
  p.printOptionalAttrDictWithKeyword(getOperation()->getAttrs());
}

::mlir::ParseResult AWhileOp::parse(::mlir::OpAsmParser &parser,
                                    ::mlir::OperationState &result) {
  // `(` operand-list `)` `:` `(` type-list `)` [`->` type-list]
  ::mlir::SmallVector<::mlir::OpAsmParser::UnresolvedOperand> operands;
  ::mlir::SmallVector<::mlir::Type> initTypes, resultTypes;
  if (failed(parser.parseLParen()))
    return failure();
  if (failed(parser.parseOperandList(operands)))
    return failure();
  if (failed(parser.parseRParen()))
    return failure();
  if (failed(parser.parseColon()))
    return failure();
  if (failed(parser.parseLParen()))
    return failure();
  if (failed(parser.parseTypeList(initTypes)))
    return failure();
  if (failed(parser.parseRParen()))
    return failure();
  if (operands.size() != initTypes.size())
    return parser.emitError(parser.getCurrentLocation(),
                            "expected ")
           << initTypes.size() << " init types for " << operands.size()
           << " operands";
  if (failed(parser.parseOptionalArrowTypeList(resultTypes)))
    return failure();

  ::mlir::SmallVector<::mlir::Value> values;
  if (failed(parser.resolveOperands(operands, initTypes,
                                    parser.getCurrentLocation(), values)))
    return failure();
  result.addOperands(values);
  result.addTypes(resultTypes);

  // Region entry args: `before` takes the init operand types; `after` takes
  // the types `a.condition` forwards (parsed from its block args in text).
  ::mlir::SmallVector<::mlir::OpAsmParser::Argument> beforeArgs;
  for (::mlir::Type ty : initTypes) {
    ::mlir::OpAsmParser::Argument a;
    a.type = ty;
    beforeArgs.push_back(a);
  }
  auto before = std::make_unique<::mlir::Region>();
  if (failed(parser.parseRegion(*before, beforeArgs)))
    return failure();
  if (failed(parser.parseKeyword("do")))
    return failure();
  auto after = std::make_unique<::mlir::Region>();
  if (failed(parser.parseRegion(*after)))
    return failure();
  result.addRegion(std::move(before));
  result.addRegion(std::move(after));
  // `attributes { ... }` may follow the regions.
  ::mlir::NamedAttrList attrs;
  if (failed(parser.parseOptionalAttrDictWithKeyword(attrs)))
    return failure();
  result.addAttributes(attrs);
  return success();
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
  if (auto field = getFieldAttr())
    if (field.getValue().empty())
      return emitOpError() << "field name must be non-empty when present";
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
  if (auto field = getFieldAttr())
    if (field.getValue().empty())
      return emitOpError() << "field name must be non-empty when present";
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
// a.to_aval / a.from_aval (phase-2 boxing boundary, M1)
//===----------------------------------------------------------------------===//

LogicalResult AToAvalOp::verify() {
  // Only M1 scalars may be boxed; an !a.aval operand is the identity (the
  // pass folds it), and aggregates/pointers have no M1 unboxed form.
  if (!isM1Scalar(getValue().getType()) &&
      !isa<AVALType>(getValue().getType()))
    return emitOpError() << "operand must be an unboxed scalar (iN/fN/i1) or "
                            "!a.aval, got "
                         << getValue().getType();
  return success();
}

LogicalResult AFromAvalOp::verify() {
  if (!isa<AVALType>(getValue().getType()))
    return emitOpError() << "operand must be !a.aval, got "
                         << getValue().getType();
  // Unchecked ABI unpack: only scalars are legal result types. Anything else
  // (aggregates, pointers, aval) is either a no-op or needs the checked
  // `a.unbox<T>` op that phase 2 has not introduced yet.
  if (!isM1Scalar(getResult().getType()))
    return emitOpError() << "result must be an unboxed scalar (iN/fN/i1), got "
                         << getResult().getType();
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
  // The body's first block argument is the induction variable, which the
  // loop produces itself; only the iter_arg block arguments are fed by the
  // init operands.
  Block::BlockArgListType bodyInputs = getBody().empty()
      ? Block::BlockArgListType()
      : getBody().front().getArguments().drop_front(1);
  regions.push_back(RegionSuccessor(&getBody(), bodyInputs));
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
  // scf.while-shaped control flow:
  //   parent  -> before            (inits feed the before entry args)
  //   before  -> after | results   (a.condition forwards operands[1:])
  //   after   -> before            (a.yield operands feed the next iteration)
  // Results are produced only by the `before -> parent` edge (condition
  // false); `after` never exits directly.
  if (getBefore().empty() || getAfter().empty())
    return;
  Block &beforeEntry = getBefore().front();
  Block &afterEntry = getAfter().front();
  if (point.isParent()) {
    regions.push_back(
        RegionSuccessor(&getBefore(), beforeEntry.getArguments()));
    return;
  }
  if (point.getRegionOrNull() == &getBefore()) {
    regions.push_back(
        RegionSuccessor(&getAfter(), afterEntry.getArguments()));
    regions.push_back(RegionSuccessor(getResults()));
    return;
  }
  if (point.getRegionOrNull() == &getAfter()) {
    regions.push_back(
        RegionSuccessor(&getBefore(), beforeEntry.getArguments()));
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
// ODS-generated definitions (mlir-tblgen -gen-op-defs output).
// The .cpp.inc opens `namespace mlir::a` itself, so it must be included at
// global scope.
//===----------------------------------------------------------------------===//

} // namespace a
} // namespace mlir

#define GET_OP_CLASSES
#include "A/AOps.cpp.inc"
