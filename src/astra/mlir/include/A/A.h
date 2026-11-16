//===- A.h - A dialect umbrella header -----------------------*- C++ -*-===//
//
// Public entry point for the `a` MLIR dialect: types, ops, attributes and
// the dialect class. C++ consumers should include this header and then call
// `mlir::a::registerDialect` (or rely on `mlir::registerAllDialects`).
//
// The dialect lives in namespace `mlir::a` (matching upstream convention,
// e.g. `mlir::LLVM`, `mlir::arith`); it is unrelated to the project-wide
// `::a` C++ namespace used by the A frontend.
//
//===----------------------------------------------------------------------===//

#ifndef A_DIALECT_A_H
#define A_DIALECT_A_H

#include "mlir/IR/Dialect.h"
#include "mlir/IR/DialectImplementation.h"
// Enum attribute declarations must precede the op declarations: AOps.h.inc
// references AConstKindAttr in generated method signatures. Both .inc files
// open `namespace mlir::a` themselves.
#include "A/AEnums.h.inc"
#include "A/ATypes.h"
#include "A/AOps.h"

//===----------------------------------------------------------------------===//
// Dialect Declarations (self-namespaceing; generated from AOps.td)
//===----------------------------------------------------------------------===//

#include "A/ADialect.h.inc"

namespace mlir {
namespace a {

/// Register the `a` dialect with the given registry (op/type/attr defs).
void registerDialect(DialectRegistry &registry);

} // namespace a
} // namespace mlir

#endif // A_DIALECT_A_H
