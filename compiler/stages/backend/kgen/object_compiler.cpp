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
  if (mlir_backend::buildExecutable(g, s, outPath, err)) return true;
  return a::buildLLVMModule(g, s, outPath, err);
}
int ObjectCompiler::run(const ModuleGraph& g, const SemaResult& s, std::istream& in, std::ostream& out, std::ostream& err) {
  if (mlir_backend::available()) return mlir_backend::runModule(g, s, in, out, err);
  return a::runLLVMModule(g, s, in, out, err);
}
bool ObjectCompiler::supported(const ModuleGraph& g) { return a::isLLVMSupported(g); }
}
