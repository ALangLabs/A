#pragma once

#include <functional>
#include <string>
#include <vector>

#include "ast/ast.hpp"
#include "tooling/source_text.hpp"

namespace a::tooling {

// Shared pretty-printers for type expressions and declaration signatures. The
// formatter, documentation generator, and language server all use these so a
// signature renders the same everywhere.
// `expr` renders default parameter values; without it they are omitted.
using ExprTextFn = std::function<std::string(const Expr&)>;
std::string typeText(const TypeExpr* type);
std::string paramText(const ParamDecl& param, const ExprTextFn& expr = {});
std::string paramsText(const std::vector<ParamDecl>& params,
                       const ExprTextFn& expr = {});
std::string typeParamsText(const std::vector<std::string>& typeParams);
std::string signatureText(const FnDecl& decl, const ExprTextFn& expr = {});

// Recovers the declaration keyword that the AST normalizes away (`var`,
// `class`, `struct`, `metaclass`). `declaration` starts the
// statement and `name` starts the declared name.
std::string declarationKeyword(const SourceText& source, const Span& declaration,
                               const Span& name);

}  // namespace a::tooling
