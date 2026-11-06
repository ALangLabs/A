#include "backend/kgen/mlir/kgen_mlir_codegen.hpp"
#include <string>
#include "backend/kgen/codegen/llvm_emitter.hpp"
#include "backend/kgen/native_backend.hpp"
namespace a::kgen::mlir_backend {
bool available() { return true; }

// Legacy LLVM-18 llc/clang subprocess backend RETIRED: the builtin native compiler
// fork (compiler/tcc) is the only native compiler for A. Kept as a shim so
// old callers still compile; both helpers delegate to the native backend.
bool emitLLVMIR(const ModuleGraph& g, const SemaResult& s, std::ostream& out, std::ostream& /*err*/) {
  return a::emitLLVMIR(g, s, out);
}

bool buildExecutable(const ModuleGraph& g, const SemaResult& s, const std::string& outputPath, std::ostream& err) {
  return buildExecutableWithNative(g, s, outputPath, err);
}

int runModule(const ModuleGraph& g, const SemaResult& s, std::istream& in, std::ostream& out, std::ostream& err) {
  return a::runLLVMModule(g, s, in, out, err);
}
}
