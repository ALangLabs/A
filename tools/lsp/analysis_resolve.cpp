#include "analysis_internal.hpp"

namespace a::lsp {
const SymbolDef* findTopLevel(const FileIndex& file, const std::string& name) {
  for (const SymbolDef& symbol : file.symbols) {
    if (symbol.topLevel && symbol.name == name) return &symbol;
  }
  return nullptr;
}

// Symbols living in scopes tagged `implTarget == typeName` (impl blocks).
inline const SymbolDef* findImplMember(const FileIndex& index, const std::string& typeName,
                                       const std::string& member) {
  const std::string leaf = lastSegment(typeName);
  for (std::size_t scopeIdx = 0; scopeIdx < index.scopes.size(); ++scopeIdx) {
    const ScopeInfo& scope = index.scopes[scopeIdx];
    if (scope.implTarget.empty() || lastSegment(scope.implTarget) != leaf) continue;
    for (const SymbolDef& symbol : index.symbols) {
      if (symbol.scope == static_cast<int>(scopeIdx) && symbol.name == member) {
        return &symbol;
      }
    }
  }
  return nullptr;
}

inline void collectImplMembers(const FileIndex& index, const std::string& typeName,
                               std::vector<const SymbolDef*>& out) {
  const std::string leaf = lastSegment(typeName);
  for (std::size_t scopeIdx = 0; scopeIdx < index.scopes.size(); ++scopeIdx) {
    const ScopeInfo& scope = index.scopes[scopeIdx];
    if (scope.implTarget.empty() || lastSegment(scope.implTarget) != leaf) continue;
    for (const SymbolDef& symbol : index.symbols) {
      if (symbol.scope == static_cast<int>(scopeIdx)) out.push_back(&symbol);
    }
  }
}

const SymbolDef* findStructMember(const Compilation& compilation, const FileIndex& file,
                                  const std::string& typeName, const std::string& member) {
  auto lookup = [&](const FileIndex& index) -> const SymbolDef* {
    const SymbolDef* typeSymbol = findTopLevel(index, lastSegment(typeName));
    if (typeSymbol && typeSymbol->memberScope >= 0) {
      for (const SymbolDef& symbol : index.symbols) {
        if (symbol.scope == typeSymbol->memberScope && symbol.name == member) return &symbol;
      }
    }
    // `impl Target { ... }` blocks index their methods under a scope tagged
    // with the implemented type rather than the type's own memberScope.
    return findImplMember(index, typeName, member);
  };

  if (const SymbolDef* symbol = lookup(file)) return symbol;
  for (const FileIndex& candidate : compilation.indexes) {
    if (const SymbolDef* symbol = lookup(candidate)) return symbol;
  }
  return nullptr;
}

std::vector<const SymbolDef*> structMembers(const Compilation& compilation,
                                            const FileIndex& file, const std::string& typeName) {
  auto collect = [&](const FileIndex& index, std::vector<const SymbolDef*>& out) {
    const SymbolDef* typeSymbol = findTopLevel(index, lastSegment(typeName));
    if (typeSymbol && typeSymbol->memberScope >= 0) {
      for (const SymbolDef& symbol : index.symbols) {
        if (symbol.scope == typeSymbol->memberScope) out.push_back(&symbol);
      }
    }
    collectImplMembers(index, typeName, out);
  };

  std::vector<const SymbolDef*> members;
  collect(file, members);
  if (!members.empty()) return members;
  for (const FileIndex& candidate : compilation.indexes) {
    collect(candidate, members);
    if (!members.empty()) return members;
  }
  return members;
}

ResolvedSymbol resolveToken(const Compilation& compilation, const FileIndex& file,
                            const std::vector<Token>& tokens, int tokenIndex) {
  if (tokenIndex < 0 || static_cast<std::size_t>(tokenIndex) >= tokens.size()) return {};
  const Token& token = tokens[static_cast<std::size_t>(tokenIndex)];
  // `self` lexes as KwSelf but is semantically the method's receiver
  // parameter; treat it as an identifier so it resolves to its SymbolDef.
  if (token.kind != Tok::Ident && token.kind != Tok::KwSelf) return {};

  if (tokenIndex >= 2) {
    const Tok previous = tokens[static_cast<std::size_t>(tokenIndex - 1)].kind;
    if (previous == Tok::Dot || previous == Tok::QuestionDot) {
      if (ResolvedSymbol member = resolveMember(compilation, file, tokens, tokenIndex);
          member.valid()) {
        return member;
      }
    }
  }

  int scope = scopeAt(file, token.span.line, token.span.col);
  return resolveName(compilation, file, scope, token.lexeme, token.span.line, token.span.col);
}

ResolvedSymbol resolveAt(const Compilation& compilation, const FileIndex& file,
                         const std::vector<Token>& tokens, int line, int byteColumn) {
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    const Token& token = tokens[i];
    if (token.kind != Tok::Ident && token.kind != Tok::KwSelf) continue;
    if (token.span.line != line) continue;
    if (byteColumn < token.span.col || byteColumn > token.span.endCol) continue;
    return resolveToken(compilation, file, tokens, static_cast<int>(i));
  }
  return {};
}

std::vector<const SymbolDef*> visibleSymbols(const Compilation& compilation,
                                             const FileIndex& file, int line, int byteColumn) {
  std::vector<const SymbolDef*> result;
  std::unordered_set<std::string> seen;
  int scope = scopeAt(file, line, byteColumn);
  for (int current = scope; current >= 0;
       current = file.scopes[static_cast<std::size_t>(current)].parent) {
    for (const SymbolDef& symbol : file.symbols) {
      if (symbol.scope != current) continue;
      if (!symbol.hoisted && !startsBeforeOrAt(symbol.span, line, byteColumn)) continue;
      if (!seen.insert(symbol.name).second) continue;
      result.push_back(&symbol);
    }
  }
  for (const ImportDef& import : file.imports) {
    if (!import.wholeModule || !seen.insert(import.alias).second) continue;
  }
  (void)compilation;
  return result;
}
}  // namespace a::lsp
