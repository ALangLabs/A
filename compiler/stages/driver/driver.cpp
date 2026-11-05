#include "driver/driver.hpp"
#include "driver/version.hpp"
#include <fstream>
#include <istream>
#include <ostream>
#include <sstream>
#include <utility>
#include <vector>
#include <iostream>
#include "diagnostics/diagnostics.hpp"
#include "pipeline/pipeline.hpp"
#include "tooling/docgen.hpp"
#include "tooling/formatter.hpp"
namespace a {
namespace {
PipelineConfig toP(const DriverConfig& c){ PipelineConfig p; p.checkOnly=c.checkOnly; p.jsonDiagnostics=c.jsonDiagnostics; p.useColor=c.useColor; p.modulePaths=c.modulePaths; p.stdlibRoot=c.stdlibRoot; return p; }
bool readAll(const std::string& path,std::string& t,std::ostream& e){ std::ifstream f(path,std::ios::binary); if(!f){e<<"error: cannot open '"<<path<<"'\n"; return false;} std::ostringstream b; b<<f.rdbuf(); t=b.str(); return true; }
}
int runSource(const std::string& n,std::string s,const DriverConfig& c,std::istream& i,std::ostream& o,std::ostream& e){ return executePipeline(n,std::move(s),toP(c),i,o,e); }
int runFile(const std::string& p,const DriverConfig& c,std::istream& i,std::ostream& o,std::ostream& e){ std::string s; if(!readAll(p,s,e)) return 66; return executePipeline(p,std::move(s),toP(c),i,o,e); }
int testFile(const std::string& p,const DriverConfig& c,std::istream& i,std::ostream& o,std::ostream& e){ return testPipeline(p,toP(c),i,o,e); }
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
int repl(const DriverConfig& c,std::istream& in,std::ostream& o,std::ostream& e){
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
int buildFile(const std::string& p,const std::string& out,const DriverConfig& c,const std::string& /*backend*/,std::ostream& e){ return buildPipeline(p,out,toP(c),e); }
int emitLLVMFile(const std::string& p,const std::string& out,const DriverConfig& c,std::ostream& e){ return emitLLVMPipeline(p,out,toP(c),e); }
int emitIRFile(const std::string& p,const DriverConfig& c,bool opt,std::ostream& o,std::ostream& e){ return emitIRPipeline(p,toP(c),opt,o,e); }
int formatFile(const std::string& p,const FormatFileOptions& opt,std::ostream& o,std::ostream& e){
  std::string s; if(!readAll(p,s,e)) return 66; DiagnosticSink sink; tooling::FormatOptions f; f.indentWidth=opt.indentWidth;
  auto r=tooling::formatSource(s,sink,f); sink.flush(e,opt.useColor); if(!r.ok) return 1;
  if(opt.checkOnly){ if(r.text==s) return 0; e<<"error: file needs formatting: "<<p<<"\n"; return 1; }
  if(opt.writeInPlace){ std::ofstream w(p,std::ios::binary|std::ios::trunc); if(!w){e<<"error: cannot write '"<<p<<"'\n"; return 1;} w<<r.text; return 0; }
  o<<r.text; return 0;
}
int documentFile(const std::string& p,const DocFileOptions& opt,std::ostream& o,std::ostream& e){
  std::string s; if(!readAll(p,s,e)) return 66; DiagnosticSink sink; tooling::DocOptions d; d.sourceName=p;
  auto r=tooling::extractDocs(s,sink,d); sink.flush(e,opt.useColor); if(!r.ok) return 1;
  auto txt=opt.json?tooling::renderJson(r.module):tooling::renderMarkdown(r.module);
  if(!opt.outputPath.empty()){ std::ofstream w(opt.outputPath,std::ios::binary|std::ios::trunc); if(!w){e<<"error: cannot write '"<<opt.outputPath<<"'\n"; return 1;} w<<txt; return 0; }
  o<<txt; return 0;
}
}
