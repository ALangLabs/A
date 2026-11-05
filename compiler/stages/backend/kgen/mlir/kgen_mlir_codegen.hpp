#pragma once
#include <iosfwd>
#include <string>
#include "name-resolution/module_loader.hpp"
#include "type-checker/sema.hpp"
namespace a::kgen::mlir_backend {
bool emitLLVMIR(const ModuleGraph& g, const SemaResult& s, std::ostream& out, std::ostream& err);
bool buildExecutable(const ModuleGraph& g, const SemaResult& s, const std::string& outputPath, std::ostream& err);
int runModule(const ModuleGraph& g, const SemaResult& s, std::istream& in, std::ostream& out, std::ostream& err);
bool available();
}
