#include "tooling/docgen_internal.hpp"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <map>
#include <sstream>
#include <utility>

#include "ast/ast.hpp"
#include "lexer/lexer.hpp"
#include "parser/parser.hpp"
#include "tooling/ast_text.hpp"
#include "tooling/source_text.hpp"
namespace a::tooling {
namespace {
std::string moduleStem(const std::string& path) {
  std::size_t slash = path.find_last_of("/\\");
  std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  std::size_t dot = name.find_last_of('.');
  if (dot != std::string::npos && dot > 0) name = name.substr(0, dot);
  return name.empty() ? "module" : name;
}

std::string trimText(std::string text) {
  std::size_t begin = 0;
  while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
  std::size_t end = text.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
  return text.substr(begin, end - begin);
}

// Python inspect.cleandoc equivalent: drops leading/trailing blank lines,
// strips the first line's indentation, and removes the common indentation
// shared by the remaining lines.
std::string cleanDocstring(const std::string& raw) {
  std::vector<std::string> lines;
  {
    std::istringstream in(raw);
    std::string line;
    while (std::getline(in, line)) lines.push_back(line);
  }
  auto blank = [](const std::string& l) { return trimText(l).empty(); };
  std::size_t begin = 0;
  std::size_t end = lines.size();
  while (begin < end && blank(lines[begin])) ++begin;
  while (end > begin && blank(lines[end - 1])) --end;
  if (begin >= end) return {};

  std::string first = lines[begin];
  const std::size_t fb = first.find_first_not_of(" \t");
  first = fb == std::string::npos ? "" : first.substr(fb);

  std::size_t minIndent = std::string::npos;
  for (std::size_t i = begin + 1; i < end; ++i) {
    if (blank(lines[i])) continue;
    const std::size_t nb = lines[i].find_first_not_of(" \t");
    if (nb != std::string::npos && (minIndent == std::string::npos || nb < minIndent)) {
      minIndent = nb;
    }
  }

  std::string result = first;
  for (std::size_t i = begin + 1; i < end; ++i) {
    std::string l = lines[i];
    if (minIndent != std::string::npos && l.size() >= minIndent) l = l.substr(minIndent);
    result += "\n" + l;
  }
  return trimText(result);
}

// Python-style docstring: a bare string-literal statement at the top of a
// function body.
std::string fnBodyDocstring(const FnDecl& decl) {
  if (decl.body.empty()) return {};
  std::string text;
  if (!docstringText(*decl.body.front(), text)) return {};
  return text;
}

// Documentation comes from `"""..."""` docstrings only: the body docstring,
// an attached docstring recorded by the parser, or a docstring statement
// directly above the declaration. Candidates are tried in order.
std::string pickDoc(std::initializer_list<std::string> docstrings) {
  for (const std::string& candidate : docstrings) {
    const std::string cleaned = cleanDocstring(candidate);
    if (!cleaned.empty()) return cleaned;
  }
  return {};
}

DocItem functionItem(const FnDecl& decl, const std::string& aboveDoc = {}) {
  DocItem item;
  item.kind = DocItem::Kind::Function;
  item.name = decl.name;
  item.signature = signatureText(decl);
  item.doc = pickDoc({fnBodyDocstring(decl), decl.doc, aboveDoc});
  item.returnType = typeText(decl.returnType.get());
  for (const ParamDecl& param : decl.params) {
    DocParameter documented;
    documented.name = param.name;
    documented.type = typeText(param.type.get());
    item.params.push_back(std::move(documented));
  }
  return item;
}

std::string structSignature(const StructDecl& decl, const std::string& keyword) {
  std::string text = keyword + " " + decl.name + typeParamsText(decl.typeParams);
  std::string parents;
  if (!decl.baseClass.empty()) parents += decl.baseClass;
  if (!decl.metaclass.empty()) {
    if (!parents.empty()) parents += ", ";
    parents += "metaclass=" + decl.metaclass;
  }
  if (!parents.empty()) text += "(" + parents + ")";
  return text;
}

DocItem structItem(const StructDecl& decl, const SourceText& source,
                   const std::string& aboveDoc = {}) {
  DocItem item;
  const std::string keyword = declarationKeyword(source, decl.span, decl.nameSpan);
  if (keyword == "metaclass") {
    item.kind = DocItem::Kind::Metaclass;
  } else {
    item.kind = DocItem::Kind::Class;
  }
  item.name = decl.name;
  item.signature = structSignature(decl, keyword.empty() ? "class" : keyword);
  item.doc = pickDoc({decl.doc, aboveDoc});

  for (const FieldDecl& field : decl.fields) {
    DocMember member;
    member.name = field.name;
    member.type = typeText(field.type.get());
    member.signature = field.name + ": " + member.type;
    member.doc = pickDoc({field.doc});
    item.fields.push_back(std::move(member));
  }
  for (const auto& method : decl.methods) {
    if (!method) continue;
    item.methods.push_back(functionItem(*method));
  }
  return item;
}

DocItem enumItem(const EnumDecl& decl, const std::string& aboveDoc = {}) {
  DocItem item;
  item.kind = DocItem::Kind::Enum;
  item.name = decl.name;
  item.signature = "enum " + decl.name;
  item.doc = pickDoc({decl.doc, aboveDoc});
  for (const EnumVariant& variant : decl.variants) {
    DocMember member;
    member.name = variant.name;
    member.signature = variant.name;
    member.doc = pickDoc({variant.doc});
    item.variants.push_back(std::move(member));
  }
  return item;
}

DocItem traitItem(const TraitDecl& decl, const std::string& aboveDoc = {}) {
  DocItem item;
  item.kind = DocItem::Kind::Trait;
  item.name = decl.name;
  std::string signature = "trait " + decl.name + typeParamsText(decl.typeParams);
  if (!decl.parents.empty()) {
    signature += "(";
    for (std::size_t i = 0; i < decl.parents.size(); ++i) {
      if (i) signature += ", ";
      signature += decl.parents[i];
    }
    signature += ")";
  }
  item.signature = signature;
  item.doc = pickDoc({decl.doc, aboveDoc});

  for (const TraitSig& sig : decl.methods) {
    DocItem method;
    method.kind = DocItem::Kind::Function;
    method.name = sig.name;
    method.signature = "fn " + sig.name + paramsText(sig.params);
    if (sig.returnType) method.signature += " -> " + typeText(sig.returnType.get());
    method.doc = pickDoc({sig.doc});
    method.returnType = typeText(sig.returnType.get());
    for (const ParamDecl& param : sig.params) {
      DocParameter documented;
      documented.name = param.name;
      documented.type = typeText(param.type.get());
      method.params.push_back(std::move(documented));
    }
    item.methods.push_back(std::move(method));
  }
  return item;
}

DocItem letItem(const LetStmt& decl, const SourceText& source,
                bool topLevel, const std::string& aboveDoc = {}) {
  DocItem item;
  const std::string keyword = declarationKeyword(source, decl.span, decl.nameSpan);
  item.name = decl.name;
  item.kind = DocItem::Kind::Variable;
  item.isPrivate = false;
  (void)keyword;
  (void)topLevel;

  std::string signature;
  signature = decl.name;
  if (decl.type) signature += ": " + typeText(decl.type.get());
  item.signature = signature;
  item.doc = pickDoc({aboveDoc});
  item.returnType = typeText(decl.type.get());
  return item;
}


}  // namespace

DocResult extractDocs(const std::string& source, DiagnosticSink& sink,
                      const DocOptions& options) {
  DocResult result;

  Lexer lexer(source, sink, 0);
  std::vector<Token> tokens = lexer.tokenize();
  if (sink.hasErrors()) return result;

  Parser parser(std::move(tokens), sink);
  std::unique_ptr<Program> program = parser.parseProgram();
  if (!program || sink.hasErrors()) return result;

  SourceText sourceView(source);

  result.module.name = options.moduleName.empty() ? moduleStem(options.sourceName)
                                                  : options.moduleName;
  result.module.path = options.sourceName;

  // Python-style docstrings: a leading string-literal statement documents the
  // module, and one directly above a declaration documents that declaration.
  std::string moduleDocstring;
  std::string pendingDoc;
  bool pendingDocSet = false;
  bool firstStatement = true;

  for (const auto& statement : program->statements) {
    if (!statement) continue;
    std::string docstring;
    if (docstringText(*statement, docstring)) {
      if (firstStatement) {
        moduleDocstring = std::move(docstring);
      } else {
        pendingDoc = std::move(docstring);
        pendingDocSet = true;
      }
      firstStatement = false;
      continue;
    }
    firstStatement = false;
    const std::string aboveDoc = pendingDocSet ? pendingDoc : "";
    pendingDocSet = false;
    pendingDoc.clear();
    switch (statement->kind) {
      case StmtKind::FnDecl:
        result.module.items.push_back(
            functionItem(static_cast<const FnDecl&>(*statement), aboveDoc));
        break;
      case StmtKind::StructDecl:
        result.module.items.push_back(structItem(
            static_cast<const StructDecl&>(*statement), sourceView, aboveDoc));
        break;
      case StmtKind::EnumDecl:
        result.module.items.push_back(
            enumItem(static_cast<const EnumDecl&>(*statement), aboveDoc));
        break;
      case StmtKind::TraitDecl:
        result.module.items.push_back(
            traitItem(static_cast<const TraitDecl&>(*statement), aboveDoc));
        break;
      case StmtKind::TypeAlias: {
        const auto& alias = static_cast<const TypeAliasDecl&>(*statement);
        DocItem item;
        item.kind = DocItem::Kind::TypeAlias;
        item.name = alias.name;
        item.signature = "type " + alias.name + " = " + typeText(alias.type.get());
        item.doc = pickDoc({aboveDoc});
        result.module.items.push_back(std::move(item));
        break;
      }
      case StmtKind::Let:
        result.module.items.push_back(letItem(
            static_cast<const LetStmt&>(*statement), sourceView, true, aboveDoc));
        break;
      case StmtKind::Expr: {
        const auto& expr = static_cast<const ExprStmt&>(*statement);
        if (sourceView.slice(statement->span) == "pass") break;
        (void)expr;
        break;
      }
      default:
        break;
    }
  }

  if (result.module.doc.empty()) result.module.doc = cleanDocstring(moduleDocstring);

  result.ok = true;
  return result;
}
}  // namespace a::tooling
