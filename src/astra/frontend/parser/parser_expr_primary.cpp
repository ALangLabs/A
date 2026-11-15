#include "parser/parser_internal.hpp"

namespace a {
ExprPtr Parser::parsePrimary() {
  const Token& token = cur();
  switch (token.kind) {
    case Tok::Int: {
      advance();
      auto expr = std::make_unique<IntLitExpr>();
      expr->span = token.span;
      expr->value = token.intValue;
      return expr;
    }
    case Tok::Float: {
      advance();
      auto expr = std::make_unique<FloatLitExpr>();
      expr->span = token.span;
      expr->value = token.floatValue;
      return expr;
    }
    case Tok::String: {
      advance();
      auto expr = std::make_unique<StringLitExpr>();
      expr->span = token.span;
      expr->parts = parseStringParts(token);
      return expr;
    }
    case Tok::Char: {
      advance();
      auto expr = std::make_unique<CharLitExpr>();
      expr->span = token.span;
      expr->value = token.charValue;
      return expr;
    }
    case Tok::KwTrue:
    case Tok::KwFalse: {
      advance();
      auto expr = std::make_unique<BoolLitExpr>();
      expr->span = token.span;
      expr->value = token.kind == Tok::KwTrue;
      return expr;
    }
    case Tok::Ident: {
      advance();
      auto expr = std::make_unique<IdentExpr>();
      expr->span = token.span;
      expr->name = token.lexeme;
      return expr;
    }
    case Tok::KwSelf: {
      advance();
      auto expr = std::make_unique<IdentExpr>();
      expr->span = token.span;
      expr->name = "self";
      return expr;
    }
    case Tok::KwNone: {
      advance();
      auto expr = std::make_unique<IdentExpr>();
      expr->span = token.span;
      expr->name = "None";
      return expr;
    }
    case Tok::KwType: {
      // 'type' is a keyword for alias declarations, but 'type(x)' is the
      // builtin call. Only the call form is a valid expression.
      if (peek(1).kind != Tok::LParen) {
        fail(token.span, "Expected an expression, found " + describeToken(token) + ".",
             "Use 'type(x)' to get a value's type name, or 'type Name = ...' to declare "
             "a type alias.");
      }
      advance();
      auto expr = std::make_unique<IdentExpr>();
      expr->span = token.span;
      expr->name = "type";
      return expr;
    }
    case Tok::KwFn:
      return parseLambda();
    case Tok::KwMatch:
      return parseMatch();
    case Tok::LParen: {
      Span start = token.span;
      advance();
      if (check(Tok::RParen)) {
        fail(cur().span, "Empty tuples are not supported.");
      }
      auto first = parseExpression();
      if (!match(Tok::Comma)) {
        expect(Tok::RParen, "')' after the expression");
        first->span = Span::join(start, previousSpan());
        return first;
      }
      auto tuple = std::make_unique<TupleExpr>();
      tuple->elements.push_back(std::move(first));
      while (!check(Tok::RParen)) {
        tuple->elements.push_back(parseExpression());
        if (!match(Tok::Comma)) break;
      }
      expect(Tok::RParen, "')' after the tuple elements");
      tuple->span = Span::join(start, previousSpan());
      return tuple;
    }
    case Tok::LBracket: {
      Span start = token.span;
      advance();
      if (check(Tok::RBracket)) {
        advance();
        auto array = std::make_unique<ArrayLitExpr>();
        array->span = Span::join(start, previousSpan());
        return array;
      }
      auto first = parseExpression();
      if (check(Tok::KwFor)) {
        advance();
        const Token& varTok = expect(Tok::Ident, "a loop variable name");
        expect(Tok::KwIn, "'in' after the loop variable");
        auto comprehension = std::make_unique<ComprehensionExpr>();
        comprehension->var = varTok.lexeme;
        comprehension->varSpan = varTok.span;
        comprehension->element = std::move(first);
        comprehension->iterable = parseExpression(3);
        if (match(Tok::KwIf)) {
          comprehension->condition = parseExpression();
        }
        expect(Tok::RBracket, "']' after the comprehension");
        comprehension->span = Span::join(start, previousSpan());
        return comprehension;
      }
      auto array = std::make_unique<ArrayLitExpr>();
      array->elements.push_back(std::move(first));
      while (match(Tok::Comma)) {
        if (check(Tok::RBracket)) break;
        array->elements.push_back(parseExpression());
      }
      expect(Tok::RBracket, "']' after the array elements");
      array->span = Span::join(start, previousSpan());
      return array;
    }
    case Tok::LBrace: {
      Span start = token.span;
      advance();
      if (check(Tok::RBrace)) {
        advance();
        // `{}` is an empty map, as in Python; an empty set needs set().
        auto map = std::make_unique<MapLitExpr>();
        map->span = Span::join(start, previousSpan());
        return map;
      }
      auto firstKey = parseExpression();
      if (check(Tok::KwFor)) {
        advance();
        const Token& varTok = expect(Tok::Ident, "a loop variable name");
        expect(Tok::KwIn, "'in' after the loop variable");
        auto comprehension = std::make_unique<ComprehensionExpr>();
        comprehension->isSet = true;
        comprehension->var = varTok.lexeme;
        comprehension->varSpan = varTok.span;
        comprehension->element = std::move(firstKey);
        comprehension->iterable = parseExpression(3);
        if (match(Tok::KwIf)) {
          comprehension->condition = parseExpression();
        }
        expect(Tok::RBrace, "'}' after the set comprehension");
        comprehension->span = Span::join(start, previousSpan());
        return comprehension;
      }
      if (match(Tok::Colon)) {
        // Map literal: {key: value, ...}  — or dict comprehension
        // {k: v for x in xs [if c]} which desugars to
        // dict(map(filter(xs, fn(x) => c), fn(x) => (k, v))).
        auto firstVal = parseExpression();
        if (check(Tok::KwFor)) {
          advance();
          const Token& varTok = expect(Tok::Ident, "a loop variable name");
          expect(Tok::KwIn, "'in' after the loop variable");
          ExprPtr iterable = parseExpression(3);
          ExprPtr condition;
          if (match(Tok::KwIf)) condition = parseExpression();
          expect(Tok::RBrace, "'}' after the dict comprehension");
          Span span = Span::join(start, previousSpan());
          auto makeLambda = [&](ExprPtr body) {
            auto lam = std::make_unique<LambdaExpr>();
            lam->span = span;
            ParamDecl p;
            p.name = varTok.lexeme;
            p.span = varTok.span;
            lam->params.push_back(std::move(p));
            lam->body = std::move(body);
            return lam;
          };
          auto callBuiltin = [&](const char* name, std::vector<ExprPtr> fargs) {
            auto call = std::make_unique<CallExpr>();
            call->span = span;
            auto callee = std::make_unique<IdentExpr>();
            callee->name = name;
            callee->span = span;
            call->callee = std::move(callee);
            for (auto& e : fargs) {
              CallArg a;
              a.value = std::move(e);
              call->args.push_back(std::move(a));
            }
            return call;
          };
          ExprPtr mapped = std::move(iterable);
          if (condition) {
            std::vector<ExprPtr> fargs;
            fargs.push_back(std::move(mapped));
            fargs.push_back(makeLambda(std::move(condition)));
            mapped = callBuiltin("filter", std::move(fargs));
          }
          auto pair = std::make_unique<TupleExpr>();
          pair->span = span;
          pair->elements.push_back(std::move(firstKey));
          pair->elements.push_back(std::move(firstVal));
          std::vector<ExprPtr> margs;
          margs.push_back(std::move(mapped));
          margs.push_back(makeLambda(std::move(pair)));
          mapped = callBuiltin("map", std::move(margs));
          std::vector<ExprPtr> dargs;
          dargs.push_back(std::move(mapped));
          return callBuiltin("dict", std::move(dargs));
        }
        auto map = std::make_unique<MapLitExpr>();
        map->entries.push_back({std::move(firstKey), std::move(firstVal)});
        while (match(Tok::Comma)) {
          if (check(Tok::RBrace)) break;
          auto key = parseExpression();
          expect(Tok::Colon, "':' after map key");
          auto value = parseExpression();
          map->entries.push_back({std::move(key), std::move(value)});
        }
        expect(Tok::RBrace, "'}' after map entries");
        map->span = Span::join(start, previousSpan());
        return map;
      }
      // Set literal: {1, 2, 3}
      auto set = std::make_unique<SetLitExpr>();
      set->elements.push_back(std::move(firstKey));
      while (match(Tok::Comma)) {
        if (check(Tok::RBrace)) break;
        set->elements.push_back(parseExpression());
      }
      expect(Tok::RBrace, "'}' after set elements");
      set->span = Span::join(start, previousSpan());
      return set;
    }
    default:
      fail(token.span, "Expected an expression, found " + describeToken(token) + ".");
  }
}

ExprPtr Parser::parseLambda() {
  Span start = cur().span;
  expect(Tok::KwFn, "'fn'");
  expect(Tok::LParen, "'(' after 'fn' in a lambda");
  auto lambda = std::make_unique<LambdaExpr>();
  if (!check(Tok::RParen)) {
    while (true) {
      lambda->params.push_back(parseParam());
      if (!match(Tok::Comma)) break;
      if (check(Tok::RParen)) break;
    }
  }
  expect(Tok::RParen, "')' after the lambda parameters");
  if (match(Tok::Arrow)) {
    lambda->returnType = parseTypeExpr();
  }
  expect(Tok::FatArrow, "'=>' before the lambda body");
  lambda->body = parseExpression();
  lambda->span = Span::join(start, lambda->body->span);
  return lambda;
}

}  // namespace a
