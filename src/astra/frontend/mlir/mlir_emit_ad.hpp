#pragma once
// `a`-dialect MLIR emitter (A -> .mlir -> a-lower-to-llvm -> LLVM IR).
// See mlir_emit_ad.cpp header for coverage and dialect-gap notes.
#include <string>

namespace a {
struct ModuleGraph;
struct SemaResult;

// Emits the checked module graph as MLIR text intended for the `a` dialect
// pipeline (a mlir-opt / a-lower-to-llvm). Signature mirrors emitMLIR.
//
// IMPORTANT (dialect text-format status): the `a` dialect registered by
// src/astra/mlir is NOT expressible in MLIR text today — see the gap list in
// mlir_emit_ad.cpp. Until those gaps are closed this entry point emits the
// llvm-dialect op stream (identical to emitMLIR), which parses under
// `a mlir-opt` and lowers/links through the existing pipeline. Swapping in
// real `a.*` ops later is a localized change inside the MlirDialectEmitter
// helpers (c64/avalInt/callI64/callAFunction/emitReturn/...), not a redesign.
bool emitMLIRDialect(const ModuleGraph& graph, const SemaResult& sema,
                     std::string& out, std::string& error,
                     bool testMode = false, const std::string* testFilter = nullptr);
}  // namespace a
