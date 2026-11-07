#include "pipeline/pipeline.hpp"
#include <algorithm>
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
#include "codegen/codegen.hpp"
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
// driver (compiler/astra/): the frontend emits x86-64 assembly, `astra`
// compiles/assembles that assembly together with the C A runtime
// (a_runtime.c) to machine code and links the executable.

// Locates the astra driver: A_ASTRA override, then the binary staged with
// the runtime (build/astralib/astra or next to this executable), then PATH.
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
      for(const char* rel: {"/astra/astra","/astralib/../astra/astra","/astralib/astra","/astra"}){
        std::string c=dir+rel;
        if(access(c.c_str(),X_OK)==0) return c;
      }
    }
  }
  return "astra";  // fall back to PATH lookup in execvp
}

// fork/exec `prog` with the given arguments; the child inherits our
// stdin/stdout/stderr. Returns the exit code, 127 on exec failure.
int runChild(const std::string& prog, const std::vector<std::string>& args){
  std::vector<char*> argv;
  argv.push_back(const_cast<char*>(prog.c_str()));
  for(const auto& a:args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  pid_t pid=fork();
  if(pid<0) return 127;
  if(pid==0){ execvp(prog.c_str(),argv.data()); _exit(127); }
  int status=0;
  while(waitpid(pid,&status,0)<0){ if(errno!=EINTR) return 127; }
  if(WIFEXITED(status)) return WEXITSTATUS(status);
  return 128+WTERMSIG(status);
}

// fork/exec `astra` with the given arguments; the child inherits our
// stdin/stdout/stderr. Returns the exit code, 127 on exec failure.
int runAstra(const std::vector<std::string>& args){
  return runChild(findAstra(),args);
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

// Locates the A runtime C source (compiler/astra/runtime/a_runtime.c) that
// is compiled/linked with the emitted assembly. Candidates: $A_RUNTIME_C, next to the astra
// binary (staged astralib copy), the source tree relative to the astra binary
// (build/astra/astra -> ../../compiler/astra/runtime), and the cwd.
bool findRuntime(const std::string& astra, std::string& runtimeC, std::string& runtimeDir) {
  std::vector<std::string> candidates;
  if (const char* e = std::getenv("A_RUNTIME_C"); e && *e) candidates.push_back(e);
  auto addDir = [&](const std::string& d) {
    if (d.empty()) return;
    candidates.push_back(d + "/a_runtime.c");
    candidates.push_back(d + "/../astralib/a_runtime.c");
    candidates.push_back(d + "/../../compiler/astra/runtime/a_runtime.c");
    candidates.push_back(d + "/../../../compiler/astra/runtime/a_runtime.c");
  };
  auto slash = astra.find_last_of('/');
  if (slash != std::string::npos) addDir(astra.substr(0, slash));
  else addDir(".");
  candidates.push_back("compiler/astra/runtime/a_runtime.c");
  candidates.push_back("./compiler/astra/runtime/a_runtime.c");
  for (const auto& c : candidates) {
    std::error_code ec;
    if (std::filesystem::exists(c, ec)) {
      runtimeC = std::filesystem::absolute(c, ec).string();
      auto pos = runtimeC.find_last_of('/');
      runtimeDir = pos == std::string::npos ? "." : runtimeC.substr(0, pos);
      return true;
    }
  }
  return false;
}

// Stages emitted source to a temp file with the given extension.
std::string stageSrc(const std::string& source, const char* ext, std::ostream& err) {
  std::string p = tempPath("gen", ext);
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  if (!f) { err << "error: cannot write temp source '" << p << "'\n"; return ""; }
  f << source;
  if (const char* dbg = std::getenv("A_DEBUG_SRC"); dbg && *dbg) {
    std::error_code ec;
    std::filesystem::copy_file(p, dbg, std::filesystem::copy_options::overwrite_existing, ec);
  }
  return p;
}
// Compiles emitted assembly (plus the runtime modules) with astra into
// outPath. The runtime is modular: every *.c next to the located
// a_runtime.c (dispatch/printing plus the domain .c modules) is compiled
// and linked in. optimizationLevel maps to the clang-style -O flag.
int astraBuild(const std::string& srcPath, const std::string& outPath, std::ostream& err, int optimizationLevel = 2) {
  std::string astra = findAstra();
  std::string runtimeC, runtimeDir;
  if (!findRuntime(astra, runtimeC, runtimeDir)) {
    err << "error: cannot locate A runtime (compiler/astra/runtime/a_runtime.c); set A_RUNTIME_C\n";
    return 1;
  }
  std::vector<std::string> rtSrcs;
  if (const char* e = std::getenv("A_RUNTIME_C"); e && *e) {
    rtSrcs.push_back(runtimeC);
  } else {
    std::error_code ec;
    for (const auto& de : std::filesystem::directory_iterator(runtimeDir, ec)) {
      if (de.path().extension() == ".c") rtSrcs.push_back(de.path().string());
    }
    if (rtSrcs.empty()) rtSrcs.push_back(runtimeC);
    else std::sort(rtSrcs.begin(), rtSrcs.end());
  }
  std::string optFlag = "-O" + std::to_string(optimizationLevel < 0 ? 0 : (optimizationLevel > 3 ? 3 : optimizationLevel));
  std::vector<std::string> args = {optFlag, "-I" + runtimeDir, "-I/usr/include", "-I/usr/include/x86_64-linux-gnu",
                     "-I/usr/lib/gcc/x86_64-linux-gnu/13/include", "-I/usr/local/include",
                     srcPath};
  args.insert(args.end(), rtSrcs.begin(), rtSrcs.end());
  args.push_back("-o"); args.push_back(outPath);
  int rc = runAstra(args);
  if (rc != 0) { err << "error: astra failed to compile '" << srcPath << "'\n"; return 1; }
  return 0;
}

// Emits assembly for a checked program, assembles+links it with astra, and
// runs the resulting binary. Returns the program's exit code.
int compileAndRun(CompiledModule& m, std::istream& in, std::ostream& out, std::ostream& err,
                  bool testMode = false, int optimizationLevel = 2) {
  (void)in; (void)out;
  // A -> asm -> machine code: the asm backend is the only codegen path.
  std::string sSource, sError;
  if (!emitAsm(*m.graph, *m.sema, sSource, sError, testMode)) {
    err << "error: asm code generation failed: " << sError << "\n";
    return kCErr;
  }
  std::string sPath = stageSrc(sSource, ".s", err);
  if (sPath.empty()) return kCErr;
  std::string bin = tempPath("bin");
  int rc = astraBuild(sPath, bin, err, optimizationLevel);
  std::error_code ec;
  std::filesystem::remove(sPath, ec);
  if (rc != 0) { std::filesystem::remove(bin, ec); return kCErr; }
  rc = runBinary(bin);
  std::filesystem::remove(bin, ec);
  return rc < 0 ? kCErr : rc;
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
  // Execution: the frontend emits x86-64 assembly, Astra assembles/compiles it
  // with the runtime into a binary, and the binary runs as a child process.
  return compileAndRun(m,in,out,err,false,config.optimizationLevel);
}
int testPipeline(const std::string& path,const PipelineConfig& config,std::istream& in,std::ostream& out,std::ostream& err){
  std::string src; if(!readAll(path,src,err)) return kNoIn;
  DiagnosticSink sink; auto m=runFrontendPipeline(path,std::move(src),config,sink,out,err); if(!m.ok) return kCErr;
  // `a test`: discover and run top-level `fn test_*()` functions.
  return compileAndRun(m,in,out,err,true,config.optimizationLevel);
}
int buildPipeline(const std::string& path,const std::string& outputPath,const PipelineConfig& config,std::ostream& err){
  std::string src; if(!readAll(path,src,err)) return kNoIn;
  DiagnosticSink sink; auto m=runFrontendPipeline(path,std::move(src),config,sink,err,err); if(!m.ok) return kCErr;
  // `a build`: A -> asm -> machine code via astra (the only backend).
  std::string sSource, sError;
  if(!emitAsm(*m.graph,*m.sema,sSource,sError)){
    err<<"error: asm code generation failed: "<<sError<<"\n"; return kCErr;
  }
  std::string sPath=stageSrc(sSource,".s",err); if(sPath.empty()) return kCErr;
  int rc=astraBuild(sPath,outputPath,err,config.optimizationLevel);
  std::error_code ec; std::filesystem::remove(sPath,ec);
  return rc;
}
int emitIRPipeline(const std::string& path,const PipelineConfig& config,bool optimize,std::ostream& out,std::ostream& err){
  std::string src; if(!readAll(path,src,err)) return kNoIn; DiagnosticSink sink; auto m=runFrontendPipeline(path,std::move(src),config,sink,err,err); if(!m.ok) return kCErr;
  ir::Program p=ir::lowerProgram(*m.graph,*m.sema);
  if(optimize){ ir::OptLevel level = ir::OptLevel::O2; ir::optLevelFromInt(config.optimizationLevel, level); a::ir::runOptPipeline(p, level); }
  out<<ir::printProgram(p); return kOk;
}
ReplSession::~ReplSession() = default;
int replEval(ReplSession& session,const std::string& source,const PipelineConfig& config,std::istream& in,std::ostream& out,std::ostream& err){
  (void)session;
  DiagnosticSink sink; auto m=runFrontendPipeline("<repl>",source,config,sink,out,err); if(!m.ok) return 1;
  return compileAndRun(m,in,out,err,false,config.optimizationLevel);
}
}
