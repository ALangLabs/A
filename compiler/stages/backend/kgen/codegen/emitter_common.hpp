#pragma once

#include <string>

#include "ast/ast.hpp"
#include "name-resolution/module_loader.hpp"

namespace a::detail {

// True for stdlib/prelude modules: they use language features the native
// subset cannot lower and are never emitted, so they must not gate user
// programs either. The entry module is user code even when its path happens
// to contain a "stdlib" directory component.
bool isStdlibModule(const ModuleGraph& graph, const Module& mod);

// Concatenates the literal parts of a string literal (interpolated
// expressions are excluded).
std::string extractString(const StringLitExpr& s);

}  // namespace a::detail
