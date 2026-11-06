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
// driver (compiler/astra/): the frontend emits C, `astra` compiles that C
// (plus the A runtime) to machine code and links the executable.

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
// the emitted C links against. Candidates: $A_RUNTIME_C, next to the astra
// binary (staged tcclib copy), the source tree relative to the astra binary
// (build/tcc/astra -> ../../compiler/astra/runtime), and the cwd.
bool findRuntime(const std::string& astra, std::string& runtimeC, std::string& runtimeDir) {
  std::vector<std::string> candidates;
  if (const char* e = std::getenv("A_RUNTIME_C"); e && *e) candidates.push_back(e);
  auto addDir = [&](const std::string& d) {
    if (d.empty()) return;
    candidates.push_back(d + "/a_runtime.c");
    candidates.push_back(d + "/../tcclib/a_runtime.c");
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

// Stages emitted C source to a temp .c file; returns "" on failure.
std::string stageC(const std::string& source, std::ostream& err) {
  std::string p = tempPath("gen", ".c");
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  if (!f) { err << "error: cannot write temp C source '" << p << "'\n"; return ""; }
  f << source;
  return p;
}

// Compiles emitted C (plus the runtime) with astra into outPath.
int astraBuildC(const std::string& cPath, const std::string& outPath, std::ostream& err) {
  std::string astra = findAstra();
  std::string runtimeC, runtimeDir;
  if (!findRuntime(astra, runtimeC, runtimeDir)) {
    err << "error: cannot locate A runtime (compiler/astra/runtime/a_runtime.c); set A_RUNTIME_C\n";
    return 1;
  }
  // Host headers for libc (sys/wait.h, unistd.h, ... used by the test
  // runner in generated C); TinyCC's staged headers alone are not enough.
  int rc = runAstra({"-I" + runtimeDir, "-I/usr/include", "-I/usr/include/x86_64-linux-gnu",
                     "-I/usr/lib/gcc/x86_64-linux-gnu/13/include", "-I/usr/local/include",
                     cPath, runtimeC, "-o", outPath});
  if (rc != 0) { err << "error: astra failed to compile '" << cPath << "'\n"; return 1; }
  return 0;
}

// Locates an executable on PATH; "" when absent.
std::string findOnPath(const char* name){
  const char* path=std::getenv("PATH");
  if(!path||!*path) return "";
  std::stringstream ss(path);
  std::string dir;
  while(std::getline(ss,dir,':')){
    if(dir.empty()) continue;
    std::string c=dir+"/"+name;
    if(access(c.c_str(),X_OK)==0) return c;
  }
  return "";
}

// Locates a system C compiler for optimized `a build` output: $A_CC, then
// the usual names on PATH. "" when none is available.
std::string findCc(){
  if(const char* e=std::getenv("A_CC"); e&&*e) return e;
  for(const char* c: {"cc","gcc","clang"}){
    std::string p=findOnPath(c);
    if(!p.empty()) return p;
  }
  return "";
}

// Compiles emitted C (plus the runtime) with a system C compiler at -O2.
// Returns 0 on success, 2 when no system compiler exists, 1 on failure.
int ccBuildC(const std::string& cPath, const std::string& outPath, std::ostream& err) {
  std::string cc=findCc();
  if(cc.empty()) return 2;
  std::string astra = findAstra();
  std::string runtimeC, runtimeDir;
  if (!findRuntime(astra, runtimeC, runtimeDir)) {
    err << "error: cannot locate A runtime (compiler/astra/runtime/a_runtime.c); set A_RUNTIME_C\n";
    return 1;
  }
  int rc = runChild(cc,{"-O2","-I"+runtimeDir,cPath,runtimeC,"-lm","-o",outPath});
  if (rc != 0) { err << "error: " << cc << " failed to compile '" << cPath << "'\n"; return 1; }
  return 0;
}

// Emits C for a checked program, compiles it with astra, and runs the
// resulting binary. Returns the program's exit code.
int compileAndRun(CompiledModule& m, std::istream& in, std::ostream& out, std::ostream& err,
                  bool testMode = false) {
  (void)in; (void)out;
  std::string cSource, codegenError;
  if (!emitC(*m.graph, *m.sema, cSource, codegenError, testMode)) {
    err << "error: code generation failed: " << codegenError << "\n";
    return kCErr;
  }
  std::string cPath = stageC(cSource, err);
  if (cPath.empty()) return kCErr;
  if (const char* dbg = std::getenv("A_DEBUG_C"); dbg && *dbg) {
    std::error_code dec; std::filesystem::copy_file(cPath, dbg, std::filesystem::copy_options::overwrite_existing, dec);
  }
  std::string bin = tempPath("bin");
  int rc = astraBuildC(cPath, bin, err);
  std::error_code ec;
  std::filesystem::remove(cPath, ec);
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
  // Execution: the frontend emits C, Astra compiles it to a binary, and the
  // binary runs as a child process.
  return compileAndRun(m,in,out,err);
}
int testPipeline(const std::string& path,const PipelineConfig& config,std::istream& in,std::ostream& out,std::ostream& err){
  std::string src; if(!readAll(path,src,err)) return kNoIn;
  DiagnosticSink sink; auto m=runFrontendPipeline(path,std::move(src),config,sink,out,err); if(!m.ok) return kCErr;
  // `a test`: discover and run top-level `fn test_*()` functions.
  return compileAndRun(m,in,out,err,true);
}
int buildPipeline(const std::string& path,const std::string& outputPath,const PipelineConfig& config,std::ostream& err){
  std::string src; if(!readAll(path,src,err)) return kNoIn;
  DiagnosticSink sink; auto m=runFrontendPipeline(path,std::move(src),config,sink,err,err); if(!m.ok) return kCErr;
  std::string cSource, codegenError;
  if(!emitC(*m.graph,*m.sema,cSource,codegenError)){ err<<"error: code generation failed: "<<codegenError<<"\n"; return kCErr; }
  std::string cPath=stageC(cSource,err); if(cPath.empty()) return kCErr;
  // `a build` prefers an optimizing system C compiler; astra (TinyCC) is the
  // fallback for machines without cc/gcc/clang and for `a run`/`a test`.
  int rc=ccBuildC(cPath,outputPath,err);
  if(rc!=0){
    if(rc==1) err<<"note: falling back to astra backend\n";
    rc=astraBuildC(cPath,outputPath,err);
  }
  std::error_code ec; std::filesystem::remove(cPath,ec);
  return rc;
}
int emitIRPipeline(const std::string& path,const PipelineConfig& config,bool optimize,std::ostream& out,std::ostream& err){
  std::string src; if(!readAll(path,src,err)) return kNoIn; DiagnosticSink sink; auto m=runFrontendPipeline(path,std::move(src),config,sink,err,err); if(!m.ok) return kCErr;
  ir::Program p=ir::lowerProgram(*m.graph,*m.sema); if(optimize) a::ir::runAll(p); out<<ir::printProgram(p); return kOk;
}
ReplSession::~ReplSession() = default;
int replEval(ReplSession& session,const std::string& source,const PipelineConfig& config,std::istream& in,std::ostream& out,std::ostream& err){
  (void)session;
  DiagnosticSink sink; auto m=runFrontendPipeline("<repl>",source,config,sink,out,err); if(!m.ok) return 1;
  return compileAndRun(m,in,out,err);
}
}
