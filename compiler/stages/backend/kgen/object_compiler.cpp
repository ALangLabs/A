#include "backend/kgen/object_compiler.hpp"
#include "backend/kgen/mlir/kgen_mlir_codegen.hpp"
#include <sstream>
#include "backend/kgen/codegen/llvm_emitter.hpp"
#include "backend/kgen/native_backend.hpp"
namespace a::kgen {
// KGEN object compiler: Astra (compiler/astra/) is the ONLY native backend.
// The old LLVM-18 llc/clang subprocess path is retired (kept only as a
// stub for API compatibility; it now delegates to the native backend).
bool ObjectCompiler::emitIR(const ModuleGraph& g, const SemaResult& s, std::ostream& out) {
  return a::emitLLVMIR(g, s, out);
}
bool ObjectCompiler::build(const ModuleGraph& g, const SemaResult& s, const std::string& outPath, std::ostream& err) {
  return buildExecutableWithNative(g, s, outPath, err);
}
int ObjectCompiler::run(const ModuleGraph& g, const SemaResult& s, std::istream& in, std::ostream& out, std::ostream& err) {
  // Callers gate on supported(); the builtin native backend is the reference.
  return a::runLLVMModule(g, s, in, out, err);
}
bool ObjectCompiler::supported(const ModuleGraph& g) { return a::isLLVMSupported(g); }
}
