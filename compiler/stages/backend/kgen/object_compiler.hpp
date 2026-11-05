#pragma once
#include <iosfwd>
#include <string>
#include "ast/ast.hpp"
#include "name-resolution/module_loader.hpp"
#include "type-checker/sema.hpp"
namespace a::kgen {
// Object compiler (Mojo ObjectCompiler/KGENToLLVMPipeline-inspired).
class ObjectCompiler {
 public:
  static bool emitIR(const ModuleGraph& g, const SemaResult& s, std::ostream& out);
  static bool build(const ModuleGraph& g, const SemaResult& s, const std::string& outPath, std::ostream& err);
  static int run(const ModuleGraph& g, const SemaResult& s, std::istream& in, std::ostream& out, std::ostream& err);
  static bool supported(const ModuleGraph& g);
};
}
