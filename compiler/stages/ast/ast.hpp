#pragma once

// Umbrella header for the AST module. Kept for backward compatibility so all
// existing `#include "ast/ast.hpp"` sites continue to work. The definitions
// are split into focused headers under `ast/`.
#include "ast/builtins.hpp"
#include "ast/exprs.hpp"
#include "ast/fwd.hpp"
#include "ast/kinds.hpp"
#include "ast/stmts.hpp"
#include "ast/type_exprs.hpp"
