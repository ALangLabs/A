// Deterministic hover/completion documentation synthesis. See synth_docs.hpp.

#include "synth_docs.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace a::lsp {

namespace {

constexpr std::size_t kMaxListItems = 8;

std::string joinBackticked(const std::vector<std::string>& items, std::size_t cap) {
  std::string out;
  const std::size_t shown = std::min(items.size(), cap);
  for (std::size_t i = 0; i < shown; ++i) {
    if (i) out += ", ";
    out += "`" + items[i] + "`";
  }
  if (items.size() > shown) {
    out += ", … +" + std::to_string(items.size() - shown) + " more";
  }
  return out;
}

// Facts collected by walking a function body once.
struct FnFacts {
  bool returnsValue = false;
  const a::Expr* firstReturnExpr = nullptr;
  std::unordered_set<std::string> params;
  std::set<std::string> mutated;  // sorted for stable output
  std::vector<std::string> calls;  // deduplicated, source order
  std::unordered_set<std::string> seenCalls;
  bool recursive = false;
  bool hasLoop = false;
  bool hasTry = false;
  bool hasLambda = false;
  bool hasComprehension = false;
  bool hasMatch = false;
  bool hasDefer = false;
};

struct FnBodyScan {
  const Compilation& compilation;
  const a::FnDecl& fn;
  FnFacts facts;

  explicit FnBodyScan(const Compilation& c, const a::FnDecl& f) : compilation(c), fn(f) {
    for (const a::ParamDecl& param : fn.params) facts.params.insert(param.name);
  }

  void run() {
    for (const auto& stmt : fn.body) {
      if (stmt) walkStmt(*stmt);
    }
  }

  // Root identifier of an assignment/mutation target chain:
  // `self.total += 1` and `items[0].name = x` both mutate `self`/`items`.
  static const a::Expr* rootExpr(const a::Expr* expr) {
    while (expr) {
      switch (expr->kind) {
        case a::ExprKind::Field:
          expr = static_cast<const a::FieldExpr*>(expr)->object.get();
          continue;
        case a::ExprKind::SafeNav:
          expr = static_cast<const a::SafeNavExpr*>(expr)->object.get();
          continue;
        case a::ExprKind::Index:
          expr = static_cast<const a::IndexExpr*>(expr)->object.get();
          continue;
        case a::ExprKind::Slice:
          expr = static_cast<const a::SliceExpr*>(expr)->object.get();
          continue;
        default:
          return expr;
      }
    }
    return nullptr;
  }

  void noteMutation(const a::Expr* target) {
    const a::Expr* root = rootExpr(target);
    if (!root || root->kind != a::ExprKind::Ident) return;
    const std::string& name = static_cast<const a::IdentExpr*>(root)->name;
    if (facts.params.count(name) || name == "self") facts.mutated.insert(name);
  }

  void noteCall(const a::Expr* callee) {
    std::string name;
    if (callee->kind == a::ExprKind::Ident) {
      name = static_cast<const a::IdentExpr*>(callee)->name;
    } else if (callee->kind == a::ExprKind::Field) {
      name = static_cast<const a::FieldExpr*>(callee)->field;
    } else if (callee->kind == a::ExprKind::SafeNav) {
      name = static_cast<const a::SafeNavExpr*>(callee)->field;
    }
    if (name.empty()) return;
    if (name == fn.name) {
      facts.recursive = true;
      return;
    }
    if (facts.seenCalls.insert(name).second) facts.calls.push_back(name);
  }

  void walkPattern(const a::Pattern& pattern) {
    if (pattern.value) walkExpr(*pattern.value);
    if (pattern.rangeStart) walkExpr(*pattern.rangeStart);
    if (pattern.rangeEnd) walkExpr(*pattern.rangeEnd);
    if (pattern.ctor) walkExpr(*pattern.ctor);
    for (const a::Pattern& sub : pattern.subpatterns) walkPattern(sub);
  }

  void walkExpr(const a::Expr& expr) {
    auto sub = [&](const a::ExprPtr& e) {
      if (e) walkExpr(*e);
    };
    switch (expr.kind) {
      case a::ExprKind::IntLit:
      case a::ExprKind::FloatLit:
      case a::ExprKind::BoolLit:
      case a::ExprKind::CharLit:
        break;
      case a::ExprKind::StringLit:
        for (const a::StringPart& part :
             static_cast<const a::StringLitExpr&>(expr).parts) {
          sub(part.expr);
        }
        break;
      case a::ExprKind::Ident:
        break;
      case a::ExprKind::Unary: {
        const auto& un = static_cast<const a::UnaryExpr&>(expr);
        if (un.op == "++" || un.op == "--") noteMutation(un.operand.get());
        sub(un.operand);
        break;
      }
      case a::ExprKind::Binary: {
        const auto& bin = static_cast<const a::BinaryExpr&>(expr);
        sub(bin.lhs);
        sub(bin.rhs);
        break;
      }
      case a::ExprKind::Call: {
        const auto& call = static_cast<const a::CallExpr&>(expr);
        // Sema marks calls that resolve to runtime builtins; listing
        // `print`/`len` as "calls" adds noise without information.
        if (call.builtinId == a::BuiltinId::None && !call.calleeIsBuiltin) {
          noteCall(call.callee.get());
        }
        sub(call.callee);
        for (const a::CallArg& arg : call.args) sub(arg.value);
        break;
      }
      case a::ExprKind::Index: {
        const auto& e = static_cast<const a::IndexExpr&>(expr);
        sub(e.object);
        sub(e.index);
        break;
      }
      case a::ExprKind::Slice: {
        const auto& e = static_cast<const a::SliceExpr&>(expr);
        sub(e.object);
        sub(e.start);
        sub(e.end);
        sub(e.step);
        break;
      }
      case a::ExprKind::Field:
        sub(static_cast<const a::FieldExpr&>(expr).object);
        break;
      case a::ExprKind::SafeNav:
        sub(static_cast<const a::SafeNavExpr&>(expr).object);
        break;
      case a::ExprKind::ArrayLit:
        for (const auto& el : static_cast<const a::ArrayLitExpr&>(expr).elements) sub(el);
        break;
      case a::ExprKind::MapLit:
        for (const auto& entry : static_cast<const a::MapLitExpr&>(expr).entries) {
          sub(entry.first);
          sub(entry.second);
        }
        break;
      case a::ExprKind::SetLit:
        for (const auto& el : static_cast<const a::SetLitExpr&>(expr).elements) sub(el);
        break;
      case a::ExprKind::Range: {
        const auto& e = static_cast<const a::RangeExpr&>(expr);
        sub(e.start);
        sub(e.end);
        sub(e.step);
        break;
      }
      case a::ExprKind::Ternary: {
        const auto& e = static_cast<const a::TernaryExpr&>(expr);
        sub(e.cond);
        sub(e.thenValue);
        sub(e.elseValue);
        break;
      }
      case a::ExprKind::Tuple:
        for (const auto& el : static_cast<const a::TupleExpr&>(expr).elements) sub(el);
        break;
      case a::ExprKind::Lambda: {
        const auto& e = static_cast<const a::LambdaExpr&>(expr);
        facts.hasLambda = true;
        for (const a::ParamDecl& param : e.params) {
          if (param.defaultValue) sub(param.defaultValue);
        }
        sub(e.body);
        break;
      }
      case a::ExprKind::Comprehension: {
        const auto& e = static_cast<const a::ComprehensionExpr&>(expr);
        facts.hasComprehension = true;
        sub(e.element);
        sub(e.iterable);
        sub(e.condition);
        break;
      }
      case a::ExprKind::Match: {
        const auto& e = static_cast<const a::MatchExpr&>(expr);
        facts.hasMatch = true;
        sub(e.subject);
        for (const a::MatchArm& arm : e.arms) {
          walkPattern(arm.pattern);
          sub(arm.guard);
          sub(arm.result);
        }
        break;
      }
      case a::ExprKind::Try: {
        facts.hasTry = true;
        sub(static_cast<const a::TryExpr&>(expr).operand);
        break;
      }
    }
  }

  void walkBlock(const std::vector<a::StmtPtr>& body) {
    for (const auto& stmt : body) {
      if (stmt) walkStmt(*stmt);
    }
  }

  void walkStmt(const a::Stmt& stmt) {
    switch (stmt.kind) {
      case a::StmtKind::Let: {
        const auto& s = static_cast<const a::LetStmt&>(stmt);
        if (s.init) walkExpr(*s.init);
        break;
      }
      case a::StmtKind::Assign: {
        const auto& s = static_cast<const a::AssignStmt&>(stmt);
        noteMutation(s.target.get());
        if (s.target) walkExpr(*s.target);
        if (s.value) walkExpr(*s.value);
        break;
      }
      case a::StmtKind::Expr:
        if (static_cast<const a::ExprStmt&>(stmt).expr) {
          walkExpr(*static_cast<const a::ExprStmt&>(stmt).expr);
        }
        break;
      case a::StmtKind::Destructure:
        if (static_cast<const a::DestructureStmt&>(stmt).value) {
          walkExpr(*static_cast<const a::DestructureStmt&>(stmt).value);
        }
        break;
      case a::StmtKind::Return: {
        const auto& s = static_cast<const a::ReturnStmt&>(stmt);
        if (s.value) {
          facts.returnsValue = true;
          if (!facts.firstReturnExpr) facts.firstReturnExpr = s.value.get();
          walkExpr(*s.value);
        }
        break;
      }
      case a::StmtKind::Defer:
        if (static_cast<const a::DeferStmt&>(stmt).body) {
          facts.hasDefer = true;
          walkStmt(*static_cast<const a::DeferStmt&>(stmt).body);
        }
        break;
      case a::StmtKind::If: {
        const auto& s = static_cast<const a::IfStmt&>(stmt);
        for (const auto& branch : s.branches) {
          if (branch.cond) walkExpr(*branch.cond);
          walkBlock(branch.body);
        }
        walkBlock(s.elseBody);
        break;
      }
      case a::StmtKind::While: {
        const auto& s = static_cast<const a::WhileStmt&>(stmt);
        facts.hasLoop = true;
        if (s.cond) walkExpr(*s.cond);
        walkBlock(s.body);
        break;
      }
      case a::StmtKind::DoWhile: {
        const auto& s = static_cast<const a::DoWhileStmt&>(stmt);
        facts.hasLoop = true;
        walkBlock(s.body);
        if (s.cond) walkExpr(*s.cond);
        break;
      }
      case a::StmtKind::For: {
        const auto& s = static_cast<const a::ForStmt&>(stmt);
        facts.hasLoop = true;
        if (s.iterable) walkExpr(*s.iterable);
        walkBlock(s.body);
        break;
      }
      case a::StmtKind::Loop: {
        facts.hasLoop = true;
        walkBlock(static_cast<const a::LoopStmt&>(stmt).body);
        break;
      }
      // Nested declarations own their bodies; their calls/mutations are
      // not this function's behaviour.
      case a::StmtKind::FnDecl:
      case a::StmtKind::StructDecl:
      case a::StmtKind::EnumDecl:
      case a::StmtKind::TraitDecl:
      case a::StmtKind::ImplDecl:
      case a::StmtKind::Break:
      case a::StmtKind::Continue:
      case a::StmtKind::Import:
      case a::StmtKind::FromImport:
      case a::StmtKind::TypeAlias:
        break;
    }
  }
};

bool isVoidLike(const std::string& type) {
  return type.empty() || type == "void" || type == "unknown" || type == "error";
}

std::string functionDoc(const Compilation& compilation, const a::FnDecl& fn) {
  FnBodyScan scan(compilation, fn);
  scan.run();
  const FnFacts& facts = scan.facts;

  std::string out;
  auto line = [&](const std::string& text) {
    if (!out.empty()) out += "\n";
    out += text;
  };

  // Return type: annotation first, then Sema's inferred signature, then
  // the type of the first returned expression.
  std::string ret;
  if (fn.returnType) ret = typeExprText(fn.returnType.get());
  if (ret.empty() || ret == "?") {
    const auto it = compilation.sema.fnReturnTypes.find(&fn);
    if (it != compilation.sema.fnReturnTypes.end() && it->second) {
      ret = a::typeName(it->second);
    }
  }
  if (ret.empty() && facts.firstReturnExpr) {
    a::TypePtr t = facts.firstReturnExpr->inferredType;
    if (!t) {
      const auto it = compilation.sema.exprTypes.find(facts.firstReturnExpr);
      if (it != compilation.sema.exprTypes.end()) t = it->second;
    }
    if (t) ret = a::typeName(t);
  }
  if (ret == "void") {
    line("**Returns:** nothing.");
  } else if (!isVoidLike(ret)) {
    line("**Returns:** `" + ret + "`");
  }

  if (!facts.mutated.empty()) {
    std::vector<std::string> names(facts.mutated.begin(), facts.mutated.end());
    line("**Mutates:** " + joinBackticked(names, kMaxListItems));
  }
  if (!facts.calls.empty()) {
    line("**Calls:** " + joinBackticked(facts.calls, kMaxListItems));
  }

  std::vector<std::string> paramNotes;
  for (const a::ParamDecl& param : fn.params) {
    if (param.defaultValue) paramNotes.push_back(param.name);
  }
  if (!paramNotes.empty()) {
    line("**Parameters with defaults:** " + joinBackticked(paramNotes, kMaxListItems));
  }
  for (const a::ParamDecl& param : fn.params) {
    if (param.variadic) {
      line("**Variadic parameter:** `*" + param.name + "`");
      break;
    }
  }

  std::vector<std::string> notes;
  if (facts.recursive) notes.push_back("recursive");
  if (facts.hasLoop) notes.push_back("contains loops");
  if (facts.hasTry) notes.push_back("propagates errors with `?`");
  if (facts.hasLambda) notes.push_back("defines lambdas");
  if (facts.hasComprehension) notes.push_back("uses comprehensions");
  if (facts.hasMatch) notes.push_back("uses match expressions");
  if (facts.hasDefer) notes.push_back("uses defer");
  if (!notes.empty()) {
    std::string joined;
    for (std::size_t i = 0; i < notes.size(); ++i) {
      if (i) joined += ", ";
      joined += notes[i];
    }
    line("**Notes:** " + joined + ".");
  }

  if (out.empty()) return {};
  out += "\n\n*Auto-generated summary — no docstring.*";
  return out;
}

std::string namesLine(const char* label, const std::vector<std::string>& names) {
  if (names.empty()) return {};
  return std::string("**") + label + "** " + joinBackticked(names, kMaxListItems);
}

std::string structDoc(const a::StructDecl& decl) {
  std::vector<std::string> sections;
  if (!decl.baseClass.empty()) sections.push_back("**Extends:** `" + decl.baseClass + "`");
  std::vector<std::string> fields;
  for (const a::FieldDecl& field : decl.fields) {
    std::string text = field.name;
    if (field.type) {
      const std::string t = typeExprText(field.type.get());
      if (!t.empty() && t != "?") text += ": " + t;
    }
    fields.push_back(text);
  }
  if (!fields.empty()) {
    sections.push_back("**Fields:** " + joinBackticked(fields, kMaxListItems));
  }
  std::vector<std::string> methods;
  for (const auto& method : decl.methods) {
    if (method) methods.push_back(method->name);
  }
  const std::string methodLine = namesLine("Methods:", methods);
  if (!methodLine.empty()) sections.push_back(methodLine);
  if (sections.empty()) return {};
  std::string out;
  for (const std::string& s : sections) {
    if (!out.empty()) out += "\n";
    out += s;
  }
  out += "\n\n*Auto-generated summary — no docstring.*";
  return out;
}

}  // namespace

std::string synthesizeSymbolDoc(const Compilation& compilation,
                                const ResolvedSymbol& resolved) {
  if (!resolved.valid()) return {};
  switch (resolved.symbol->kind) {
    case SymbolKind::Function:
    case SymbolKind::Method:
    case SymbolKind::Struct:
    case SymbolKind::Class:
    case SymbolKind::Enum:
    case SymbolKind::Interface:
      break;
    default:
      return {};
  }
  const a::Stmt* decl = findDeclAt(compilation, resolved.file->path,
                                   resolved.symbol->span.line,
                                   resolved.symbol->span.col);
  if (!decl) return {};

  std::string out;
  switch (decl->kind) {
    case a::StmtKind::FnDecl:
      out = functionDoc(compilation, static_cast<const a::FnDecl&>(*decl));
      break;
    case a::StmtKind::StructDecl:
      out = structDoc(static_cast<const a::StructDecl&>(*decl));
      break;
    case a::StmtKind::EnumDecl: {
      const auto& e = static_cast<const a::EnumDecl&>(*decl);
      std::vector<std::string> variants;
      for (const a::EnumVariant& v : e.variants) variants.push_back(v.name);
      const std::string line = namesLine("Variants:", variants);
      if (!line.empty()) {
        out = line;
        if (e.isAlgebraic()) out += "\n*Algebraic enum — variants may carry payloads.*";
        out += "\n\n*Auto-generated summary — no docstring.*";
      }
      break;
    }
    case a::StmtKind::TraitDecl: {
      const auto& t = static_cast<const a::TraitDecl&>(*decl);
      std::vector<std::string> sections;
      if (!t.parents.empty()) sections.push_back(namesLine("Extends:", t.parents));
      std::vector<std::string> methods;
      for (const a::TraitSig& m : t.methods) methods.push_back(m.name);
      const std::string req = namesLine("Requires:", methods);
      if (!req.empty()) sections.push_back(req);
      for (std::size_t i = 0; i < sections.size(); ++i) {
        if (i) out += "\n";
        out += sections[i];
      }
      if (!out.empty()) out += "\n\n*Auto-generated summary — no docstring.*";
      break;
    }
    case a::StmtKind::ImplDecl: {
      const auto& impl = static_cast<const a::ImplDecl&>(*decl);
      out = "**Implements:** `" + impl.traitName + "` for `" + impl.targetName + "`";
      std::vector<std::string> methods;
      for (const auto& m : impl.methods) {
        if (m) methods.push_back(m->name);
      }
      const std::string methodLine = namesLine("Methods:", methods);
      if (!methodLine.empty()) out += "\n" + methodLine;
      out += "\n\n*Auto-generated summary — no docstring.*";
      break;
    }
    case a::StmtKind::TypeAlias: {
      const auto& alias = static_cast<const a::TypeAliasDecl&>(*decl);
      if (alias.type) {
        const std::string t = typeExprText(alias.type.get());
        if (!t.empty() && t != "?") {
          out = "**Alias for:** `" + t + "`\n\n*Auto-generated summary — no docstring.*";
        }
      }
      break;
    }
    default:
      break;
  }
  return out;
}

}  // namespace a::lsp
