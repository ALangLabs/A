#pragma once
#include <iosfwd>
#include <string>
#include <vector>
namespace a {
struct DriverConfig { bool checkOnly=false,jsonDiagnostics=false,useColor=false; int jobs=1; int optimizationLevel=3; std::string backend="llvm"; std::vector<std::string> modulePaths; std::string stdlibRoot; std::string target; std::string testFilter; };
int runFile(const std::string& path,const DriverConfig& config,std::istream& in,std::ostream& out,std::ostream& err);
int runSource(const std::string& displayPath,std::string source,const DriverConfig& config,std::istream& in,std::ostream& out,std::ostream& err);
int testFile(const std::string& path,const DriverConfig& config,std::istream& in,std::ostream& out,std::ostream& err);
int testPath(const std::string& path,const DriverConfig& config,std::istream& in,std::ostream& out,std::ostream& err);
int repl(const DriverConfig& config,std::istream& in,std::ostream& out,std::ostream& err);
int buildFile(const std::string& path,const std::string& outputPath,const DriverConfig& config,std::ostream& err);

struct FormatFileOptions { bool checkOnly=false,writeInPlace=false; int indentWidth=4; bool useColor=false; };
int formatFile(const std::string& path,const FormatFileOptions& options,std::ostream& out,std::ostream& err);
struct DocFileOptions { bool json=false,useColor=false; std::string outputPath; };
int documentFile(const std::string& path,const DocFileOptions& options,std::ostream& out,std::ostream& err);
}
