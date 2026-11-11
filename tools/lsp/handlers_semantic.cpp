#include <set>
#include <tuple>

#include "handlers_common.hpp"

// Semantic tokens: delta-encode [type, modifiers] per token, per the
// legend in initialize. Mirrors pyrefly's semanticTokens.
void LanguageServer::semanticTokens(const Value& id, const Value* params, bool rangeOnly) {
  Document* document = documentFrom(params);
  if (!document || !document->compilation || !params) return replyNull(id);
  const Compilation& compilation = *document->compilation;
  const FileIndex* file = compilation.entryIndex();
  const std::vector<Token>* tokens = compilation.entryTokens();
  if (!file || !tokens) return replyNull(id);
  int rangeStartLine = -1, rangeEndLine = -1;
  if (rangeOnly) {
    const Value* range = params->find("range");
    if (range) {
      const Value* s = range->find("start");
      const Value* e = range->find("end");
      if (s && e) {
        rangeStartLine = static_cast<int>(s->find("line") ? s->find("line")->asInt() : 0) + 1;
        rangeEndLine = static_cast<int>(e->find("line") ? e->find("line")->asInt() : 0) + 1;
      }
    }
  }
  // Legend indices: 0 namespace,1 type,2 class,3 enum,4 function,
  // 5 method,6 parameter,7 variable,8 property,9 keyword,10 comment,
  // 11 string,12 number,13 operator.
  struct LexInfo { int line, col, len, type, mod; };
  std::vector<LexInfo> lexemes;
  auto pushLex = [&](Span span, int type, int mod = 0) {
    if (!span.valid()) return;
    if (rangeOnly && rangeStartLine >= 0 && (span.line < rangeStartLine || span.line > rangeEndLine)) return;
    std::string line = lineText(document->text, span.line);
    int startChar = byteToUtf16Column(line, std::max(0, span.col - 1));
    int endCol = span.endCol > span.col ? span.endCol : span.col + 1;
    if (span.endLine != span.line) endCol = (int)line.size() + 1;
    int len = byteToUtf16Column(line, std::max(0, endCol - 1)) - startChar;
    if (len <= 0) len = 1;
    lexemes.push_back({span.line - 1, startChar, len, type, mod});
  };
  // Precompute definition spans for classification. Keyed by
  // (fileId, line, col) so arbitrarily large files cannot collide.
  std::set<std::tuple<int, int, int>> defKeys;
  auto keyOf = [](Span s) { return std::make_tuple(s.fileId, s.line, s.col); };
  // Symbol kind (+ parameter flag) -> semantic token type index.
  auto tokenTypeFor = [](const SymbolDef& sym) -> int {
    if (sym.parameter) return 6;
    switch (sym.kind) {
      case SymbolKind::Struct: case SymbolKind::Class:
      case SymbolKind::Interface: return 2;
      case SymbolKind::Enum: case SymbolKind::EnumMember: return 3;
      case SymbolKind::Function: return 4;
      case SymbolKind::Method: return 5;
      case SymbolKind::Field: case SymbolKind::Property: return 8;
      case SymbolKind::Module: case SymbolKind::Namespace: case SymbolKind::Package: return 0;
      default: return 7;
    }
  };
  for (const SymbolDef& sym : file->symbols) {
    defKeys.insert(keyOf(sym.span));
    pushLex(sym.span, tokenTypeFor(sym), 1);  // declaration modifier
  }
  for (std::size_t ti = 0; ti < tokens->size(); ++ti) {
    const Token& tok = (*tokens)[ti];
    if (tok.kind == Tok::Eof) continue;
    if (defKeys.count(keyOf(tok.span))) continue;
    int t = -1;
    switch (tok.kind) {
      case Tok::String: t = 11; break;
      case Tok::Int: case Tok::Float: t = 12; break;
      case Tok::Char: t = 11; break;
      case Tok::Plus: case Tok::Minus: case Tok::Star: case Tok::Slash:
      case Tok::Percent: case Tok::EqEq: case Tok::NotEq: case Tok::Lt:
      case Tok::Gt: case Tok::Le: case Tok::Ge: case Tok::Eq: case Tok::Bang:
      case Tok::Question: case Tok::Dot: case Tok::Comma: case Tok::Colon:
      case Tok::QuestionDot: case Tok::QuestionQuestion: t = 13; break;
      default: break;
    }
    if (t >= 0) { pushLex(tok.span, t); continue; }
    // Keywords are lexed as Idents with keyword lexemes in A.
    bool isKw = false;
    for (const char* kw : kKeywords) if (tok.lexeme == kw && tok.kind != Tok::KwSelf) { isKw = true; break; }
    if (isKw) { pushLex(tok.span, 9); continue; }
    if (tok.kind == Tok::Ident || tok.kind == Tok::KwSelf) {
      // Type name? builtin type names classify as `type`.
      if (a::builtinTypeByName(tok.lexeme)) { pushLex(tok.span, 1); continue; }
      if (a::findBuiltinDoc(tok.lexeme)) { pushLex(tok.span, 4); continue; }
      ResolvedSymbol r = a::lsp::resolveToken(compilation, *file, *tokens,
          (int)ti);
      if (r.valid()) {
        pushLex(tok.span, tokenTypeFor(*r.symbol));
        continue;
      }
      // Unresolved identifiers still deserve a token: member names after
      // '.'/'?.' color as property (or method when called), whole-module
      // import aliases as namespace, everything else as variable.
      bool afterDot = ti >= 1 &&
          ((*tokens)[ti - 1].kind == Tok::Dot ||
           (*tokens)[ti - 1].kind == Tok::QuestionDot);
      if (afterDot) {
        bool called = ti + 1 < tokens->size() &&
            (*tokens)[ti + 1].kind == Tok::LParen;
        pushLex(tok.span, called ? 5 : 8);
        continue;
      }
      bool moduleAlias = false;
      for (const ImportDef& imp : file->imports) {
        if (imp.wholeModule && imp.alias == tok.lexeme) { moduleAlias = true; break; }
      }
      pushLex(tok.span, moduleAlias ? 0 : 7);
      continue;
    }
    // Remaining punctuation/operators (parens, brackets, arrows, assign
    // ops, bitwise ops, ...): color as operator. Skip layout tokens.
    if (tok.kind == Tok::Newline || tok.kind == Tok::Indent ||
        tok.kind == Tok::Dedent) {
      continue;
    }
    pushLex(tok.span, 13);
  }
  // Comments get type 10.
  if (const std::vector<a::Comment>* comments = compilation.commentsForModule(
          compilation.graph.entry >= 0 ? compilation.graph.entry : 0)) {
    for (const a::Comment& c : *comments) pushLex(c.span, 10);
  }
  std::sort(lexemes.begin(), lexemes.end(), [](const LexInfo& a, const LexInfo& b) {
    if (a.line != b.line) return a.line < b.line;
    return a.col < b.col;
  });
  Value data = Value::array();
  int prevLine = 0, prevCol = 0;
  for (const LexInfo& lex : lexemes) {
    int deltaLine = lex.line - prevLine;
    int deltaCol = deltaLine == 0 ? lex.col - prevCol : lex.col;
    data.push(deltaLine); data.push(deltaCol); data.push(lex.len);
    data.push(lex.type); data.push(lex.mod);
    prevLine = lex.line; prevCol = lex.col;
  }
  Value result = Value::object();
  result["data"] = std::move(data);
  reply(id, std::move(result));
}

// Inlay hints: inferred type after `let` without annotation, and
// parameter names at call sites with multiple args (ty/pyrefly style).
void LanguageServer::inlayHints(const Value& id, const Value* params) {
  Document* document = documentFrom(params);
  if (!document || !document->compilation || !params) return replyNull(id);
  const Compilation& compilation = *document->compilation;
  const FileIndex* file = compilation.entryIndex();
  if (!file) return replyNull(id);
  const Value* rangeVal = params->find("range");
  int startLine = 0, endLine = INT_MAX;
  if (rangeVal) {
    const Value* s = rangeVal->find("start");
    const Value* e = rangeVal->find("end");
    if (s && e) {
      startLine = static_cast<int>(s->find("line") ? s->find("line")->asInt() : 0);
      endLine = static_cast<int>(e->find("line") ? e->find("line")->asInt() : 0);
    }
  }
  Value hints = Value::array();
  // Inferred types for variables without annotations.
  for (const auto& var : compilation.sema.variables) {
    if (!var.type) continue;
    // Only plain `name = ...` declarations get a type hint — parameters,
    // loop variables, and pattern bindings would flood the view.
    if (!var.variableDecl) continue;
    if (var.span.line <= 0) continue;
    int varLine = var.span.line - 1;
    if (varLine < startLine || varLine > endLine) continue;
    const SymbolDef* sym = nullptr;
    for (const SymbolDef& s : file->symbols) {
      if (s.span.line == var.span.line && s.span.col == var.span.col) { sym = &s; break; }
    }
    if (!sym) continue;
    // Skip if already annotated (detail contains ": ").
    if (sym->detail.find(": ") != std::string::npos) continue;
    std::string t = a::typeName(var.type);
    if (t.empty() || t == "error" || t == "unknown") continue;
    // Skip unresolved type variables (`maybe = None` infers `?`).
    if (t.find("unknown") != std::string::npos || t.find("an ") != std::string::npos) continue;
    Value hint = Value::object();
    Value pos = Value::object();
    std::string lt = lineText(document->text, var.span.line);
    pos["line"] = var.span.line - 1;
    pos["character"] = byteToUtf16Column(lt, std::max(0, var.span.endCol - 1));
    hint["position"] = std::move(pos);
    hint["label"] = ": " + t;
    hint["kind"] = 1;  // Type
    hint["paddingRight"] = true;
    hint["tooltip"] = "Inferred type (from a check)";
    hints.push(std::move(hint));
  }
  // Parameter-name hints at call sites (ty/pyrefly style): `add(x, y)`
  // shows `lhs:`/`rhs:` before positional arguments whose callee resolves
  // to a known function or struct constructor.
  const int entryModule = compilation.graph.entry >= 0 ? compilation.graph.entry : 0;
  for (const a::lsp::CallSite& site : a::lsp::collectCallSites(compilation)) {
    if (site.moduleId != entryModule || site.positionalArgSpans.empty()) continue;
    ResolvedSymbol callee = resolveCallCallee(compilation, site);
    if (!callee.valid()) continue;
    const std::vector<std::string> names = paramNamesForSymbol(compilation, callee);
    if (names.empty()) continue;
    const std::string* moduleText = compilation.textForModule(site.moduleId);
    const std::string& source = moduleText ? *moduleText : document->text;
    const std::vector<Token>* moduleTokens =
        site.moduleId >= 0 &&
                static_cast<std::size_t>(site.moduleId) < compilation.tokensByModule.size()
            ? &compilation.tokensByModule[static_cast<std::size_t>(site.moduleId)]
            : nullptr;
    const std::size_t argCount =
        std::min(names.size(), site.positionalArgSpans.size());
    for (std::size_t i = 0; i < argCount; ++i) {
      const std::string& name = names[i];
      if (name.empty() || name == "_" || name == "self") continue;
      const Span& argSpan = site.positionalArgSpans[i];
      if (!argSpan.valid()) continue;
      // Pylance-style: no hint when the argument is already a bare
      // identifier with the same name as the parameter.
      bool redundant = false;
      if (moduleTokens) {
        for (const Token& argTok : *moduleTokens) {
          if (argTok.kind != Tok::Ident) continue;
          if (argTok.span.line != argSpan.line || argTok.span.col != argSpan.col ||
              argTok.span.endCol != argSpan.endCol) {
            continue;
          }
          redundant = argTok.lexeme == name;
          break;
        }
      }
      if (redundant) continue;
      Value hint = Value::object();
      Value pos = Value::object();
      pos["line"] = argSpan.line - 1;
      pos["character"] =
          byteToUtf16Column(lineText(source, argSpan.line),
                            std::max(0, argSpan.col - 1));
      hint["position"] = std::move(pos);
      hint["label"] = name + ": ";
      hint["kind"] = 2;  // Parameter
      hint["paddingRight"] = false;
      hints.push(std::move(hint));
    }
  }
  Value result = Value::array();
  // Return array of hints directly.
  reply(id, std::move(hints));
}

// Code actions: quick fixes tied to diagnostics (like ty/pyrefly).

}  // namespace a::lsp
