#pragma once

#include "analysis.hpp"
#include "name-resolution/builtin_docs.hpp"
#include "name-resolution/native_modules.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace a::lsp {

namespace fs = std::filesystem;

inline const Span kWholeFileSpan{1, 1, INT_MAX / 2, 1, 0};

inline bool startsBeforeOrAt(const Span& span, int line, int col) {
  if (!span.valid()) return false;
  if (line != span.line) return line > span.line;
  return col >= span.col;
}

inline bool startsAfter(const Span& a, const Span& b) {
  if (a.line != b.line) return a.line > b.line;
  return a.col > b.col;
}

inline bool spanContains(const Span& span, int line, int col) {
  if (!span.valid()) return false;
  const bool afterStart = line > span.line || (line == span.line && col >= span.col);
  const bool beforeEnd = line < span.endLine || (line == span.endLine && col <= span.endCol);
  return afterStart && beforeEnd;
}

inline Span blockSpan(const std::vector<StmtPtr>& body, const Span& fallback) {
  if (body.empty()) return fallback;
  Span span = body.front()->span;
  if (body.back()->span.valid()) {
    span.endLine = body.back()->span.endLine;
    span.endCol = body.back()->span.endCol;
  }
  return span;
}

inline std::string readFileText(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return {};
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

inline std::string canonicalPath(const std::string& path) {
  std::error_code error;
  fs::path canonical = fs::weakly_canonical(path, error);
  if (!error) return canonical.string();
  error.clear();
  fs::path absolute = fs::absolute(path, error);
  return error ? path : absolute.string();
}

inline std::string moduleStem(const std::string& path) {
  std::size_t slash = path.find_last_of("/\\");
  std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  std::size_t dot = name.find_last_of('.');
  if (dot != std::string::npos && dot > 0) name = name.substr(0, dot);
  for (char& c : name) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') c = '_';
  }
  if (name.empty()) name = "module";
  return name;
}

inline std::string importAlias(const std::string& explicitAlias, const std::string& path,
                        const std::string& bareName) {
  if (!explicitAlias.empty()) return explicitAlias;
  if (!path.empty()) return moduleStem(path);
  return bareName;
}

inline std::string lastSegment(const std::string& name) {
  std::size_t dot = name.find_last_of('.');
  return dot == std::string::npos ? name : name.substr(dot + 1);
}

inline std::string exprTypeName(const Expr* expr) {
  if (!expr) return {};
  switch (expr->kind) {
    case ExprKind::IntLit: return "i32";
    case ExprKind::FloatLit: return "f64";
    case ExprKind::BoolLit: return "bool";
    case ExprKind::CharLit: return "char";
    case ExprKind::StringLit: return "String";
    case ExprKind::ArrayLit: return "array";
    case ExprKind::MapLit: return "map";
    case ExprKind::SetLit: return "set";
    case ExprKind::Tuple: return "tuple";
    case ExprKind::Lambda: return "fn";
    case ExprKind::Call: {
      const auto& call = static_cast<const CallExpr&>(*expr);
      if (call.callee && call.callee->kind == ExprKind::Ident) {
        const auto& ident = static_cast<const IdentExpr&>(*call.callee);
        if (!ident.name.empty() && std::isupper(static_cast<unsigned char>(ident.name[0]))) {
          return ident.name;
        }
      }
      return {};
    }
    default:
      return {};
  }
}

inline std::string varDetail(const std::string& name, const std::string& type, bool mutableBinding,
                      bool constant) {
  std::string detail;
  if (constant) {
    detail = "const " + name;
  } else {
    detail = (mutableBinding ? "var " : "") + name;
  }
  if (!type.empty()) detail += ": " + type;
  return detail;
}

inline int scopeAt(const FileIndex& file, int line, int col) {
  int best = 0;
  int bestDepth = -1;
  for (std::size_t i = 0; i < file.scopes.size(); ++i) {
    const ScopeInfo& scope = file.scopes[i];
    if (!spanContains(scope.span, line, col)) continue;
    if (scope.depth > bestDepth) {
      bestDepth = scope.depth;
      best = static_cast<int>(i);
    }
  }
  return best;
}

inline const SymbolDef* findGlobalTopLevel(const Compilation& compilation, const std::string& name) {
  for (const FileIndex& file : compilation.indexes) {
    if (const SymbolDef* symbol = findTopLevel(file, name)) return symbol;
  }
  return nullptr;
}

inline ResolvedSymbol findModuleMember(const Compilation& compilation, int moduleId,
                                const std::string& name) {
  const FileIndex* file = compilation.indexForModule(moduleId);
  if (!file) return {};
  const SymbolDef* symbol = findTopLevel(*file, name);
  if (!symbol) return {};
  return ResolvedSymbol{symbol, file};
}

inline ResolvedSymbol resolveName(const Compilation& compilation, const FileIndex& file, int scope,
                           const std::string& name, int line, int col) {
  for (int current = scope; current >= 0;
       current = file.scopes[static_cast<std::size_t>(current)].parent) {
    const SymbolDef* best = nullptr;
    for (const SymbolDef& symbol : file.symbols) {
      if (symbol.scope != current || symbol.name != name) continue;
      if (!symbol.hoisted && !startsBeforeOrAt(symbol.span, line, col)) continue;
      if (!best || startsAfter(symbol.span, best->span)) best = &symbol;
    }
    if (best) return ResolvedSymbol{best, &file};
  }

  for (const ImportDef& import : file.imports) {
    if (import.wholeModule || import.alias != name || import.moduleId < 0) continue;
    if (ResolvedSymbol resolved = findModuleMember(compilation, import.moduleId, import.member);
        resolved.valid()) {
      return resolved;
    }
  }

  if (const SymbolDef* symbol = findGlobalTopLevel(compilation, name)) {
    for (const FileIndex& candidate : compilation.indexes) {
      for (const SymbolDef& local : candidate.symbols) {
        if (&local == symbol) return ResolvedSymbol{&local, &candidate};
      }
    }
  }
  return {};
}

inline ResolvedSymbol resolveMember(const Compilation& compilation, const FileIndex& file,
                             const std::vector<Token>& tokens, int memberTokenIndex) {
  if (memberTokenIndex < 2) return {};
  const Token& baseToken = tokens[static_cast<std::size_t>(memberTokenIndex - 2)];
  const Token& memberToken = tokens[static_cast<std::size_t>(memberTokenIndex)];
  if (baseToken.kind != Tok::Ident) return {};

  for (const ImportDef& import : file.imports) {
    if (!import.wholeModule || import.alias != baseToken.lexeme || import.moduleId < 0) continue;
    if (ResolvedSymbol resolved =
            findModuleMember(compilation, import.moduleId, memberToken.lexeme);
        resolved.valid()) {
      return resolved;
    }
  }

  ResolvedSymbol base = resolveToken(compilation, file, tokens, memberTokenIndex - 2);
  if (base.valid() && !base.symbol->typeName.empty()) {
    if (const SymbolDef* member =
            findStructMember(compilation, *base.file, base.symbol->typeName, memberToken.lexeme)) {
      return ResolvedSymbol{member, base.file};
    }
  }

  for (const FileIndex& candidate : compilation.indexes) {
    for (const SymbolDef& symbol : candidate.symbols) {
      if (symbol.memberScope < 0) continue;
      if (symbol.kind != SymbolKind::Struct && symbol.kind != SymbolKind::Class &&
          symbol.kind != SymbolKind::Enum) {
        continue;
      }
      if (const SymbolDef* member =
              findStructMember(compilation, candidate, symbol.name, memberToken.lexeme)) {
        return ResolvedSymbol{member, &candidate};
      }
    }
  }
  return {};
}


}  // namespace a::lsp
