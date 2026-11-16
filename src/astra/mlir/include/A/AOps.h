//===- AOps.h - A dialect operation declarations -----------*- C++ -*-===//
//
// Op declarations for the `a` dialect. Generated classes come from
// AOps.h.inc (mlir-tblgen -gen-op-decls); the .inc opens
// `namespace mlir::a` itself, so it is included at global scope.
//
//===----------------------------------------------------------------------===//

#ifndef A_DIALECT_AOPS_H
#define A_DIALECT_AOPS_H

#include "A/ATypes.h"
#include "mlir/IR/OpDefinition.h"
#include "mlir/Bytecode/BytecodeOpInterface.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Interfaces/CallInterfaces.h"
#include "mlir/Interfaces/ControlFlowInterfaces.h"
#include "mlir/Interfaces/FunctionInterfaces.h"
#include "mlir/Interfaces/InferTypeOpInterface.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"

//===----------------------------------------------------------------------===//
// ODS-Generated Declarations (self-namespaceing)
//===----------------------------------------------------------------------===//

#define GET_OP_CLASSES
#include "A/AOps.h.inc"

#endif // A_DIALECT_AOPS_H
