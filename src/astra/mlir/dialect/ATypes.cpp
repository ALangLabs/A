//===- ATypes.cpp - A dialect type implementations ----------*- C++ -*-===//
//
// Verifiers for the `a` dialect types. Trivial (parameterless) types only
// need the storage check performed by TypeDef; parameterized types get
// light validation here.
//
//===----------------------------------------------------------------------===//

#include "A/A.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/DialectImplementation.h"
#include "mlir/IR/Types.h"
#include "llvm/ADT/TypeSwitch.h"

namespace mlir {
namespace a {

//===----------------------------------------------------------------------===//
// AVAL
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// AStruct / AEnum: non-empty names
//===----------------------------------------------------------------------===//

LogicalResult AStructType::verify(function_ref<InFlightDiagnostic()> emitError,
                                  StringRef name) {
  if (name.empty())
    return emitError() << "expected non-empty struct name in !a.struct<...>";
  return success();
}

LogicalResult AEnumType::verify(function_ref<InFlightDiagnostic()> emitError,
                                StringRef name) {
  if (name.empty())
    return emitError() << "expected non-empty enum name in !a.enum<...>";
  return success();
}

//===----------------------------------------------------------------------===//
// AArray: positive size
//===----------------------------------------------------------------------===//

LogicalResult AArrayType::verify(function_ref<InFlightDiagnostic()> emitError,
                                 unsigned size) {
  if (size == 0)
    return emitError() << "expected positive array size in !a.array<N>";
  return success();
}

} // namespace a
} // namespace mlir

//===----------------------------------------------------------------------===//
// ODS-generated definitions (mlir-tblgen -gen-typedef-defs output).
// The .cpp.inc opens `namespace mlir::a` itself and also emits the
// MLIR_DEFINE_EXPLICIT_TYPE_ID definitions, so it is included at global
// scope and the manual TypeID block above is intentionally absent.
//===----------------------------------------------------------------------===//

#define GET_TYPEDEF_CLASSES
// The generated printer/parser helpers are only referenced when a type opts
// into custom assembly; all `a` types use the default, leaving them unused.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "A/ATypes.cpp.inc"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

//===----------------------------------------------------------------------===//
// Dialect type registration
//
// Defined here (not ADialect.cpp) because the parametric type storage classes
// are only complete after ATypes.cpp.inc is included.
//===----------------------------------------------------------------------===//

namespace mlir {
namespace a {

void ADialect::registerTypes() {
  addTypes<AVALType, APtrType, AStructType, AArrayType, AEnumType, ANoneType,
           ABoolType>();
}

} // namespace a
} // namespace mlir
