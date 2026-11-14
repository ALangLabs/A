#include "driver/driver.hpp"
#include "driver/version.hpp"
#include "exe_path.hpp"
#include <fstream>
#include <istream>
#include <ostream>
#include <sstream>
#include <utility>
#include <vector>
#include <memory>
#include <iostream>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include "diagnostics/diagnostics.hpp"
#include "pipeline/pipeline.hpp"
#include "name-resolution/module_loader.hpp"
#include "tooling/docgen.hpp"
#include "tooling/formatter.hpp"
#include <filesystem>
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif
namespace a {
namespace {
PipelineConfig toP(const DriverConfig& c){ PipelineConfig p; p.checkOnly=c.checkOnly; p.jsonDiagnostics=c.jsonDiagnostics; p.useColor=c.useColor; p.jobs=c.jobs; p.optimizationLevel=c.optimizationLevel; p.modulePaths=c.modulePaths; p.stdlibRoot=c.stdlibRoot; p.target=c.target; p.testFilter=c.testFilter; p.backend=c.backend; return p; }
bool readAll(const std::string& path,std::string& t,std::ostream& e){
  std::error_code ec;
  if(std::filesystem::is_directory(path,ec)){ e<<"error: '"<<path<<"' is a directory, not a source file\n"; return false; }
  std::ifstream f(path,std::ios::binary); if(!f){e<<"error: cannot open '"<<path<<"'\n"; return false;} std::ostringstream b; b<<f.rdbuf(); t=b.str(); return true;
}
}
int runSource(const std::string& n,std::string s,const DriverConfig& c,std::istream& i,std::ostream& o,std::ostream& e){ return executePipeline(n,std::move(s),toP(c),i,o,e); }
int runFile(const std::string& p,const DriverConfig& c,std::istream& i,std::ostream& o,std::ostream& e){ std::string s; if(!readAll(p,s,e)) return 66; return executePipeline(p,std::move(s),toP(c),i,o,e); }
int testFile(const std::string& p,const DriverConfig& c,std::istream& i,std::ostream& o,std::ostream& e){ return testPipeline(p,toP(c),i,o,e); }
int testPath(const std::string& p,const DriverConfig& c,std::istream& i,std::ostream& o,std::ostream& e){ return a::testPath(p,toP(c),i,o,e); }
namespace {
// Heuristic for Python-style continuation: unclosed brackets or a trailing
// ':' block opener mean more input is expected. Strings and '#' comments are
// skipped so brackets inside them don't count.
bool replInputComplete(const std::string& text){
  int parens=0,squares=0,braces=0;
  bool inString=false; char quote=0; bool escaped=false; bool comment=false;
  std::istringstream lines(text); std::string line;
  int pendingColonIndent=-1;
  auto indentOf=[](const std::string& l){ int n=0; while(n<(int)l.size()&&(l[n]==' '||l[n]=='\t'))++n; return n; };
  while(std::getline(lines,line)){
    comment=false;
    for(size_t i=0;i<line.size();++i){
      char c=line[i];
      if(comment) break;
      if(inString){ if(escaped){escaped=false;} else if(c=='\\')escaped=true; else if(c==quote)inString=false; continue; }
      if(c=='#'){comment=true;break;}
      if(c=='"'||c=='\''){inString=true;quote=c;continue;}
      if(c=='(')++parens; else if(c==')')--parens;
      else if(c=='[')++squares; else if(c==']')--squares;
      else if(c=='{')++braces; else if(c=='}')--braces;
    }
    // strip comment+strings crudely for the ':' check: use raw line end
    std::string code=line;
    size_t hash=code.find('#'); if(hash!=std::string::npos) code.resize(hash);
    while(!code.empty()&&(code.back()==' '||code.back()=='\t'||code.back()=='\r')) code.pop_back();
    if(code.empty()) continue;
    int ind=indentOf(line);
    if(pendingColonIndent>=0){
      if(ind>pendingColonIndent){ pendingColonIndent=-1; }
      else { pendingColonIndent=-1; /* block never opened: treat ':' line as complete attempt, compiler will diagnose */ }
    }
    if(!code.empty()&&code.back()==':') pendingColonIndent=ind;
  }
  return parens<=0&&squares<=0&&braces<=0&&pendingColonIndent<0&&!inString;
}
}
#if defined(__unix__) || defined(__APPLE__)
// Locates the self-hosted A REPL (stdlib/std/repl/main.a): $A_STDLIB, then
// paths relative to this executable (build/a -> ../stdlib), then cwd.
std::string findReplMain(const DriverConfig& c){
  std::vector<std::string> roots;
  if(!c.stdlibRoot.empty()) roots.push_back(c.stdlibRoot);
  if(const char* e=std::getenv("A_STDLIB"); e&&*e) roots.push_back(e);
  const std::string self=exePath();
  if(!self.empty()){
    std::string dir=self; auto s=dir.find_last_of('/'); if(s!=std::string::npos) dir.resize(s);
    roots.push_back(dir+"/../stdlib");
    roots.push_back(dir+"/../../stdlib");
  }
  roots.push_back("stdlib");
  roots.push_back(".");
  for(const auto& r:roots){
    const std::string ext = std::string(kSourceExt);
    const std::string legacy = std::string(kLegacySourceExt);
    const std::vector<std::string> replRels = {"/std/repl/main."+ext,"/repl/main."+ext,
                                              "/std/repl/main."+legacy,"/repl/main."+legacy};
    for(const auto& rel: replRels){
      std::string p=r+rel; std::error_code ec;
      if(std::filesystem::exists(p,ec)) return p;
    }
  }
  return "";
}
#endif
int repl(const DriverConfig& c,std::istream& in,std::ostream& o,std::ostream& e){
#if defined(__unix__) || defined(__APPLE__)
  // On a TTY, `a repl` runs the self-hosted interactive shell written in A
  // (stdlib/std/repl/main.a). Piped stdin keeps the simple line REPL below.
  if(isatty(STDIN_FILENO)){
    std::string main=findReplMain(c);
    if(!main.empty()) return runFile(main,c,in,o,e);
    e<<"warning: could not locate stdlib/std/repl/main.a; using line REPL\n";
  }
#endif
  // Line-based REPL: each complete input is compiled and run by the LLVM
  // toolchain as a fresh process (top-level bindings do not persist).
  o<<"A REPL "<<kVersion<<" — 'exit'/'quit' or Ctrl-D to leave\n";
  ReplSession session;
  PipelineConfig pc=toP(c); pc.checkOnly=false;
  std::string sessionSource, pending;
  std::string line;
  while(true){
    o<<(pending.empty()?">>> ":"... ")<<std::flush;
    if(!std::getline(in,line)) break;
    if(pending.empty()&&(line=="exit"||line=="quit")) break;
    pending+=line; pending+='\n';
    if(!replInputComplete(pending)) continue;
    std::string attempt=sessionSource+pending;
    int rc=replEval(session,attempt,pc,in,o,e);
    if(rc==1){ pending.clear(); continue; } // compile error: discard, don't poison session
    sessionSource=std::move(attempt);
    pending.clear();
    (void)rc;
  }
  o<<"\n";
  return 0;
}
int buildFile(const std::string& p,const std::string& out,const DriverConfig& c,std::ostream& e){ return buildPipeline(p,out,toP(c),e); }
int formatFile(const std::string& p,const FormatFileOptions& opt,std::ostream& o,std::ostream& e){
  std::string s; if(!readAll(p,s,e)) return 66; DiagnosticSink sink;
  // formatSource lexes/parses with fileId 0; register the file so parse
  // errors carry `path:line:col` like `a check` instead of a bare message.
  sink.addFile(std::make_shared<SourceFile>(p,s));
  tooling::FormatOptions f; f.indentWidth=opt.indentWidth;
  auto r=tooling::formatSource(s,sink,f); sink.flush(e,opt.useColor); if(!r.ok) return 1;
  if(opt.checkOnly){ if(r.text==s) return 0; e<<"error: file needs formatting: "<<p<<"\n"; return 1; }
  if(opt.writeInPlace){ std::ofstream w(p,std::ios::binary|std::ios::trunc); if(!w){e<<"error: cannot write '"<<p<<"'\n"; return 1;} w<<r.text; return 0; }
  o<<r.text; return 0;
}
int documentFile(const std::string& p,const DocFileOptions& opt,std::ostream& o,std::ostream& e){
  std::string s; if(!readAll(p,s,e)) return 66; DiagnosticSink sink;
  sink.addFile(std::make_shared<SourceFile>(p,s));  // fileId 0, see formatFile
  tooling::DocOptions d; d.sourceName=p;
  auto r=tooling::extractDocs(s,sink,d); sink.flush(e,opt.useColor); if(!r.ok) return 1;
  auto txt=opt.json?tooling::renderJson(r.module):tooling::renderMarkdown(r.module);
  if(!opt.outputPath.empty()){ std::ofstream w(opt.outputPath,std::ios::binary|std::ios::trunc); if(!w){e<<"error: cannot write '"<<opt.outputPath<<"'\n"; return 1;} w<<txt; return 0; }
  o<<txt; return 0;
}
}
