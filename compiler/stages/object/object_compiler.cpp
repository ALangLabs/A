#include "object/object_compiler.hpp"
#include <fstream>
#include "backend/kgen/kgen_backend.hpp"
#include "ir/ir.hpp"
#include "optimization/optimizer.hpp"
namespace a {
int ObjectCompiler::compile(const ModuleGraph& g,const SemaResult& s,const ObjectCompileOptions& o,std::ostream& e){
  if(!kgen::isSupported(g)){ e<<"error: this program is not yet supported by the KGEN/LLVM backend; run with 'a run' for full semantics\n"; return 1; }
  if(!kgen::buildExecutable(g,s,o.outputPath,e)) return 1;
  return 0;
}
int ObjectCompiler::emitLLVM(const ModuleGraph& g,const SemaResult& s,const std::string& outPath,std::ostream& e){
  if(!kgen::isSupported(g)){ e<<"error: this program is not yet supported by the KGEN/LLVM backend\n"; return 1; }
  std::ofstream ofs(outPath); if(!ofs){ e<<"error: cannot create '"<<outPath<<"'\n"; return 1; }
  if(!kgen::emitLLVMIR(g,s,ofs)){ e<<"error: LLVM IR emission failed\n"; return 1; }
  return 0;
}
int ObjectCompiler::emitIRText(const ir::Program& p,bool opt,std::ostream& os){
  ir::Program c=p; if(opt) a::ir::runAll(c); os<<ir::printProgram(c); return 0;
}
}
