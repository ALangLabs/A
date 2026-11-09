#include "parser/parser_internal.hpp"

namespace a {

void Parser::parseTypeParams(std::vector<std::string>& typeParams,
                             std::vector<std::string>& bounds,
                             std::vector<Span>& boundSpans) {
  // Current token is '<'.
  advance();
  while (!check(Tok::Gt) && !check(Tok::Eof)) {
    const Token& tp = expect(Tok::Ident, "a type parameter name");
    typeParams.push_back(tp.lexeme);
    if (match(Tok::Colon)) {
      // Inline trait bounds: `T: Speak` or `T: Speak + Sized`. Comma still
      // separates type parameters, so `+` is the bound separator (as in
      // Rust); each bound name lands in the declaration's constraint list.
      while (true) {
        const Token& bound = expect(Tok::Ident, "a trait name after ':'");
        bounds.push_back(bound.lexeme);
        boundSpans.push_back(bound.span);
        if (!match(Tok::Plus)) break;
      }
    }
    if (!match(Tok::Comma)) break;
  }
  expect(Tok::Gt, "'>' after type parameters");
}

StmtPtr Parser::parseFnDecl(bool entryKw) {
  Span start = cur().span;
  if (entryKw) {
    advance();  // consume 'entry'
  } else {
    expect(Tok::KwFn, "'fn'");
  }
  const Token& nameTok = expect(Tok::Ident, "a function name");

  auto decl = std::make_unique<FnDecl>();
  decl->name = nameTok.lexeme;
  decl->nameSpan = nameTok.span;
  decl->isEntry = entryKw;

  if (check(Tok::Lt)) {
    parseTypeParams(decl->typeParams, decl->whereClause, decl->whereSpans);
  }

  expect(Tok::LParen, "'(' after the function name");
  if (!check(Tok::RParen)) {
    bool seenVariadic = false;
    while (true) {
      if (check(Tok::KwComptime)) {
        // Compile-time parameter: `comptime N: int`. Recorded for future
        // specialization; treated as a typed value inside the body today.
        Span paramSpan = cur().span;
        advance();
        ComptimeParam param;
        const Token& nameTok2 = expect(Tok::Ident, "a compile-time parameter name");
        param.name = nameTok2.lexeme;
        param.span = paramSpan;
        if (match(Tok::Colon)) param.type = parseTypeExpr();
        decl->comptimeParams.push_back(std::move(param));
      } else {
        ParamDecl pd = parseParam(true);
        if (seenVariadic) {
          fail(pd.span, "No parameters may follow a variadic '*" + pd.name + "' parameter.");
        }
        if (pd.variadic) seenVariadic = true;
        decl->params.push_back(std::move(pd));
      }
      if (!match(Tok::Comma)) break;
      if (check(Tok::RParen)) break;
    }
  }
  expect(Tok::RParen, "')' after the parameter list");

  if (match(Tok::Arrow)) {
    decl->returnType = parseTypeExpr();
  }

  // Trait constraints: `fn f[T](x: T) -> bool where Equatable:`
  if (match(Tok::KwWhere)) {
    decl->whereSpan = previousSpan();
    while (true) {
      const Token& wTok = expect(Tok::Ident, "a trait name after 'where'");
      decl->whereClause.push_back(wTok.lexeme);
      decl->whereSpans.push_back(wTok.span);
      if (!match(Tok::Comma)) break;
    }
  }

  // Allow either ':' or direct '=' (expression body) without requiring ':'
  if (match(Tok::Colon)) {
    if (match(Tok::Eq)) {
      auto value = parseExpression();
      auto ret = std::make_unique<ReturnStmt>();
      ret->span = spanOf(value);
      ret->value = std::move(value);
      expectNewline();
      decl->body.push_back(std::move(ret));
    } else {
      decl->body = parseSuite();
    }
  } else if (match(Tok::Eq)) {
    auto value = parseExpression();
    auto ret = std::make_unique<ReturnStmt>();
    ret->span = spanOf(value);
    ret->value = std::move(value);
    expectNewline();
    decl->body.push_back(std::move(ret));
  } else {
    fail(cur().span, "Expected ':' or '=' after function signature.");
  }

  decl->span = Span::join(start, previousSpan());
  return decl;
}

ParamDecl Parser::parseParam(bool allowDefault) {
  ParamDecl param;
  if (allowDefault && check(Tok::Star)) {
    // Variadic parameter: `*args` or `*args: T` (T is the element type; the
    // runtime value is a list of T). Must be the last parameter.
    advance();
    param.variadic = true;
    const Token& nameTok = expect(Tok::Ident, "a parameter name after '*'");
    param.name = nameTok.lexeme;
    param.span = nameTok.span;
    if (match(Tok::Colon)) {
      param.type = parseTypeExpr();
    }
    return param;
  }
  if (check(Tok::KwSelf)) {
    param.span = cur().span;
    param.name = "self";
    advance();
  } else {
    const Token& nameTok = expect(Tok::Ident, "a parameter name");
    param.name = nameTok.lexeme;
    param.span = nameTok.span;
  }
  if (match(Tok::Colon)) {
    param.type = parseTypeExpr();
  }
  if (check(Tok::Eq)) {
    if (param.name == "self" || !allowDefault) {
      fail(cur().span, "Default parameter values are not supported here.");
    }
    advance();
    param.defaultValue = parseExpression();
  }
  return param;
}

StmtPtr Parser::parseStructDecl() {
  Span start = cur().span;
  advance(); // consume class or metaclass

  const Token& nameTok = expect(Tok::Ident, "a class name");

  auto decl = std::make_unique<StructDecl>();
  decl->name = nameTok.lexeme;
  decl->nameSpan = nameTok.span;

  if (match(Tok::Lt)) {
    while (!check(Tok::Gt) && !check(Tok::Eof)) {
      const Token& tp = expect(Tok::Ident, "a type parameter name");
      decl->typeParams.push_back(tp.lexeme);
      if (!match(Tok::Comma)) break;
    }
    expect(Tok::Gt, "'>' after type parameters");
  }

  // Support class(BaseClass) ...
  if (match(Tok::LParen)) {
    while (!check(Tok::RParen) && !check(Tok::Eof)) {
      if (check(Tok::Ident) && peek(1).kind == Tok::Eq) {
        std::string kw = advance().lexeme;
        advance(); // consume '='
        const Token& valTok = expect(Tok::Ident, "a metaclass name");
        if (kw == "metaclass") {
          decl->metaclass = valTok.lexeme;
        }
      } else if (check(Tok::KwMetaclass) && peek(1).kind == Tok::Eq) {
        advance(); // consume metaclass
        advance(); // consume '='
        const Token& valTok = expect(Tok::Ident, "a metaclass name");
        decl->metaclass = valTok.lexeme;
      } else {
        const Token& baseTok = expect(Tok::Ident, "a base class name");
        decl->baseClass = baseTok.lexeme;
      }
      if (!match(Tok::Comma)) break;
    }
    expect(Tok::RParen, "')' after inheritance list");
  }

  expect(Tok::Colon, "':' after the name");
  expectNewline();
  expect(Tok::Indent, "an indented body");

  std::string pendingDoc;
  bool pendingDocSet = false;
  bool typeDocSet = false;
  while (!check(Tok::Dedent) && !check(Tok::Eof)) {
    if (match(Tok::Newline)) continue;
    if (check(Tok::KwPass)) {
      advance();
      expectNewline();
      continue;
    }
    if (check(Tok::String)) {
      // Python-style docstring inside struct / class body
      std::string doc;
      parseDocstringStatement(doc);
      if (!typeDocSet && decl->fields.empty() && decl->methods.empty()) {
        decl->doc = std::move(doc);
        typeDocSet = true;
      } else {
        pendingDoc = std::move(doc);
        pendingDocSet = true;
      }
      continue;
    }
    if (check(Tok::KwFn)) {
      auto method = parseFnDecl();
      if (pendingDocSet) {
        static_cast<FnDecl*>(method.get())->doc = std::move(pendingDoc);
        pendingDoc.clear();
        pendingDocSet = false;
      }
      decl->methods.push_back(std::unique_ptr<FnDecl>(static_cast<FnDecl*>(method.release())));
      continue;
    }
    const Token& fieldTok = expect(Tok::Ident, "a field or method declaration");
    FieldDecl field;
    field.name = fieldTok.lexeme;
    field.span = fieldTok.span;
    if (match(Tok::Colon)) field.type = parseTypeExpr();
    if (pendingDocSet) {
      field.doc = std::move(pendingDoc);
      pendingDoc.clear();
      pendingDocSet = false;
    }
    expectNewline();
    decl->fields.push_back(std::move(field));
  }
  expect(Tok::Dedent, "the end of the struct body");

  decl->span = Span::join(start, previousSpan());
  return decl;
}

StmtPtr Parser::parseTypeAliasDecl() {
  Span start = cur().span;
  expect(Tok::KwType, "'type'");
  const Token& nameTok = expect(Tok::Ident, "a type name");
  auto decl = std::make_unique<TypeAliasDecl>();
  decl->name = nameTok.lexeme;
  decl->nameSpan = nameTok.span;
  expect(Tok::Eq, "'=' after the type alias name");
  decl->type = parseTypeExpr();
  expectNewline();
  decl->span = Span::join(start, previousSpan());
  return decl;
}



StmtPtr Parser::parseTraitDecl() {
  Span start = cur().span;
  expect(Tok::KwTrait, "'trait'");
  const Token& nameTok = expect(Tok::Ident, "a trait name");
  auto decl = std::make_unique<TraitDecl>();
  decl->name = nameTok.lexeme;
  decl->nameSpan = nameTok.span;

  if (check(Tok::Lt)) {
    parseTypeParams(decl->typeParams, decl->whereClause, decl->whereSpans);
  }

  // Trait inheritance: trait Equatable(Comparable):
  if (match(Tok::LParen)) {
    while (!check(Tok::RParen) && !check(Tok::Eof)) {
      const Token& parentTok = expect(Tok::Ident, "a parent trait name");
      decl->parents.push_back(parentTok.lexeme);
      if (!match(Tok::Comma)) break;
    }
    expect(Tok::RParen, "')' after parent traits");
  }

  if (match(Tok::KwWhere)) {
    while (true) {
      const Token& wTok = expect(Tok::Ident, "a trait name after 'where'");
      decl->whereClause.push_back(wTok.lexeme);
      decl->whereSpans.push_back(wTok.span);
      if (!match(Tok::Comma)) break;
    }
  }

  expect(Tok::Colon, "':' after the trait name");
  expectNewline();
  expect(Tok::Indent, "an indented body");
  std::string pendingDoc;
  bool pendingDocSet = false;
  bool typeDocSet = false;
  while (!check(Tok::Dedent) && !check(Tok::Eof)) {
    if (match(Tok::Newline)) continue;
    if (check(Tok::KwPass)) {
      advance();
      expectNewline();
      continue;
    }
    if (check(Tok::String)) {
      std::string doc;
      parseDocstringStatement(doc);
      if (!typeDocSet && decl->methods.empty()) {
        decl->doc = std::move(doc);
        typeDocSet = true;
      } else {
        pendingDoc = std::move(doc);
        pendingDocSet = true;
      }
      continue;
    }
    expect(Tok::KwFn, "a trait method signature");
    const Token& methodTok = expect(Tok::Ident, "a method name");
    TraitSig sig;
    sig.name = methodTok.lexeme;
    sig.span = methodTok.span;
    if (pendingDocSet) {
      sig.doc = std::move(pendingDoc);
      pendingDoc.clear();
      pendingDocSet = false;
    }
    expect(Tok::LParen, "'(' after the method name");
    if (!check(Tok::RParen)) {
      while (true) {
        sig.params.push_back(parseParam());
        if (!match(Tok::Comma)) break;
        if (check(Tok::RParen)) break;
      }
    }
    expect(Tok::RParen, "')' after the parameter list");
    if (match(Tok::Arrow)) sig.returnType = parseTypeExpr();
    expectNewline();
    decl->methods.push_back(std::move(sig));
  }
  expect(Tok::Dedent, "the end of the trait body");

  decl->span = Span::join(start, previousSpan());
  return decl;
}

// impl Trait for Type:
//     pass
StmtPtr Parser::parseImplDecl() {
  Span start = cur().span;
  expect(Tok::KwImpl, "'impl'");
  const Token& traitTok = expect(Tok::Ident, "a trait name after 'impl'");
  auto decl = std::make_unique<ImplDecl>();
  decl->traitName = traitTok.lexeme;
  decl->traitSpan = traitTok.span;

  expect(Tok::KwFor, "'for' after the trait name");
  const Token& targetTok = expect(Tok::Ident, "a type name after 'for'");
  decl->targetName = targetTok.lexeme;
  decl->targetSpan = targetTok.span;

  // Optional body: `pass`, or method definitions.
  if (match(Tok::Colon)) {
    expectNewline();
    expect(Tok::Indent, "an indented body");
    std::string pendingDoc;
    bool pendingDocSet = false;
    while (!check(Tok::Dedent) && !check(Tok::Eof)) {
      if (match(Tok::Newline)) continue;
      if (check(Tok::KwPass)) {
        advance();
        expectNewline();
        continue;
      }
      if (check(Tok::String)) {
        std::string doc;
        parseDocstringStatement(doc);
        pendingDoc = std::move(doc);
        pendingDocSet = true;
        continue;
      }
      if (check(Tok::KwFn)) {
        auto method = parseFnDecl();
        if (pendingDocSet) {
          static_cast<FnDecl*>(method.get())->doc = std::move(pendingDoc);
          pendingDoc.clear();
          pendingDocSet = false;
        }
        decl->methods.push_back(std::unique_ptr<FnDecl>(static_cast<FnDecl*>(method.release())));
        continue;
      }
      fail(cur().span, "Expected a method definition or 'pass' in the impl body.");
    }
    expect(Tok::Dedent, "the end of the impl body");
  } else {
    expectNewline();
  }

  decl->span = Span::join(start, previousSpan());
  return decl;
}

    StmtPtr Parser::parseEnumDecl() {
  Span start = cur().span;
  expect(Tok::KwEnum, "'enum'");
  const Token& nameTok = expect(Tok::Ident, "an enum name");

  auto decl = std::make_unique<EnumDecl>();
  decl->name = nameTok.lexeme;
  decl->nameSpan = nameTok.span;

  if (match(Tok::Lt)) {
    while (!check(Tok::Gt) && !check(Tok::Eof)) {
      const Token& tp = expect(Tok::Ident, "a type parameter name");
      decl->typeParams.push_back(tp.lexeme);
      if (!match(Tok::Comma)) break;
    }
    expect(Tok::Gt, "'>' after type parameters");
  }

  expect(Tok::Colon, "':' after the enum name");
  expectNewline();
  expect(Tok::Indent, "an indented enum body");
  std::string pendingDoc;
  bool pendingDocSet = false;
  bool typeDocSet = false;
  while (!check(Tok::Dedent) && !check(Tok::Eof)) {
    if (match(Tok::Newline)) continue;
    if (check(Tok::String)) {
      std::string doc;
      parseDocstringStatement(doc);
      if (!typeDocSet && decl->variants.empty()) {
        decl->doc = std::move(doc);
        typeDocSet = true;
      } else {
        pendingDoc = std::move(doc);
        pendingDocSet = true;
      }
      continue;
    }
    // 'None' is a keyword, but the prelude's Option declares a variant with
    // that name; allow it in this one position.
    Token variantTok;
    if (check(Tok::KwNone)) {
      variantTok = advance();
    } else {
      variantTok = expect(Tok::Ident, "an enum variant name");
    }
    EnumVariant variant;
    variant.name = variantTok.lexeme;
    variant.span = variantTok.span;
    if (pendingDocSet) {
      variant.doc = std::move(pendingDoc);
      pendingDoc.clear();
      pendingDocSet = false;
    }
    if (match(Tok::LParen)) {
      if (!check(Tok::RParen)) {
        while (true) {
          const Token& fieldTok = expect(Tok::Ident, "a payload field name");
          FieldDecl field;
          field.name = fieldTok.lexeme;
          field.span = fieldTok.span;
          if (match(Tok::Colon)) field.type = parseTypeExpr();
          variant.fields.push_back(std::move(field));
          if (!match(Tok::Comma)) break;
          if (check(Tok::RParen)) break;
        }
      }
      expect(Tok::RParen, "')' after the payload fields");
    } else if (match(Tok::Eq)) {
      variant.value = parseExpression();
    }
    expectNewline();
    decl->variants.push_back(std::move(variant));
  }
  expect(Tok::Dedent, "the end of the enum body");
  if (decl->variants.empty()) {
    fail(start, "An enum needs at least one variant.");
  }

  decl->span = Span::join(start, previousSpan());
  return decl;
}

StmtPtr Parser::parseTypedDeclaration() {
  Span start = cur().span;
  const Token& nameTok = advance();
  expect(Tok::Colon, "':'");
  auto type = parseTypeExpr();
  expect(Tok::Eq, "'=' (variables must be initialized)");
  auto init = parseExpression();
  expectNewline();

  auto decl = std::make_unique<LetStmt>();
  decl->span = Span::join(start, previousSpan());
  decl->name = nameTok.lexeme;
  decl->nameSpan = nameTok.span;
  decl->type = std::move(type);
  decl->init = std::move(init);
  decl->mutableBinding = true;
  return decl;
}

StmtPtr Parser::parseDestructureDecl(bool mutableBinding, Span start) {
  expect(Tok::LParen, "'('");
  auto stmt = std::make_unique<DestructureStmt>();
  while (true) {
    const Token& nameTok = expect(Tok::Ident, "a binding name or '_'");
    stmt->names.push_back(nameTok.lexeme);
    stmt->nameSpans.push_back(nameTok.span);
    if (!match(Tok::Comma)) break;
    if (check(Tok::RParen)) break;
  }
  expect(Tok::RParen, "')' after the destructuring targets");
  expect(Tok::Eq, "'=' (variables must be initialized)");
  stmt->value = parseExpression();
  expectNewline();
  stmt->mutableBinding = mutableBinding;
  stmt->span = Span::join(start, previousSpan());
  return stmt;
}

StmtPtr Parser::buildDestructure(ExprPtr target, ExprPtr value, bool mutableBinding,
                                 Span span) {
  auto* tuple = dynamic_cast<TupleExpr*>(target.get());
  if (!tuple) {
    fail(spanOf(target), "Only a tuple target may be destructured.");
  }
  auto stmt = std::make_unique<DestructureStmt>();
  stmt->span = span;
  stmt->mutableBinding = mutableBinding;
  for (auto& element : tuple->elements) {
    auto* ident = dynamic_cast<IdentExpr*>(element.get());
    if (!ident) {
      fail(spanOf(element),
           "Destructuring targets must be names or '_'.",
           "Write (a, b) = ... with plain names.");
    }
    stmt->names.push_back(ident->name);
    stmt->nameSpans.push_back(element->span);
  }
  stmt->value = std::move(value);
  return stmt;
}
}  // namespace a
