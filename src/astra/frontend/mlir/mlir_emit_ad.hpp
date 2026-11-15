#pragma once
// `a`-dialect MLIR emitter (A -> .mlir -> a-lower-to-llvm -> LLVM IR).
// See mlir_emit_ad.cpp header for coverage and dialect-gap notes.
#include <string>

namespace a {
struct ModuleGraph;
struct SemaResult;

// Emits the checked module graph as MLIR text for the `a`-dialect
// pipeline (a mlir-opt / a-lower-to-llvm). Signature mirrors emitMLIR.
//
// DIALECT STATUS: the emitter produces llvm-dialect ops plus the `a`
// ops that are text-expressible and semantics-preserving —
// `a.runtime.call` for every `a_*` runtime call and `a.addressof` for
// string/cstr globals feeding those calls. The remaining ops stay
// llvm.* because the `a` op verifiers require !a.aval/!a.ptr operands
// that the emitter's raw-i64 aval domain cannot produce (see the gap
// list in mlir_emit_ad.cpp).
bool emitMLIRDialect(const ModuleGraph& graph, const SemaResult& sema,
                     std::string& out, std::string& error,
                     bool testMode = false, const std::string* testFilter = nullptr);
}  // namespace a
