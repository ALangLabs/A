#include "parser/parser_internal.hpp"

namespace a {
StmtPtr Parser::parseIf() {
  Span start = cur().span;
  auto stmt = std::make_unique<IfStmt>();

  expect(Tok::KwIf, "'if'");
  auto cond = parseExpression();
  expect(Tok::Colon, "':' after the condition");
  stmt->branches.push_back(IfStmt::Branch{std::move(cond), parseSuite()});

  while (match(Tok::KwElif)) {
    auto elifCond = parseExpression();
    expect(Tok::Colon, "':' after the condition");
    stmt->branches.push_back(IfStmt::Branch{std::move(elifCond), parseSuite()});
  }

  if (match(Tok::KwElse)) {
    expect(Tok::Colon, "':' after 'else'");
    stmt->elseBody = parseSuite();
  }

  stmt->span = Span::join(start, previousSpan());
  return stmt;
}

StmtPtr Parser::parseWhile() {
  Span start = cur().span;
  expect(Tok::KwWhile, "'while'");
  auto stmt = std::make_unique<WhileStmt>();
  stmt->cond = parseExpression();
  expect(Tok::Colon, "':' after the condition");
  stmt->body = parseSuite();
  stmt->span = Span::join(start, previousSpan());
  return stmt;
}

StmtPtr Parser::parseDoWhile() {
  Span start = cur().span;
  expect(Tok::KwDo, "'do'");
  expect(Tok::Colon, "':' after 'do'");
  auto stmt = std::make_unique<DoWhileStmt>();
  stmt->body = parseSuite();
  expect(Tok::KwWhile, "'while' after the 'do' block");
  stmt->cond = parseExpression();
  expectNewline();
  stmt->span = Span::join(start, previousSpan());
  return stmt;
}

StmtPtr Parser::parseFor() {
  Span start = cur().span;
  expect(Tok::KwFor, "'for'");
  const Token& varTok = expect(Tok::Ident, "a loop variable name");
  expect(Tok::KwIn, "'in' after the loop variable");
  auto stmt = std::make_unique<ForStmt>();
  stmt->var = varTok.lexeme;
  stmt->varSpan = varTok.span;
  stmt->iterable = parseExpression();
  expect(Tok::Colon, "':' after the iterable expression");
  stmt->body = parseSuite();
  stmt->span = Span::join(start, previousSpan());
  return stmt;
}

StmtPtr Parser::parseLoop() {
  Span start = cur().span;
  expect(Tok::KwLoop, "'loop'");
  expect(Tok::Colon, "':' after 'loop'");
  auto stmt = std::make_unique<LoopStmt>();
  stmt->body = parseSuite();
  stmt->span = Span::join(start, previousSpan());
  return stmt;
}

StmtPtr Parser::parseReturn() {
  Span start = cur().span;
  expect(Tok::KwReturn, "'return'");
  auto stmt = std::make_unique<ReturnStmt>();
  if (!check(Tok::Newline) && !check(Tok::Eof) && !check(Tok::Dedent)) {
    stmt->value = parseExpression();
  }
  expectNewline();
  stmt->span = Span::join(start, previousSpan());
  return stmt;
}

StmtPtr Parser::parseBreak() {
  Span start = cur().span;
  expect(Tok::KwBreak, "'break'");
  auto stmt = std::make_unique<BreakStmt>();
  expectNewline();
  stmt->span = Span::join(start, previousSpan());
  return stmt;
}

StmtPtr Parser::parseContinue() {
  Span start = cur().span;
  expect(Tok::KwContinue, "'continue'");
  auto stmt = std::make_unique<ContinueStmt>();
  expectNewline();
  stmt->span = Span::join(start, previousSpan());
  return stmt;
}

StmtPtr Parser::parseDefer() {
  Span start = cur().span;
  expect(Tok::KwDefer, "'defer'");
  auto stmt = std::make_unique<DeferStmt>();
  stmt->body = parseStatement();
  if (!pendingStmts_.empty()) {
    fail(stmt->body->span, "Chained assignment is not allowed in 'defer'.",
         "Assign the names on separate lines inside a block.");
  }
  stmt->span = Span::join(start, previousSpan());
  return stmt;
}

StmtPtr Parser::parseImport() {
  Span start = cur().span;
  expect(Tok::KwImport, "'import'");

  auto stmt = std::make_unique<ImportStmt>();
  auto parseModulePrefix = [&](Span& targetSpan, std::string& out) {
    const Token& kw = expect(Tok::Ident, "'python' or 'std'");
    expect(Tok::Colon, "':' after '" + kw.lexeme + "'");
    auto isNameTok = [](Tok k) {
      return k == Tok::Ident || (k >= Tok::KwFn && k <= Tok::KwImpl);
    };
    std::string mod;
    if (isNameTok(cur().kind)) {
      mod = advance().lexeme;
    } else {
      fail(cur().span, "Expected a module name after '" + kw.lexeme + ":'");
    }
    while (check(Tok::Dot) && isNameTok(peek(1).kind)) { advance(); mod += "." + advance().lexeme; }
    targetSpan = Span::join(kw.span, previousSpan());
    out = std::move(mod);
  };
  if (check(Tok::String)) {
    const Token& pathTok = advance();
    stmt->quoted = true;
    stmt->path = decodeEscapes(pathTok.strValue, pathTok.span);
    stmt->targetSpan = pathTok.span;
  } else if (check(Tok::Ident) && (cur().lexeme == "python" || cur().lexeme == "std") &&
             peek(1).kind == Tok::Colon) {
    if (cur().lexeme == "python") {
      stmt->pythonModuleImport = true;
      parseModulePrefix(stmt->targetSpan, stmt->pythonModule);
    } else {
      stmt->stdModuleImport = true;
      parseModulePrefix(stmt->targetSpan, stmt->stdModule);
      const std::size_t dot = stmt->stdModule.find_last_of('.');
      stmt->bareName = dot == std::string::npos ? stmt->stdModule
                                              : stmt->stdModule.substr(dot + 1);
    }
  } else if (check(Tok::Ident)) {
    const Token& nameTok = advance();
    stmt->bareName = nameTok.lexeme;
    stmt->targetSpan = nameTok.span;
  } else {
    fail(cur().span, "Expected a module path or name after 'import'.",
         "Write import \"lib/util.a\", import math, import std:<package>, or import python:<package>.");
  }

  if (match(Tok::KwAs)) {
    const Token& aliasTok = expect(Tok::Ident, "an alias after 'as'");
    stmt->alias = aliasTok.lexeme;
  }

  expectNewline();
  stmt->span = Span::join(start, previousSpan());
  return stmt;
}

StmtPtr Parser::parseFromImport() {
  Span start = cur().span;
  expect(Tok::KwFrom, "'from'");

  auto stmt = std::make_unique<FromImportStmt>();
  auto parseModulePrefix = [&](Span& targetSpan, std::string& out) {
    const Token& kw = expect(Tok::Ident, "'python' or 'std'");
    expect(Tok::Colon, "':' after '" + kw.lexeme + "'");
    auto isNameTok = [](Tok k) {
      return k == Tok::Ident || (k >= Tok::KwFn && k <= Tok::KwImpl);
    };
    std::string mod;
    if (isNameTok(cur().kind)) {
      mod = advance().lexeme;
    } else {
      fail(cur().span, "Expected a module name after '" + kw.lexeme + ":'");
    }
    while (check(Tok::Dot) && isNameTok(peek(1).kind)) { advance(); mod += "." + advance().lexeme; }
    targetSpan = Span::join(kw.span, previousSpan());
    out = std::move(mod);
  };
  if (check(Tok::String)) {
    const Token& pathTok = advance();
    stmt->quoted = true;
    stmt->path = decodeEscapes(pathTok.strValue, pathTok.span);
    stmt->targetSpan = pathTok.span;
  } else if (check(Tok::Ident) && (cur().lexeme == "python" || cur().lexeme == "std") &&
             peek(1).kind == Tok::Colon) {
    if (cur().lexeme == "python") {
      stmt->pythonModuleImport = true;
      parseModulePrefix(stmt->targetSpan, stmt->pythonModule);
    } else {
      stmt->stdModuleImport = true;
      parseModulePrefix(stmt->targetSpan, stmt->stdModule);
      const std::size_t dot = stmt->stdModule.find_last_of('.');
      stmt->bareName = dot == std::string::npos ? stmt->stdModule
                                              : stmt->stdModule.substr(dot + 1);
    }
  } else if (check(Tok::Ident)) {
    const Token& nameTok = advance();
    stmt->bareName = nameTok.lexeme;
    stmt->targetSpan = nameTok.span;
  } else {
    fail(cur().span, "Expected a module path or name after 'from'.",
         "Write from \"lib/util.a\" import add, from math import sqrt, from std:<pkg> import name, or from python:<pkg> import name.");
  }

  expect(Tok::KwImport, "'import' after the module path");

  while (true) {
    const Token& nameTok = expect(Tok::Ident, "a name to import");
    FromImportStmt::Item item;
    item.name = nameTok.lexeme;
    item.alias = item.name;
    item.span = nameTok.span;
    if (match(Tok::KwAs)) {
      item.alias = expect(Tok::Ident, "an alias after 'as'").lexeme;
    }
    stmt->items.push_back(std::move(item));
    if (!match(Tok::Comma)) break;
    if (check(Tok::Newline) || check(Tok::Eof)) break;
  }

  expectNewline();
  stmt->span = Span::join(start, previousSpan());
  return stmt;
}

StmtPtr Parser::parseSimpleStatement() {
  Span start = cur().span;
  auto target = parseExpression();
  // Unparenthesized tuple targets: `a, b = pair`. A top-level comma after the
  // first expression is only meaningful as a destructuring target here.
  if (check(Tok::Comma)) {
    auto tuple = std::make_unique<TupleExpr>();
    tuple->span = target->span;
    tuple->elements.push_back(std::move(target));
    while (match(Tok::Comma)) {
      if (check(Tok::Newline) || check(Tok::Eof) || check(Tok::Dedent)) break;
      tuple->elements.push_back(parseExpression());
    }
    tuple->span = Span::join(start, previousSpan());
    target = std::move(tuple);
  }
  if (isAssignmentOp(cur().kind)) {
    const Token& opTok = advance();
    auto value = parseExpression();
    // `x = y = v` chained assignment: when the "value" is a plain name
    // followed by '=', it is actually another target. Only '=' chains are
    // supported and only plain-name targets can feed later targets.
    std::vector<ExprPtr> chainTargets;
    std::vector<Span> chainEqSpans;
    while (opTok.kind == Tok::Eq &&
           dynamic_cast<TupleExpr*>(target.get()) == nullptr &&
           dynamic_cast<IdentExpr*>(value.get()) != nullptr &&
           cur().kind == Tok::Eq) {
      chainTargets.push_back(std::move(value));
      chainEqSpans.push_back(advance().span);
      value = parseExpression();
    }
    if (!chainTargets.empty()) {
      // targets = [target] + chainTargets; the last target receives the
      // value, each earlier target copies the next one's name.
      std::vector<std::string> names;
      names.reserve(chainTargets.size());
      for (auto& t : chainTargets) {
        names.push_back(static_cast<IdentExpr*>(t.get())->name);
      }
      std::vector<Span> eqSpans;
      eqSpans.push_back(opTok.span);
      for (auto& s : chainEqSpans) eqSpans.push_back(s);
      // Returned stmt assigns the deepest target first; pending stmts copy
      // names outward so every target ends up with the same value.
      // total targets = [target] + chainTargets (length m); names[i] is the
      // name of total[i+1] for i in 0..m-2.
      auto stmt = std::make_unique<AssignStmt>();
      stmt->span = Span::join(start, previousSpan());
      stmt->op = "=";
      stmt->binop = BinOp::Invalid;
      stmt->opSpan = eqSpans.back();
      stmt->target = std::move(chainTargets.back());
      stmt->value = std::move(value);
      StmtPtr result = std::move(stmt);
      const std::size_t m = chainTargets.size() + 1;
      for (std::size_t i = m - 1; i-- > 0;) {
        auto s = std::make_unique<AssignStmt>();
        s->span = result->span;
        s->op = "=";
        s->binop = BinOp::Invalid;
        s->opSpan = eqSpans[i];
        if (i == 0) {
          s->target = std::move(target);
        } else {
          s->target = std::move(chainTargets[i - 1]);
        }
        auto src = std::make_unique<IdentExpr>();
        src->name = names[i];
        src->span = s->target->span;
        s->value = std::move(src);
        pendingStmts_.push_back(std::move(s));
      }
      expectNewline();
      return result;
    }
    // `a, b = 1, 2`: allow a bare comma tuple on the right for destructuring.
    if (dynamic_cast<TupleExpr*>(target.get()) != nullptr && check(Tok::Comma)) {
      auto rhs = std::make_unique<TupleExpr>();
      rhs->span = value->span;
      rhs->elements.push_back(std::move(value));
      while (match(Tok::Comma)) {
        if (check(Tok::Newline) || check(Tok::Eof) || check(Tok::Dedent)) break;
        rhs->elements.push_back(parseExpression());
      }
      rhs->span = Span::join(start, previousSpan());
      value = std::move(rhs);
    }
    expectNewline();
    if (dynamic_cast<TupleExpr*>(target.get()) != nullptr) {
      if (opTok.kind != Tok::Eq) {
        fail(opTok.span, "Destructuring only supports '='.",
             "Use separate statements to update the bound names.");
      }
      return buildDestructure(std::move(target), std::move(value),
                              /*mutableBinding=*/true,
                              Span::join(start, previousSpan()));
    }
    auto stmt = std::make_unique<AssignStmt>();
    stmt->span = Span::join(start, previousSpan());
    stmt->op = opName(opTok.kind);
    stmt->binop = binOp(opTok.kind);
    stmt->opSpan = opTok.span;
    stmt->target = std::move(target);
    stmt->value = std::move(value);
    return stmt;
  }
  expectNewline();
  auto stmt = std::make_unique<ExprStmt>();
  stmt->span = Span::join(start, previousSpan());
  stmt->expr = std::move(target);
  return stmt;
}

}  // namespace a
