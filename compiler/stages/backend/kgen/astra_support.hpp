#pragma once

#include "ast/ast.hpp"
#include "name-resolution/module_loader.hpp"

namespace a::detail {
bool isNativeSupported(const ModuleGraph& graph);
bool exprProvablyFloat(const Expr& expr);
}  // namespace a::detail

namespace a {
// True when the module uses only constructs the Astra native subset can lower.
bool isNativeSupported(const ModuleGraph& graph);
// Legacy name kept for call-site compatibility during backend migration.
inline bool isLLVMSupported(const ModuleGraph& graph) { return isNativeSupported(graph); }
}  // namespace a
