#include "parser/parser_internal.hpp"

namespace a {
Pattern Parser::parsePattern() {
  Pattern primary = parsePrimaryPattern();
  if (!check(Tok::Bar)) return primary;
  Pattern orPattern;
  orPattern.kind = Pattern::Kind::Or;
  orPattern.span = primary.span;
  orPattern.subpatterns.push_back(std::move(primary));
  while (match(Tok::Bar)) {
    Pattern alt = parsePrimaryPattern();
    orPattern.span = Span::join(orPattern.span, alt.span);
    orPattern.subpatterns.push_back(std::move(alt));
  }
  return orPattern;
}

Pattern Parser::parsePrimaryPattern() {
  Pattern pattern;
  pattern.span = cur().span;
  if (check(Tok::Ident) && cur().lexeme == "_") {
    advance();
    pattern.kind = Pattern::Kind::Wildcard;
    return pattern;
  }

  if (check(Tok::LParen)) {
    Span start = cur().span;
    advance();
    while (!check(Tok::RParen) && !check(Tok::Eof)) {
      pattern.subpatterns.push_back(parsePattern());
      if (!match(Tok::Comma)) break;
    }
    Span end = cur().span;
    expect(Tok::RParen, "')' after the tuple pattern");
    if (pattern.subpatterns.size() == 1) return std::move(pattern.subpatterns.front());
    pattern.kind = Pattern::Kind::Tuple;
    pattern.span = Span::join(start, end);
    if (pattern.subpatterns.empty()) {
      fail(start, "Empty tuple patterns are not allowed; use '_'.");
    }
    return pattern;
  }

  if (check(Tok::Ident)) {
    std::vector<std::string> parts;
    std::vector<Span> spans;
    while (true) {
      parts.push_back(cur().lexeme);
      spans.push_back(cur().span);
      advance();
      if (check(Tok::Dot) && peek(1).kind == Tok::Ident) {
        advance();
        continue;
      }
      break;
    }
    if (parts.size() > 1 && check(Tok::LParen)) {
      pattern.kind = Pattern::Kind::Variant;
      pattern.ctor = makeFieldChain(parts, spans);
      advance();
      while (!check(Tok::RParen) && !check(Tok::Eof)) {
        pattern.subpatterns.push_back(parsePattern());
        if (!match(Tok::Comma)) break;
      }
      Span end = cur().span;
      expect(Tok::RParen, "')' after the variant pattern payload");
      pattern.span = Span::join(spans.front(), end);
      return pattern;
    }
    if (parts.size() == 1 && check(Tok::LParen)) {
      fail(cur().span, "Variant patterns must name their enum, e.g. Shape." + parts.front() +
                           "(...).");
    }
    if (check(Tok::DotDot)) {
      pattern.kind = Pattern::Kind::Range;
      pattern.rangeStart = makeFieldChain(parts, spans);
      advance();
      pattern.rangeEnd = parseExpression(10);
      if (check(Tok::DotDot)) {
        fail(cur().span, "Range patterns do not take a step.");
      }
      pattern.span = Span::join(spans.front(), previousSpan());
      return pattern;
    }
    if (parts.size() == 1) {
      pattern.kind = Pattern::Kind::Binding;
      pattern.name = parts.front();
      pattern.span = spans.front();
      return pattern;
    }
    pattern.kind = Pattern::Kind::Literal;  // plain enum tag, e.g. Color.Red
    pattern.value = makeFieldChain(parts, spans);
    pattern.span = spans.back();
    return pattern;
  }

  auto expr = parseExpression(10);
  if (check(Tok::DotDot)) {
    pattern.kind = Pattern::Kind::Range;
    pattern.rangeStart = std::move(expr);
    advance();
    pattern.rangeEnd = parseExpression(10);
    if (check(Tok::DotDot)) {
      fail(cur().span, "Range patterns do not take a step.");
    }
    pattern.span = Span::join(pattern.rangeStart->span, previousSpan());
    return pattern;
  }
  bool literal = dynamic_cast<IntLitExpr*>(expr.get()) ||
                 dynamic_cast<FloatLitExpr*>(expr.get()) ||
                 dynamic_cast<StringLitExpr*>(expr.get()) ||
                 dynamic_cast<CharLitExpr*>(expr.get()) ||
                 dynamic_cast<BoolLitExpr*>(expr.get());
  if (auto* unary = dynamic_cast<UnaryExpr*>(expr.get());
      unary && unary->op == "-") {
    literal = dynamic_cast<IntLitExpr*>(unary->operand.get()) ||
              dynamic_cast<FloatLitExpr*>(unary->operand.get());
  }
  if (!literal) {
    fail(expr->span, "Match patterns must be literals, ranges, variant patterns, or '_'.",
         "Ranges are inclusive, e.g. 400..499; variants destructure, e.g. Shape.Circle(r).");
  }
  pattern.kind = Pattern::Kind::Literal;
  pattern.span = expr->span;
  pattern.value = std::move(expr);
  return pattern;
}

ExprPtr Parser::parseMatch() {
  Span start = cur().span;
  expect(Tok::KwMatch, "'match'");
  auto matchExpr = std::make_unique<MatchExpr>();
  matchExpr->subject = parseExpression();
  expect(Tok::Colon, "':' after the match subject");
  expectNewline();
  expect(Tok::Indent, "an indented block of match arms");
  while (!check(Tok::Dedent) && !check(Tok::Eof)) {
    if (match(Tok::Newline)) continue;
    MatchArm arm;
    arm.pattern = parsePattern();
    if (match(Tok::KwIf)) {
      arm.guard = parseExpression();
    }
    expect(Tok::FatArrow, "'=>' after the match pattern");
    arm.result = parseExpression();
    expectNewline();
    matchExpr->arms.push_back(std::move(arm));
  }
  expect(Tok::Dedent, "the end of the match block");
  if (matchExpr->arms.empty()) {
    fail(start, "A match expression needs at least one arm.");
  }
  matchExpr->span = Span::join(start, previousSpan());
  return matchExpr;
}

}  // namespace a
