#include "parser/parser_internal.hpp"

namespace a {
TypeExprPtr Parser::parseTypeExpr() {
  TypeExprPtr type;
  if (check(Tok::LBracket)) {
    Span start = cur().span;
    advance();
    auto elem = parseTypeExpr();
    expect(Tok::RBracket, "']' after the element type");
    auto array = std::make_unique<ArrayTypeExpr>();
    array->span = Span::join(start, previousSpan());
    array->element = std::move(elem);
    type = std::move(array);
  } else if (check(Tok::LParen)) {
    Span start = cur().span;
    advance();
    auto first = parseTypeExpr();
    if (match(Tok::Comma)) {
      auto tuple = std::make_unique<TupleTypeExpr>();
      tuple->elements.push_back(std::move(first));
      while (!check(Tok::RParen)) {
        tuple->elements.push_back(parseTypeExpr());
        if (!match(Tok::Comma)) break;
      }
      expect(Tok::RParen, "')' after the tuple element types");
      tuple->span = Span::join(start, previousSpan());
      type = std::move(tuple);
    } else {
      expect(Tok::RParen, "')' after the type");
      first->span = Span::join(start, previousSpan());
      type = std::move(first);
    }
  } else if (check(Tok::LBrace)) {
    Span start = cur().span;
    advance();
    auto first = parseTypeExpr();
    if (match(Tok::Colon)) {
      auto map = std::make_unique<MapTypeExpr>();
      map->key = std::move(first);
      map->value = parseTypeExpr();
      expect(Tok::RBrace, "'}' after the map value type");
      map->span = Span::join(start, previousSpan());
      type = std::move(map);
    } else {
      auto set = std::make_unique<SetTypeExpr>();
      set->element = std::move(first);
      expect(Tok::RBrace, "'}' after the set element type");
      set->span = Span::join(start, previousSpan());
      type = std::move(set);
    }
  } else if (check(Tok::KwFn)) {
    Span start = cur().span;
    advance();
    expect(Tok::LParen, "'(' after 'fn' in a function type");
    auto fn = std::make_unique<FunctionTypeExpr>();
    if (!check(Tok::RParen)) {
      while (true) {
        fn->params.push_back(parseTypeExpr());
        if (!match(Tok::Comma)) break;
        if (check(Tok::RParen)) break;
      }
    }
    expect(Tok::RParen, "')' after the function type parameters");
    expect(Tok::Arrow, "'->' before the function return type");
    fn->returnType = parseTypeExpr();
    fn->span = Span::join(start, previousSpan());
    type = std::move(fn);
  } else {
    const Token& nameTok = expect(Tok::Ident, "a type name");
    auto named = std::make_unique<NamedTypeExpr>();
    named->span = nameTok.span;
    std::string name = nameTok.lexeme;
    while (match(Tok::Dot)) {
      named->qualifiers.push_back(name);
      const Token& part = expect(Tok::Ident, "a type name after '.'");
      name = part.lexeme;
    }
    named->name = std::move(name);
    if (match(Tok::Lt)) {
      // '>>' lexes as one token but closes two generic levels; the loop
      // stops on it and matchTypeArgGt splits it into two '>' tokens.
      while (!check(Tok::Gt) && !check(Tok::Shr) && !check(Tok::Eof)) {
        // Integer literals are allowed as type arguments for comptime
        // parameters such as the lane count in `vector[float, 4]`.
        if (check(Tok::Int)) {
          const Token& lit = advance();
          auto konst = std::make_unique<ConstTypeExpr>();
          konst->span = lit.span;
          konst->value = lit.intValue;
          named->typeArgs.push_back(std::move(konst));
        } else {
          named->typeArgs.push_back(parseTypeExpr());
        }
        if (!match(Tok::Comma)) break;
      }
      if (!matchTypeArgGt()) {
        fail(cur().span, "Expected '>' after type arguments, found " +
                             describeToken(cur()) + ".");
      }
    }
    named->span = Span::join(named->span, previousSpan());
    type = std::move(named);
  }

  // Fixed-size suffixes are C-like: `i32[2][3]` is two rows of three. Collect
  // the dimensions in source order and wrap the element type from the inside
  // out, so the first suffix becomes the outermost array.
  std::vector<std::pair<std::int64_t, Span>> dimensions;
  while (check(Tok::LBracket)) {
    Span start = cur().span;
    advance();
    const Token& size = expect(Tok::Int, "a fixed array length");
    if (size.intValue <= 0) {
      fail(size.span, "A fixed array length must be a positive integer.");
    }
    expect(Tok::RBracket, "']' after the array length");
    dimensions.push_back({size.intValue, Span::join(start, previousSpan())});
  }
  for (auto it = dimensions.rbegin(); it != dimensions.rend(); ++it) {
    auto fixed = std::make_unique<ArrayTypeExpr>();
    fixed->span = Span::join(it->second, type->span);
    fixed->element = std::move(type);
    fixed->length = it->first;
    type = std::move(fixed);
  }

  while (match(Tok::Question)) {
    auto optional = std::make_unique<OptionalTypeExpr>();
    optional->span = Span::join(type->span, previousSpan());
    optional->element = std::move(type);
    type = std::move(optional);
  }
  return type;
}
}  // namespace a
