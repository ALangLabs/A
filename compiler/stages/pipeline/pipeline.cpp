#include "pipeline/pipeline.hpp"
#include <fstream>
#include <sstream>
#include <utility>
#include "backend/kgen/execution_engine.hpp"
#include "backend/kgen/execution_engine_impl.hpp"
#include "backend/kgen/kgen_backend.hpp"
#include "backend/kgen/repl_eval.hpp"
#include "diagnostics/diagnostics.hpp"
#include "ir/ir.hpp"
#include "name-resolution/module_loader.hpp"
#include "object/object_compiler.hpp"
#include "optimization/optimizer.hpp"
#include "ownership/ownership.hpp"
#include "type-checker/sema.hpp"
namespace a {
namespace {
constexpr int kOk=0,kCErr=1,kNoIn=66;
bool flushErrors(DiagnosticSink& s,const PipelineConfig& c,std::ostream& o,std::ostream& e){
  if(!s.hasErrors()) return true;
  if(c.jsonDiagnostics&&c.checkOnly) s.writeJson(o); else s.flush(e,c.useColor);
  return false;
}
void flushFinal(DiagnosticSink& s,const PipelineConfig& c,std::ostream& o,std::ostream& e){
  if(c.jsonDiagnostics&&c.checkOnly) s.writeJson(o); else s.flush(e,c.useColor);
}
bool readAll(const std::string& p,std::string& t,std::ostream& e){
  std::ifstream f(p,std::ios::binary); if(!f){e<<"error: cannot open '"<<p<<"'\n"; return false;}
  std::ostringstream b; b<<f.rdbuf(); if(f.bad()){e<<"error: failed to read '"<<p<<"'\n"; return false;} t=b.str(); return true;
}
}
CompiledModule runFrontendPipeline(const std::string& path,std::string source,const PipelineConfig& config,DiagnosticSink& sink,std::ostream& diagOut,std::ostream& diagErr){
  ModuleLoaderOptions opts; opts.modulePaths=config.modulePaths; opts.stdlibRoot=config.stdlibRoot;
  ModuleLoader loader(std::move(opts),sink); ModuleGraph graph=loader.load(path,std::move(source));
  if(!flushErrors(sink,config,diagOut,diagErr)) return {};
  Sema sema(graph,sink); SemaResult result=sema.analyze(); if(!flushErrors(sink,config,diagOut,diagErr)) return {};
  analyzeOwnership(graph,sink); if(!flushErrors(sink,config,diagOut,diagErr)) return {};
  flushFinal(sink,config,diagOut,diagErr);
  CompiledModule m; m.graph=std::make_unique<ModuleGraph>(std::move(graph)); m.sema=std::make_unique<SemaResult>(std::move(result)); m.ok=true; return m;
}
int executePipeline(const std::string& path,std::string source,const PipelineConfig& config,std::istream& in,std::ostream& out,std::ostream& err){
  DiagnosticSink sink; auto m=runFrontendPipeline(path,std::move(source),config,sink,out,err); if(!m.ok) return kCErr; if(config.checkOnly) return kOk;
  // Fast path: native-subset programs compile and run directly
  // (Goose/Mojo-style). If native compilation is unavailable, fall back to
  // the internal engine so `run` keeps full dynamic semantics.
  if(std::getenv("A_NATIVE_RUN")&&kgen::isSupported(*m.graph)){
    std::string base="/tmp/a_native_run_"+std::to_string(::getpid())+"_"+std::to_string(::rand());
    if(kgen::compileNativeExecutable(*m.graph,*m.sema,base,err)){
      return kgen::runNativeExecutable(base,in,out,err);
    }
  }
  return kgen::runWithEngine(*m.graph,*m.sema,sink,in,out,err);
}
int testPipeline(const std::string& path,const PipelineConfig& config,std::istream& in,std::ostream& out,std::ostream& err){
  std::string src; if(!readAll(path,src,err)) return kNoIn; DiagnosticSink sink; auto m=runFrontendPipeline(path,std::move(src),config,sink,out,err); if(!m.ok) return kCErr;
  return kgen::testWithEngine(*m.graph,*m.sema,sink,in,out,err);
}
int buildPipeline(const std::string& path,const std::string& outputPath,const PipelineConfig& config,std::ostream& err){
  std::string src; if(!readAll(path,src,err)) return kNoIn; DiagnosticSink sink; auto m=runFrontendPipeline(path,std::move(src),config,sink,err,err); if(!m.ok) return kCErr;
  ObjectCompileOptions o; o.outputPath=outputPath;
  return ObjectCompiler::compile(*m.graph,*m.sema,o,err);
}
int emitLLVMPipeline(const std::string& path,const std::string& outputPath,const PipelineConfig& config,std::ostream& err){
  std::string src; if(!readAll(path,src,err)) return kNoIn; DiagnosticSink sink; auto m=runFrontendPipeline(path,std::move(src),config,sink,err,err); if(!m.ok) return kCErr;
  return ObjectCompiler::emitLLVM(*m.graph,*m.sema,outputPath,err);
}
int emitIRPipeline(const std::string& path,const PipelineConfig& config,bool optimize,std::ostream& out,std::ostream& err){
  std::string src; if(!readAll(path,src,err)) return kNoIn; DiagnosticSink sink; auto m=runFrontendPipeline(path,std::move(src),config,sink,err,err); if(!m.ok) return kCErr;
  ir::Program p=ir::lowerProgram(*m.graph,*m.sema); if(optimize) a::ir::runAll(p); out<<ir::printProgram(p); return kOk;
}
ReplSession::~ReplSession() = default;
int replEval(ReplSession& session,const std::string& source,const PipelineConfig& config,std::istream& in,std::ostream& out,std::ostream& err){
  DiagnosticSink frontendSink;
  auto m=runFrontendPipeline("<repl>",source,config,frontendSink,out,err);
  if(!m.ok) return 1;
  if(!session.primed){
    session.graph=std::move(m.graph); session.sema=std::move(m.sema);
    if(!session.sink) session.sink=std::make_unique<DiagnosticSink>();
    session.engine=std::make_unique<KGENExecutionEngine>(*session.graph,*session.sema,*session.sink,in,out,err);
    session.engine->setReplEcho(true);
    session.primed=true;
  }else{
    // Mutate in place: the engine holds references to these objects.
    *session.graph=std::move(*m.graph); *session.sema=std::move(*m.sema);
  }
  int rc=session.engine->evalPending();
  session.sink->flush(err,false);
  return rc;
}

namespace {
ReplSession g_replSession;
PipelineConfig g_replConfig;
bool g_replBusy = false;

// `repl.eval` bridge used by the self-hosted REPL UI. Reentrancy (user code
// calling repl.eval while an eval is in flight) is rejected with code 3.
int replEvalBridge(const std::string& source, std::istream& in, std::ostream& out,
                   std::ostream& err) {
  if (g_replBusy) return 3;
  g_replBusy = true;
  const int rc = replEval(g_replSession, source, g_replConfig, in, out, err);
  g_replBusy = false;
  return rc;
}
}  // namespace

int runReplUi(const std::string& entryPath, const PipelineConfig& config,
              std::istream& in, std::ostream& out, std::ostream& err) {
  g_replConfig = config;
  g_replConfig.checkOnly = false;
  kgen::setReplEvalHandler(&replEvalBridge);

  std::string source;
  if (!readAll(entryPath, source, err)) return kNoIn;
  DiagnosticSink sink;
  auto m = runFrontendPipeline(entryPath, std::move(source), g_replConfig, sink, out, err);
  if (!m.ok) return kCErr;
  return kgen::runWithEngine(*m.graph, *m.sema, sink, in, out, err);
}
}
