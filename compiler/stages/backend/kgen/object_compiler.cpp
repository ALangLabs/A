#include "backend/kgen/object_compiler.hpp"
#include "backend/kgen/mlir/kgen_mlir_codegen.hpp"
#include <sstream>
#include "backend/kgen/codegen/llvm_emitter.hpp"
namespace a::kgen {
// KGEN object compiler: prefers LLVM-18 toolchain (llc/clang) path when available.
bool ObjectCompiler::emitIR(const ModuleGraph& g, const SemaResult& s, std::ostream& out) {
  std::ostringstream err;
  if (mlir_backend::emitLLVMIR(g, s, out, err)) return true;
  return a::emitLLVMIR(g, s, out);
}
bool ObjectCompiler::build(const ModuleGraph& g, const SemaResult& s, const std::string& outPath, std::ostream& err) {
  // The X86Emitter+gcc path is the reference native backend; the MLIR/llc-18
  // path is kept as a fallback for targets the direct emitter cannot handle.
  if (a::buildLLVMModule(g, s, outPath, err)) return true;
  return mlir_backend::buildExecutable(g, s, outPath, err);
}
int ObjectCompiler::run(const ModuleGraph& g, const SemaResult& s, std::istream& in, std::ostream& out, std::ostream& err) {
  // Callers gate on supported(); the X86Emitter+gcc path is the reference
  // native backend. The MLIR/llc-18 path emitted IR llc rejected, so it is
  // not used for `run`.
  return a::runLLVMModule(g, s, in, out, err);
}
bool ObjectCompiler::supported(const ModuleGraph& g) { return a::isLLVMSupported(g); }
}
