#pragma once
#include <iosfwd>
#include <string>
namespace a { struct ModuleGraph; struct SemaResult; namespace ir { struct Program; }
struct ObjectCompileOptions { std::string outputPath; };
class ObjectCompiler {
 public:
  static int compile(const ModuleGraph&, const SemaResult&, const ObjectCompileOptions&, std::ostream& err);
  static int emitLLVM(const ModuleGraph&, const SemaResult&, const std::string& outputPath, std::ostream& err);
  static int emitIRText(const ir::Program&, bool optimize, std::ostream& out);
};
}
