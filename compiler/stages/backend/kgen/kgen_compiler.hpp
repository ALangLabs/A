#pragma once
#include <string>
#include "ast/ast.hpp"
#include "name-resolution/module_loader.hpp"
#include "type-checker/sema.hpp"
namespace a::kgen {
struct CompileOptions {
  std::string targetTriple;
  int optLevel = 2;
  bool emitLLVMIR = false;
  std::string outputPath;
};
}
