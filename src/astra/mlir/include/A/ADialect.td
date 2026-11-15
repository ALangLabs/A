//===- ADialect.td - A dialect definition ----------------------*- tablegen -*-===//
//
// The `a` dialect is the high-level IR produced by the A frontend before
// lowering to LLVM. Phase 1 preserves the boxed aval semantics of the C
// runtime (compiler/astra/runtime); all value ops carry `!a.aval` (i64) or
// `!a.ptr` operands and lower to the same `llvm.*` op stream that
// src/astra/frontend/mlir/mlir_emit.cpp emits today.
//
// Design rule: generic ops (arith, index, cf, scf) are reused via their own
// dialects; `a` only contains A-specific semantics: aval boxing, runtime
// calls, fibers, nominal aggregates, string literals.
//
//===----------------------------------------------------------------------===//

#ifndef A_DIALECT_TD
#define A_DIALECT_TD

include "mlir/IR/DialectBase.td"

def A_Dialect : Dialect {
  let name = "a";
  let cppNamespace = "::mlir::a";
  let summary = "A language IR";
  let description = [{
    The `a` dialect is the A language's own MLIR dialect. It is the boundary
    between the checked A AST and the LLVM dialect. In phase 1 every A value
    is boxed as `!a.aval` (tagged i64) and heap/aggregate operations go
    through the `a_*` C runtime via `a.runtime.call`; phase 2 introduces
    unboxed scalar types and lowers aggregates to LLVM memory ops.
  }];

  let hasConstantMaterializer = 1;
  let useDefaultAttributePrinterParser = 1;
  let useDefaultTypePrinterParser = 1;

  let extraClassDeclaration = [{
    /// Registers all `a` types with this dialect.
    void registerTypes();

    /// Name of the attribute carrying the runtime function signature on
    /// `a.runtime.call` ops (used by the verifier).
    static StringRef getRuntimeCallSignatureAttrName() {
      return "signature";
    }
  }];
}

#endif // A_Dialect_TD
