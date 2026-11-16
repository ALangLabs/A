//===- ATypes.h - A dialect type declarations --------------*- C++ -*-===//
//
// Type declarations for the `a` dialect. The ODS-generated classes (incl.
// type ID declarations) are pulled in via ATypes.h.inc (produced by
// mlir-tblgen -gen-typedef-decls). The generated .inc opens
// `namespace mlir::a` itself, so it must be included at global scope.
//
//===----------------------------------------------------------------------===//

#ifndef A_DIALECT_ATYPES_H
#define A_DIALECT_ATYPES_H

#include "mlir/IR/Types.h"
#include "mlir/Support/TypeID.h"

//===----------------------------------------------------------------------===//
// ODS-Generated Declarations (self-namespaceing)
//===----------------------------------------------------------------------===//

#define GET_TYPEDEF_CLASSES
#include "A/ATypes.h.inc"

#endif // A_DIALECT_ATYPES_H
