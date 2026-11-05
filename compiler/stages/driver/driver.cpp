#include "driver/driver.hpp"
#include <fstream>
#include <istream>
#include <ostream>
#include <sstream>
#include <utility>
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
int repl(const DriverConfig& c,std::istream& in,std::ostream& o,std::ostream& e){
  (void)in; o<<"A REPL (v2)\n";
  std::string line;
  while(o<<">>> "<<std::flush && std::getline(in,line)){
    if(line=="exit"||line=="quit") break;
    if(line.empty()) continue;
    std::ostringstream out,err;
    executePipeline("<repl>", line+"\n", toP(c), in, out, err);
    o<<out.str(); e<<err.str();
  }
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
