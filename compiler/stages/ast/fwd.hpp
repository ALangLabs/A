#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lexer/span.hpp"

namespace a {

struct Expr;
struct Stmt;
struct TypeExpr;
struct FnDecl;
struct StructDecl;
struct Type;
struct Pattern;
struct MatchArm;
struct CallArg;
struct StringPart;
struct FieldDecl;
struct EnumVariant;
struct TraitSig;
struct ComptimeParam;

using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;
using TypeExprPtr = std::unique_ptr<TypeExpr>;
using TypePtr = std::shared_ptr<Type>;

}  // namespace a
