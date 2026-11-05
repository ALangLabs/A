#include "backend/kgen/kgen_backend.hpp"
#include "backend/kgen/object_compiler.hpp"
#include "backend/kgen/codegen/llvm_emitter.hpp"
namespace a::kgen {
bool emitLLVMIR(const ModuleGraph& g, const SemaResult& s, std::ostream& out) { return ObjectCompiler::emitIR(g, s, out); }
bool buildExecutable(const ModuleGraph& g, const SemaResult& s, const std::string& outPath, std::ostream& err) { return ObjectCompiler::build(g, s, outPath, err); }
int runModule(const ModuleGraph& g, const SemaResult& s, std::istream& in, std::ostream& out, std::ostream& err) { return ObjectCompiler::run(g, s, in, out, err); }
bool isSupported(const ModuleGraph& g) { return ObjectCompiler::supported(g); }
bool compileNativeExecutable(const ModuleGraph& g, const SemaResult& s, const std::string& base, std::ostream& err) {
  return a::compileLLVMModule(g, s, base, err);
}
int runNativeExecutable(const std::string& base, std::istream& in, std::ostream& out, std::ostream& err) {
  return a::runBuiltLLVMModule(base, in, out, err);
}
}
