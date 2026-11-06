#include "pipeline/pipeline.hpp"
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>
#include <sys/types.h>
#include <sys/wait.h>
#include "diagnostics/diagnostics.hpp"
#include "ir/ir.hpp"
#include "name-resolution/module_loader.hpp"
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

// ---------------------------------------------------------------- Astra exec
// `a run`/`a build`/`a test` are thin wrappers over the Astra toolchain
// driver (compiler/astra/): the .a source goes straight to `astra`, which
// compiles A -> machine code and links the executable in-process.

// Locates the astra driver: A_ASTRA override, then the binary staged with
// the runtime (build/tcclib/astra or next to this executable), then PATH.
std::string findAstra(){
  if(const char* e=std::getenv("A_ASTRA"); e&&*e) return e;
  char exe[4096];
  ssize_t n=readlink("/proc/self/exe",exe,sizeof(exe)-1);
  if(n>0){
    exe[n]='\0';
    std::string dir=exe;
    auto pos=dir.find_last_of('/');
    if(pos!=std::string::npos){
      dir=dir.substr(0,pos);
      for(const char* rel: {"/tcclib/astra","/astra","/tcc/astra","/../tcclib/astra"}){
        std::string c=dir+rel;
        if(access(c.c_str(),X_OK)==0) return c;
      }
    }
  }
  return "astra";  // fall back to PATH lookup in execvp
}

// fork/exec `astra` with the given arguments; the child inherits our
// stdin/stdout/stderr. Returns the exit code, 127 on exec failure.
int runAstra(const std::vector<std::string>& args){
  std::string astra=findAstra();
  std::vector<char*> argv;
  argv.push_back(const_cast<char*>(astra.c_str()));
  for(const auto& a:args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  pid_t pid=fork();
  if(pid<0) return 127;
  if(pid==0){ execvp(astra.c_str(),argv.data()); _exit(127); }
  int status=0;
  while(waitpid(pid,&status,0)<0){ if(errno!=EINTR) return 127; }
  if(WIFEXITED(status)) return WEXITSTATUS(status);
  return 128+WTERMSIG(status);
}

// fork/exec a built binary directly; the child inherits our
// stdin/stdout/stderr. Returns the exit code, 127 on exec failure.
int runBinary(const std::string& path){
  pid_t pid=fork();
  if(pid<0) return 127;
  if(pid==0){ execl(path.c_str(),path.c_str(),static_cast<char*>(nullptr)); _exit(127); }
  int status=0;
  while(waitpid(pid,&status,0)<0){ if(errno!=EINTR) return 127; }
  if(WIFEXITED(status)) return WEXITSTATUS(status);
  return 128+WTERMSIG(status);
}

std::string tempPath(const char* tag,const std::string& name=""){
  std::string base="/tmp/a_"+std::string(tag)+"_"+std::to_string(::getpid())+"_"+std::to_string(::rand());
  if(!name.empty()){
    std::string ext=name.substr(name.find_last_of('.')==std::string::npos?name.size():name.find_last_of('.'));
    base+=ext;
  }
  return base;
}

// Stages inline source (-c / repl) to a temp .a file; returns "" on failure.
std::string stageSource(const std::string& source,std::ostream& err){
  std::string p=tempPath("src",".a");
  std::ofstream f(p,std::ios::binary|std::ios::trunc);
  if(!f){err<<"error: cannot write temp source '"<<p<<"'\n"; return "";}
  f<<source;
  return p;
}

// Compiles srcPath with astra into outPath. Returns 0 on success.
int astraBuild(const std::string& srcPath,const std::string& outPath,std::ostream& err){
  int rc=runAstra({srcPath,"-o",outPath});
  if(rc!=0){err<<"error: astra failed to compile '"<<srcPath<<"'\n"; return 1;}
  return 0;
}

// Compiles `source` with astra and runs the resulting binary, returning the
// program's exit code. `inputPath` is passed to astra directly when it names
// a real file (runFile/build); otherwise the source is staged to /tmp.
int astraCompileRun(const std::string& inputPath,const std::string& source,
                    std::istream& in,std::ostream& out,std::ostream& err){
  (void)in;(void)out;
  std::string src=inputPath;
  std::string staged;
  std::error_code ec;
  if(src.empty()||!std::filesystem::exists(src,ec)){
    staged=stageSource(source,err);
    if(staged.empty()) return kCErr;
    src=staged;
  }
  std::string bin=tempPath("bin");
  int rc=astraBuild(src,bin,err);
  std::filesystem::remove(staged,ec);
  if(rc!=0){std::filesystem::remove(bin,ec); return kCErr;}
  rc=runBinary(bin);
  std::filesystem::remove(bin,ec);
  return rc<0?kCErr:rc;
}
}
CompiledModule runFrontendPipeline(const std::string& path,std::string source,const PipelineConfig& config,DiagnosticSink& sink,std::ostream& diagOut,std::ostream& diagErr){
  ModuleLoaderOptions opts; opts.modulePaths=config.modulePaths; opts.stdlibRoot=config.stdlibRoot;
  ModuleLoader loader(std::move(opts),sink); ModuleGraph graph=loader.load(path,std::move(source));
  if(!flushErrors(sink,config,diagOut,diagErr)) return {};
  Sema sema(graph,sink); SemaResult result = config.jobs > 1 ? sema.analyzeParallel(config.jobs) : sema.analyze(); if(!flushErrors(sink,config,diagOut,diagErr)) return {};
  analyzeOwnership(graph,sink); if(!flushErrors(sink,config,diagOut,diagErr)) return {};
  flushFinal(sink,config,diagOut,diagErr);
  CompiledModule m; m.graph=std::make_unique<ModuleGraph>(std::move(graph)); m.sema=std::make_unique<SemaResult>(std::move(result)); m.ok=true; return m;
}
int executePipeline(const std::string& path,std::string source,const PipelineConfig& config,std::istream& in,std::ostream& out,std::ostream& err){
  DiagnosticSink sink; auto m=runFrontendPipeline(path,source,config,sink,out,err); if(!m.ok) return kCErr; if(config.checkOnly) return kOk;
  // Execution is delegated to Astra: the .a source is compiled and linked
  // by the builtin toolchain driver, then the binary runs as a child process.
  return astraCompileRun(path,source,in,out,err);
}
int testPipeline(const std::string& path,const PipelineConfig& config,std::istream& in,std::ostream& out,std::ostream& err){
  std::string src; if(!readAll(path,src,err)) return kNoIn;
  DiagnosticSink sink; auto m=runFrontendPipeline(path,std::move(src),config,sink,out,err); if(!m.ok) return kCErr;
  // `a test` compiles and runs the program through Astra; test_* discovery
  // is handled by the program's own entry point.
  return astraCompileRun(path,"",in,out,err);
}
int buildPipeline(const std::string& path,const std::string& outputPath,const PipelineConfig& config,std::ostream& err){
  std::string src; if(!readAll(path,src,err)) return kNoIn;
  DiagnosticSink sink; auto m=runFrontendPipeline(path,std::move(src),config,sink,err,err); if(!m.ok) return kCErr;
  return astraBuild(path,outputPath,err);
}
int emitIRPipeline(const std::string& path,const PipelineConfig& config,bool optimize,std::ostream& out,std::ostream& err){
  std::string src; if(!readAll(path,src,err)) return kNoIn; DiagnosticSink sink; auto m=runFrontendPipeline(path,std::move(src),config,sink,err,err); if(!m.ok) return kCErr;
  ir::Program p=ir::lowerProgram(*m.graph,*m.sema); if(optimize) a::ir::runAll(p); out<<ir::printProgram(p); return kOk;
}
ReplSession::~ReplSession() = default;
int replEval(ReplSession& session,const std::string& source,const PipelineConfig& config,std::istream& in,std::ostream& out,std::ostream& err){
  (void)session;(void)config;
  return astraCompileRun("",source,in,out,err);
}
}
