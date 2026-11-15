#include "tooling/formatter_internal.hpp"

#include "tooling/ast_text.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace a::tooling::detail {

// --------------------------------------------------------- statements

int Formatter::emitStatements(const std::vector<StmtPtr>& body, int level, int limitLine,
                     int headerLine) {
    int lastLine = headerLine;
    for (std::size_t i = 0; i < body.size(); ++i) {
      if (!body[i]) continue;
      const int nextLine = i + 1 < body.size() && body[i + 1]->span.valid()
                               ? body[i + 1]->span.line
                               : limitLine;
      emitStatement(*body[i], level, nextLine, lastLine);
    }
    emitLeftoverComments(limitLine, level, lastLine);
    return lastLine;
  }

void Formatter::emitStatement(const Stmt& statement, int level, int nextLine, int& lastLine) {
    const Span& span = statement.span;

    if (isBlockStatement(statement)) {
      if (span.valid()) emitLeadingComments(span.line, level, lastLine);
      emitBlockStatement(statement, level, nextLine, lastLine);
      attachTrailing(span, lastLine);
      if (span.valid()) lastLine = std::max(lastLine, span.endLine);
      return;
    }

    if (span.valid()) {
      // Hoist comments written inside multiline simple statements so no source
      // text is lost when the expression is normalised.
      emitLeadingComments(span.line, level, lastLine);
      emitInteriorComments(span, level, lastLine);
      ensureGap(span.line, lastLine);
    }

    const std::string text = inlineStatementText(statement, level);
    output_ += indentText(level) + text + '\n';
    if (span.valid()) lastLine = span.endLine;

    attachTrailing(span, lastLine);
    if (span.valid()) lastLine = std::max(lastLine, span.endLine);
  }

void Formatter::emitBlockStatement(const Stmt& statement, int level, int nextLine, int& lastLine) {
    switch (statement.kind) {
      case StmtKind::FnDecl:
        emitFunction(static_cast<const FnDecl&>(statement), level, nextLine, lastLine);
        return;
      case StmtKind::StructDecl:
        emitStruct(static_cast<const StructDecl&>(statement), level, nextLine, lastLine);
        return;
      case StmtKind::EnumDecl:
        emitEnum(static_cast<const EnumDecl&>(statement), level, nextLine, lastLine);
        return;
      case StmtKind::TraitDecl:
        emitTrait(static_cast<const TraitDecl&>(statement), level, nextLine, lastLine);
        return;
      case StmtKind::ImplDecl:
        emitImpl(static_cast<const ImplDecl&>(statement), level, nextLine, lastLine);
        return;
      case StmtKind::If:
        emitIf(static_cast<const IfStmt&>(statement), level, nextLine, lastLine);
        return;
      case StmtKind::While: {
        const auto& loop = static_cast<const WhileStmt&>(statement);
        emitHeader("while " + exprText(*loop.cond, 0, level) + ":", level, statement.span,
                   lastLine);
        lastLine = emitStatements(loop.body, level + 1, nextLine, loop.cond->span.endLine);
        return;
      }
      case StmtKind::For: {
        const auto& loop = static_cast<const ForStmt&>(statement);
        emitHeader("for " + loop.var + " in " + exprText(*loop.iterable, 0, level) + ":",
                   level, statement.span, lastLine);
        lastLine = emitStatements(loop.body, level + 1, nextLine,
                                  loop.iterable->span.endLine);
        return;
      }
      case StmtKind::Loop: {
        const auto& loop = static_cast<const LoopStmt&>(statement);
        emitHeader("loop:", level, statement.span, lastLine);
        lastLine = emitStatements(loop.body, level + 1, nextLine, statement.span.line);
        return;
      }
      case StmtKind::DoWhile: {
        const auto& loop = static_cast<const DoWhileStmt&>(statement);
        emitHeader("do:", level, statement.span, lastLine);
        lastLine = emitStatements(loop.body, level + 1, loop.cond->span.line,
                                  statement.span.line);
        ensureGap(loop.cond->span.line, lastLine);
        output_ += indentText(level) + "while " + exprText(*loop.cond, 0, level) + '\n';
        lastLine = loop.cond->span.endLine;
        return;
      }
      default:
        return;
    }
  }

void Formatter::emitFunction(const FnDecl& decl, int level, int nextLine, int& lastLine) {
    emitDocstring(decl.doc, level);
    std::string header = signatureText(
        decl, [this, level](const Expr& e) { return exprText(e, 0, level + 1); }) + ":";
    emitHeader(header, level, decl.span, lastLine);
    lastLine = emitStatements(decl.body, level + 1, nextLine, decl.span.line);
  }

void Formatter::emitIf(const IfStmt& statement, int level, int nextLine, int& lastLine) {
    for (std::size_t i = 0; i < statement.branches.size(); ++i) {
      const IfStmt::Branch& branch = statement.branches[i];
      const std::string keyword = i == 0 ? "if" : "elif";
      std::string header = keyword + " " + exprText(*branch.cond, 0, level) + ":";
      if (i == 0) {
        emitHeader(header, level, statement.span, lastLine);
      } else {
        ensureGap(branch.cond->span.line, lastLine);
        output_ += indentText(level) + header + '\n';
        lastLine = branch.cond->span.endLine;
      }

      int limit = nextLine;
      if (i + 1 < statement.branches.size()) {
        limit = statement.branches[i + 1].cond->span.line;
      } else if (!statement.elseBody.empty() && statement.elseBody.front()->span.valid()) {
        limit = statement.elseBody.front()->span.line;
      }
      lastLine = emitStatements(branch.body, level + 1, limit, branch.cond->span.endLine);
    }

    if (!statement.elseBody.empty()) {
      const int elseLine = statement.elseBody.front()->span.valid()
                               ? statement.elseBody.front()->span.line
                               : lastLine + 1;
      ensureGap(elseLine, lastLine);
      output_ += indentText(level) + "else:\n";
      lastLine = elseLine;
      lastLine = emitStatements(statement.elseBody, level + 1, nextLine, elseLine);
    }
  }

void Formatter::emitStruct(const StructDecl& decl, int level, int nextLine, int& lastLine) {
    const std::string keyword = declarationKeyword(source_, decl.span, decl.nameSpan);
    std::string header = keyword.empty() ? "class" : keyword;
    header += " " + decl.name + typeParamsText(decl.typeParams);

    std::string parents;
    if (!decl.baseClass.empty()) parents += decl.baseClass;
    if (!decl.metaclass.empty()) {
      if (!parents.empty()) parents += ", ";
      parents += "metaclass=" + decl.metaclass;
    }
    if (!parents.empty()) header += "(" + parents + ")";
    header += ":";
    emitHeader(header, level, decl.span, lastLine);
    emitDocstring(decl.doc, level + 1);

    struct Member {
      const FieldDecl* field = nullptr;
      const FnDecl* method = nullptr;
      int line = 0;
    };
    std::vector<Member> members;
    members.reserve(decl.fields.size() + decl.methods.size());
    for (const FieldDecl& field : decl.fields) {
      members.push_back(Member{&field, nullptr, field.span.line});
    }
    for (const auto& method : decl.methods) {
      if (method) members.push_back(Member{nullptr, method.get(), method->span.line});
    }
    std::stable_sort(members.begin(), members.end(),
                     [](const Member& a, const Member& b) { return a.line < b.line; });

    if (members.empty()) {
      output_ += indentText(level + 1) + "pass\n";
      return;
    }

    for (std::size_t i = 0; i < members.size(); ++i) {
      const Member& member = members[i];
      const int memberNext = i + 1 < members.size() ? members[i + 1].line : nextLine;
      if (member.field) {
        emitLeadingComments(member.line, level + 1, lastLine);
        ensureGap(member.line, lastLine);
        // Untyped fields (dynamic core): emit bare name so output
        // re-parses. A trailing ": " would be rejected by the parser
        // ("Expected a type name, found end of line").
        std::string fieldText = member.field->name;
        if (member.field->type) fieldText += ": " + typeText(member.field->type.get());
        emitDocstring(member.field->doc, level + 1);
        output_ += indentText(level + 1) + fieldText + '\n';
        lastLine = member.field->span.endLine;
        attachTrailing(member.field->span, lastLine);
      } else if (member.method) {
        emitStatement(*member.method, level + 1, memberNext, lastLine);
      }
    }
    emitLeftoverComments(nextLine, level + 1, lastLine);
  }

void Formatter::emitEnum(const EnumDecl& decl, int level, int nextLine, int& lastLine) {
    emitHeader("enum " + decl.name + ":", level, decl.span, lastLine);
    emitDocstring(decl.doc, level + 1);
    if (decl.variants.empty()) {
      output_ += indentText(level + 1) + "pass\n";
      return;
    }
    for (std::size_t i = 0; i < decl.variants.size(); ++i) {
      const EnumVariant& variant = decl.variants[i];
      const int next = i + 1 < decl.variants.size() ? decl.variants[i + 1].span.line : nextLine;
      emitLeadingComments(variant.span.line, level + 1, lastLine);
      ensureGap(variant.span.line, lastLine);
      std::string text = variant.name;
      if (variant.value) text += " = " + exprText(*variant.value, 0, level + 1);
      emitDocstring(variant.doc, level + 1);
      output_ += indentText(level + 1) + text + '\n';
      lastLine = variant.value ? variant.value->span.endLine : variant.span.endLine;
      attachTrailing(variant.span, lastLine);
      (void)next;
    }
    emitLeftoverComments(nextLine, level + 1, lastLine);
  }

void Formatter::emitTrait(const TraitDecl& decl, int level, int nextLine, int& lastLine) {
    std::string header = "trait " + decl.name + typeParamsText(decl.typeParams);
    if (!decl.parents.empty()) {
      header += "(";
      for (std::size_t i = 0; i < decl.parents.size(); ++i) {
        if (i) header += ", ";
        header += decl.parents[i];
      }
      header += ")";
    }
    if (!decl.whereClause.empty()) {
      header += " where ";
      for (std::size_t i = 0; i < decl.whereClause.size(); ++i) {
        if (i) header += ", ";
        header += decl.whereClause[i];
      }
    }
    header += ":";
    emitHeader(header, level, decl.span, lastLine);
    emitDocstring(decl.doc, level + 1);

    if (decl.methods.empty()) {
      output_ += indentText(level + 1) + "pass\n";
      return;
    }

    for (std::size_t i = 0; i < decl.methods.size(); ++i) {
      const TraitSig& sig = decl.methods[i];
      const int next = i + 1 < decl.methods.size() ? decl.methods[i + 1].span.line : nextLine;
      emitLeadingComments(sig.span.line, level + 1, lastLine);
      ensureGap(sig.span.line, lastLine);
      std::string text = "fn " + sig.name +
          paramsText(sig.params, [this, level](const Expr& e) {
            return exprText(e, 0, level + 2);
          });
      if (sig.returnType) text += " -> " + typeText(sig.returnType.get());
      emitDocstring(sig.doc, level + 1);
      output_ += indentText(level + 1) + text + '\n';
      lastLine = sig.span.endLine;
      attachTrailing(sig.span, lastLine);
      (void)next;
    }
    emitLeftoverComments(nextLine, level + 1, lastLine);
  }

void Formatter::emitImpl(const ImplDecl& decl, int level, int nextLine, int& lastLine) {
    emitHeader("impl " + decl.traitName + " for " + decl.targetName + ":", level, decl.span,
               lastLine);
    if (decl.methods.empty()) {
      output_ += indentText(level + 1) + "pass\n";
      return;
    }
    for (std::size_t i = 0; i < decl.methods.size(); ++i) {
      const int next = i + 1 < decl.methods.size() ? decl.methods[i + 1]->span.line : nextLine;
      emitStatement(*decl.methods[i], level + 1, next, lastLine);
    }
    emitLeftoverComments(nextLine, level + 1, lastLine);
  }

std::string Formatter::inlineStatementText(const Stmt& statement, int level) {
    switch (statement.kind) {
      case StmtKind::Let: {
        const auto& decl = static_cast<const LetStmt&>(statement);
        const std::string keyword = declarationKeyword(source_, decl.span, decl.nameSpan);
        (void)keyword;
        std::string text = decl.name;
        if (decl.type) text += ": " + typeText(decl.type.get());
        text += " = " + exprText(*decl.init, 0, level);
        return text;
      }
      case StmtKind::Assign: {
        const auto& assign = static_cast<const AssignStmt&>(statement);
        return exprText(*assign.target, 0, level) + " " + assign.op + " " +
               exprText(*assign.value, 0, level);
      }
      case StmtKind::Expr: {
        const auto& expr = static_cast<const ExprStmt&>(statement);
        if (source_.slice(statement.span) == "pass") return "pass";
        if (!expr.expr) return "pass";
        return exprText(*expr.expr, 0, level);
      }
      case StmtKind::Destructure: {
        const auto& decl = static_cast<const DestructureStmt&>(statement);
        std::string text;
        if (source_.firstChar(decl.span) != '(') text = "var ";
        text += "(";
        for (std::size_t i = 0; i < decl.names.size(); ++i) {
          if (i) text += ", ";
          text += decl.names[i];
        }
        if (decl.names.size() == 1) text += ",";
        text += ") = " + exprText(*decl.value, 0, level);
        return text;
      }
      case StmtKind::Return: {
        const auto& ret = static_cast<const ReturnStmt&>(statement);
        if (!ret.value) return "return";
        return "return " + exprText(*ret.value, 0, level);
      }
      case StmtKind::Break:
        return "break";
      case StmtKind::Continue:
        return "continue";
      case StmtKind::Defer: {
        const auto& defer = static_cast<const DeferStmt&>(statement);
        if (!defer.body) return "defer pass";
        return "defer " + inlineStatementText(*defer.body, level);
      }
      case StmtKind::Import: {
        const auto& import = static_cast<const ImportStmt&>(statement);
        std::string text = "import ";
        const std::string target = source_.slice(import.targetSpan);
        text += target.empty() ? (import.quoted ? "\"" + import.path + "\"" : import.bareName)
                               : target;
        if (!import.alias.empty()) text += " as " + import.alias;
        return text;
      }
      case StmtKind::FromImport: {
        const auto& import = static_cast<const FromImportStmt&>(statement);
        std::string text = "from ";
        const std::string target = source_.slice(import.targetSpan);
        text += target.empty() ? (import.quoted ? "\"" + import.path + "\"" : import.bareName)
                               : target;
        text += " import ";
        for (std::size_t i = 0; i < import.items.size(); ++i) {
          if (i) text += ", ";
          text += import.items[i].name;
          if (!import.items[i].alias.empty() && import.items[i].alias != import.items[i].name) {
            text += " as " + import.items[i].alias;
          }
        }
        return text;
      }
      default:
        return source_.slice(statement.span);
    }
  }

}  // namespace a::tooling::detail
