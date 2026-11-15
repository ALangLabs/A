//===- ADialect.cpp - `a` dialect registration & glue ---------*- C++ -*-===//
//
// Dialect-level implementation for the `a` MLIR dialect: type registration,
// attribute parsing/printing hooks and constant materialization. Op/type
// definitions come from the ODS-generated .inc files included at the bottom.
//
//===----------------------------------------------------------------------===//

#include "A/A.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/DialectImplementation.h"

namespace mlir {
namespace a {

//===----------------------------------------------------------------------===//
// Type registration
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Attribute parsing/printing
//===----------------------------------------------------------------------===//

::mlir::Attribute
ADialect::parseAttribute(::mlir::DialectAsmParser &parser, ::mlir::Type type) const {
  // The `a` dialect currently defines no custom attributes; all op
  // attributes are builtins (StringAttr/IntegerAttr/TypeAttr/...). Return
  // failure so the parser reports a clean diagnostic.
  (void)parser;
  (void)type;
  return Attribute();
}

void ADialect::printAttribute(::mlir::Attribute attr,
                              ::mlir::DialectAsmPrinter &os) const {
  os.printAttribute(attr);
}

//===----------------------------------------------------------------------===//
// Constant materialization
//===----------------------------------------------------------------------===//

::mlir::Operation *
ADialect::materializeConstant(::mlir::OpBuilder &builder, ::mlir::Attribute value,
                              ::mlir::Type type, ::mlir::Location loc) {
  // Phase 1: only `!a.aval` constants are materialized, as `a.const` ops.
  if (!isa<AVALType>(type))
    return nullptr;

  AConstKind kind;
  IntegerAttr ivalue;
  FloatAttr fvalue;
  StringAttr str;

  if (auto intAttr = dyn_cast<IntegerAttr>(value)) {
    kind = AConstKind::Int;
    ivalue = IntegerAttr::get(IntegerType::get(builder.getContext(), 64),
                              intAttr.getInt());
  } else if (auto floatAttr = dyn_cast<FloatAttr>(value)) {
    kind = AConstKind::Float;
    fvalue = floatAttr;
  } else if (auto boolAttr = dyn_cast<BoolAttr>(value)) {
    kind = AConstKind::Bool;
    ivalue = IntegerAttr::get(IntegerType::get(builder.getContext(), 64),
                              boolAttr.getValue() ? 1 : 0);
  } else if (auto strAttr = dyn_cast<StringAttr>(value)) {
    kind = AConstKind::Str;
    str = strAttr;
  } else {
    return nullptr;
  }

  return builder
      .create<AConstOp>(loc, type, AConstKindAttr::get(builder.getContext(), kind),
                        ivalue, fvalue, str)
      .getOperation();
}

//===----------------------------------------------------------------------===//
// Dialect registration entry point
//===----------------------------------------------------------------------===//

void registerDialect(DialectRegistry &registry) { registry.insert<ADialect>(); }

//===----------------------------------------------------------------------===//
// Dialect initialization
//
// Registers every `a` op, the `AConstKindAttr` enum attribute, and all `a`
// types. The op list mirrors the GET_OP_LIST section of the generated
// AOps.cpp.inc; keep them in sync when ops are added/removed in AOps.td.
//===----------------------------------------------------------------------===//

void ADialect::initialize() {
  addOperations<AAddressofOp, AAllocaOp, AArrayGetOp, AArraySetOp, ACallOp,
                AConditionOp, AConstOp, AFibJoinOp, AFibMainOp, AFibSpawnOp,
                AForOp, AFuncOp, AGetElementPtrOp, ALoadOp, AReturnOp,
                ARuntimeCallOp, AStoreOp, AStructGetFieldOp, AStructSetFieldOp,
                AWhileOp, AYieldOp>();
  // NOTE: AConstKindAttr (an I64EnumAttr) is intentionally not registered via
  // addAttributes<>: that instantiation requires the complete
  // IntegerAttrStorage definition, which only exists in the MLIRIR TU that
  // includes BuiltinAttributes.cpp.inc. Enum attrs still print/verify through
  // their own TypeID without dialect registration.
  registerTypes();
}

} // namespace a
} // namespace mlir

//===----------------------------------------------------------------------===//
// ODS-generated definitions (self-namespaceing .inc files, global scope).
//===----------------------------------------------------------------------===//

// Dialect ctor/dtor (unguarded). Enum attribute utility definitions
// (AEnums.cpp.inc) provide stringifyAConstKind/symbolizeAConstKind and
// AConstKindAttr::get; include them here at global scope.
#include "A/ADialect.cpp.inc"
#include "A/AEnums.cpp.inc"
