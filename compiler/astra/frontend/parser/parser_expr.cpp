#include "parser/parser_internal.hpp"

namespace a {
// ------------------------------------------------------------- expressions

int Parser::binaryPrecedence(Tok kind) {
  switch (kind) {
    case Tok::Pipe: return 1;
    case Tok::Question: return 2;
    case Tok::QuestionQuestion: return 3;
    case Tok::KwOr: return 4;
    case Tok::KwAnd: return 5;
    case Tok::KwIn:
    case Tok::EqEq:
    case Tok::NotEq:
    case Tok::Lt:
    case Tok::Gt:
    case Tok::Le:
    case Tok::Ge:
    case Tok::KwIs: return 7;
    case Tok::DotDot: return 8;
    case Tok::Bar: return 9;
    case Tok::Caret: return 10;
    case Tok::Amp: return 11;
    case Tok::Shl:
    case Tok::Shr: return 12;
    case Tok::Plus:
    case Tok::Minus: return 13;
    case Tok::Star:
    case Tok::Slash:
    case Tok::Percent: return 14;
    case Tok::StarStar: return 16;
    default: return -1;
  }
}

bool Parser::isRightAssociative(Tok kind) {
  return kind == Tok::StarStar || kind == Tok::Question;
}

const char* Parser::opName(Tok kind) {
  switch (kind) {
    case Tok::KwOr: return "or";
    case Tok::KwAnd: return "and";
    case Tok::KwIn: return "in";
    case Tok::KwIs: return "==";
    case Tok::EqEq: return "==";
    case Tok::NotEq: return "!=";
    case Tok::Lt: return "<";
    case Tok::Gt: return ">";
    case Tok::Le: return "<=";
    case Tok::Ge: return ">=";
    case Tok::DotDot: return "..";
    case Tok::QuestionQuestion: return "??";
    case Tok::Pipe: return "|>";
    case Tok::Plus: return "+";
    case Tok::Minus: return "-";
    case Tok::Star: return "*";
    case Tok::Slash: return "/";
    case Tok::Percent: return "%";
    case Tok::StarStar: return "**";
    case Tok::Amp: return "&";
    case Tok::Bar: return "|";
    case Tok::Caret: return "^";
    case Tok::Tilde: return "~";
    case Tok::Shl: return "<<";
    case Tok::Shr: return ">>";
    case Tok::Eq: return "=";
    case Tok::PlusEq: return "+=";
    case Tok::MinusEq: return "-=";
    case Tok::StarEq: return "*=";
    case Tok::SlashEq: return "/=";
    case Tok::PercentEq: return "%=";
    case Tok::AmpEq: return "&=";
    case Tok::BarEq: return "|=";
    case Tok::CaretEq: return "^=";
    case Tok::ShlEq: return "<<=";
    case Tok::ShrEq: return ">>=";
    default: return "?";
  }
}

BinOp Parser::binOp(Tok kind) {
  switch (kind) {
    case Tok::Plus: return BinOp::Add;
    case Tok::PlusEq: return BinOp::Add;
    case Tok::Minus: return BinOp::Sub;
    case Tok::MinusEq: return BinOp::Sub;
    case Tok::Star: return BinOp::Mul;
    case Tok::StarEq: return BinOp::Mul;
    case Tok::Slash: return BinOp::Div;
    case Tok::SlashEq: return BinOp::Div;
    case Tok::Percent: return BinOp::Mod;
    case Tok::PercentEq: return BinOp::Mod;
    case Tok::StarStar: return BinOp::Pow;
    case Tok::EqEq: return BinOp::Eq;
    case Tok::NotEq: return BinOp::Ne;
    case Tok::Lt: return BinOp::Lt;
    case Tok::Gt: return BinOp::Gt;
    case Tok::Le: return BinOp::Le;
    case Tok::Ge: return BinOp::Ge;
    case Tok::Amp: return BinOp::BitAnd;
    case Tok::AmpEq: return BinOp::BitAnd;
    case Tok::Bar: return BinOp::BitOr;
    case Tok::BarEq: return BinOp::BitOr;
    case Tok::Caret: return BinOp::BitXor;
    case Tok::CaretEq: return BinOp::BitXor;
    case Tok::Shl: return BinOp::Shl;
    case Tok::ShlEq: return BinOp::Shl;
    case Tok::Shr: return BinOp::Shr;
    case Tok::ShrEq: return BinOp::Shr;
    case Tok::KwAnd: return BinOp::And;
    case Tok::KwOr: return BinOp::Or;
    case Tok::KwIn: return BinOp::In;
    case Tok::KwIs: return BinOp::Eq;
    case Tok::QuestionQuestion: return BinOp::Coalesce;
    case Tok::DotDot: return BinOp::Range;
    default: return BinOp::Invalid;
  }
}

bool Parser::isAssignmentOp(Tok kind) {
  switch (kind) {
    case Tok::Eq:
    case Tok::PlusEq:
    case Tok::MinusEq:
    case Tok::StarEq:
    case Tok::SlashEq:
    case Tok::PercentEq:
    case Tok::AmpEq:
    case Tok::BarEq:
    case Tok::CaretEq:
    case Tok::ShlEq:
    case Tok::ShrEq:
      return true;
    default:
      return false;
  }
}

ExprPtr Parser::parseExpression(int minPrec) {
  ExprPtr left = parsePrefix();
  while (true) {
    if (cur().kind == Tok::KwNot && peek(1).kind == Tok::KwIn) {
      Span notSpan = advance().span; // 'not'
      Span inSpan = advance().span;  // 'in'
      int prec = 7;
      auto right = parseExpression(prec + 1);
      auto binary = std::make_unique<BinaryExpr>();
      binary->span = Span::join(left->span, right->span);
      binary->opSpan = Span::join(notSpan, inSpan);
      binary->op = "not in";
      binary->binop = BinOp::NotIn;
      binary->lhs = std::move(left);
      binary->rhs = std::move(right);
      left = std::move(binary);
      continue;
    }
    if (cur().kind == Tok::KwIs && peek(1).kind == Tok::KwNot) {
      Span isSpan = advance().span;   // 'is'
      Span notSpan = advance().span;  // 'not'
      auto right = parseExpression(8);
      auto binary = std::make_unique<BinaryExpr>();
      binary->span = Span::join(left->span, right->span);
      binary->opSpan = Span::join(isSpan, notSpan);
      binary->op = "!=";
      binary->binop = BinOp::Ne;
      binary->lhs = std::move(left);
      binary->rhs = std::move(right);
      left = std::move(binary);
      continue;
    }

    Tok kind = cur().kind;
    int prec = binaryPrecedence(kind);
    if (prec < minPrec || prec < 0) break;
    const Token& opTok = advance();
    if (kind == Tok::Question) {
      auto thenValue = parseExpression();
      expect(Tok::Colon, "':' in the conditional expression");
      auto elseValue = parseExpression(prec);
      auto ternary = std::make_unique<TernaryExpr>();
      ternary->span = Span::join(left->span, elseValue->span);
      ternary->cond = std::move(left);
      ternary->thenValue = std::move(thenValue);
      ternary->elseValue = std::move(elseValue);
      left = std::move(ternary);
      continue;
    }
    if (kind == Tok::DotDot) {
      auto end = parseExpression(prec + 1);
      if (auto* range = dynamic_cast<RangeExpr*>(left.get())) {
        if (range->step) {
          fail(opTok.span, "A range takes at most a start, end, and step.",
               "Write start..end or start..end..step.");
        }
        range->span = Span::join(range->span, end->span);
        range->step = std::move(end);
      } else {
        auto newRange = std::make_unique<RangeExpr>();
        newRange->span = Span::join(left->span, end->span);
        newRange->start = std::move(left);
        newRange->end = std::move(end);
        left = std::move(newRange);
      }
      continue;
    }
    int nextMin = isRightAssociative(kind) ? prec : prec + 1;
    auto right = parseExpression(nextMin);
    if (kind == Tok::Pipe) {
      Span leftSpan = left->span;
      auto* call = dynamic_cast<CallExpr*>(right.get());
      if (!call || dynamic_cast<IdentExpr*>(call->callee.get()) == nullptr) {
        fail(opTok.span, "The right side of '|>' must be a call to a plain function.",
             "Write value |> name(args); the value becomes the first argument.");
      }
      CallArg piped;
      piped.value = std::move(left);
      call->args.insert(call->args.begin(), std::move(piped));
      call->span = Span::join(leftSpan, call->span);
      left = std::move(right);
      continue;
    }
    auto binary = std::make_unique<BinaryExpr>();
    binary->span = Span::join(left->span, right->span);
    binary->opSpan = opTok.span;
    binary->op = opName(kind);
    binary->binop = binOp(kind);
    binary->lhs = std::move(left);
    binary->rhs = std::move(right);
    left = std::move(binary);
  }
  return left;
}

ExprPtr Parser::parsePrefix() {
  if (check(Tok::KwNot)) {
    const Token& opTok = advance();
    auto operand = parseExpression(6);
    auto expr = std::make_unique<UnaryExpr>();
    expr->span = Span::join(opTok.span, operand->span);
    expr->opSpan = opTok.span;
    expr->op = "not";
    expr->operand = std::move(operand);
    return expr;
  }
  if (check(Tok::PlusPlus) || check(Tok::MinusMinus)) {
    const Token& opTok = advance();
    auto operand = parseExpression(15);
    auto expr = std::make_unique<UnaryExpr>();
    expr->span = Span::join(opTok.span, operand->span);
    expr->opSpan = opTok.span;
    expr->op = opTok.kind == Tok::PlusPlus ? "++" : "--";
    expr->postfix = false;
    expr->operand = std::move(operand);
    return expr;
  }
  if (check(Tok::Minus) || check(Tok::Tilde)) {
    const Token& opTok = advance();
    auto operand = parseExpression(15);
    auto expr = std::make_unique<UnaryExpr>();
    expr->span = Span::join(opTok.span, operand->span);
    expr->opSpan = opTok.span;
    expr->op = opTok.kind == Tok::Minus ? "-" : "~";
    expr->operand = std::move(operand);
    return expr;
  }
  return parsePostfix();
}

ExprPtr Parser::parsePostfix() {
  auto expr = parsePrimary();
  while (true) {
    if (check(Tok::PlusPlus) || check(Tok::MinusMinus)) {
      const Token& opTok = advance();
      auto unary = std::make_unique<UnaryExpr>();
      unary->span = Span::join(expr->span, opTok.span);
      unary->opSpan = opTok.span;
      unary->op = opTok.kind == Tok::PlusPlus ? "++" : "--";
      unary->postfix = true;
      unary->operand = std::move(expr);
      expr = std::move(unary);
      continue;
    }
    if (check(Tok::Dot)) {
      advance();
      const Token& fieldTok = expect(Tok::Ident, "a field or method name");
      auto field = std::make_unique<FieldExpr>();
      field->span = Span::join(expr->span, fieldTok.span);
      field->fieldSpan = fieldTok.span;
      field->field = fieldTok.lexeme;
      field->object = std::move(expr);
      expr = std::move(field);
      continue;
    }
    if (check(Tok::QuestionDot)) {
      advance();
      const Token& fieldTok = expect(Tok::Ident, "a field name after '?.'");
      auto nav = std::make_unique<SafeNavExpr>();
      nav->span = Span::join(expr->span, fieldTok.span);
      nav->fieldSpan = fieldTok.span;
      nav->field = fieldTok.lexeme;
      nav->object = std::move(expr);
      expr = std::move(nav);
      continue;
    }
    if (check(Tok::Bang)) {
      const Token& opTok = advance();
      auto unwrap = std::make_unique<UnaryExpr>();
      unwrap->span = Span::join(expr->span, opTok.span);
      unwrap->opSpan = opTok.span;
      unwrap->op = "!";
      unwrap->operand = std::move(expr);
      expr = std::move(unwrap);
      continue;
    }
    if (check(Tok::LParen)) {
      auto args = parseCallArgs();
      auto call = std::make_unique<CallExpr>();
      call->span = Span::join(expr->span, previousSpan());
      call->callee = std::move(expr);
      call->args = std::move(args);
      expr = std::move(call);
      continue;
    }
     if (check(Tok::LBracket)) {
       expr = parseCallOrIndexPostfix(std::move(expr));
       continue;
     }
     if (check(Tok::Question)) {
       bool ternary = false;
       int depth = 0;
       for (std::size_t i = pos_ + 1; i < tokens_.size(); ++i) {
         Tok k = tokens_[i].kind;
         if (k == Tok::LParen || k == Tok::LBracket || k == Tok::LBrace) {
           ++depth;
         } else if (k == Tok::RParen || k == Tok::RBracket || k == Tok::RBrace) {
           if (depth == 0) break;
           --depth;
         } else if (depth == 0 && k == Tok::Colon) {
           ternary = true;
           break;
         } else if (depth == 0 && (k == Tok::Comma || k == Tok::Newline || k == Tok::Eof ||
                                    k == Tok::Dedent)) {
           break;
         }
       }
       if (ternary) break;
       const Token& opTok = advance();
       auto tryExpr = std::make_unique<TryExpr>();
       tryExpr->span = Span::join(expr->span, opTok.span);
       tryExpr->operand = std::move(expr);
       expr = std::move(tryExpr);
       continue;
     }
     break;
  }
  return expr;
}

ExprPtr Parser::parseCallOrIndexPostfix(ExprPtr base) {
  Span start = cur().span;
  expect(Tok::LBracket, "'['");

  auto parseSliceTail = [&](std::unique_ptr<SliceExpr> slice) -> ExprPtr {
    if (!check(Tok::RBracket)) {
      if (check(Tok::DotDot)) {
        fail(cur().span, "Expected a slice end before '..'.",
             "Write start..end..step, e.g. items[0..10..2].");
      }
      slice->end = parseExpression(13);
      if (match(Tok::DotDot)) {
        if (check(Tok::RBracket)) {
          fail(cur().span, "Expected a slice step after '..'.");
        }
        slice->step = parseExpression(13);
        if (check(Tok::DotDot)) {
          fail(cur().span, "A slice takes at most a start, end, and step.");
        }
      }
    }
    expect(Tok::RBracket, "']' after the slice");
    slice->bracketSpan = Span::join(start, previousSpan());
    slice->span = Span::join(slice->object->span, previousSpan());
    return slice;
  };

  if (match(Tok::DotDot)) {
    auto slice = std::make_unique<SliceExpr>();
    slice->object = std::move(base);
    return parseSliceTail(std::move(slice));
  }

  auto first = parseExpression(13);
  if (match(Tok::DotDot)) {
    auto slice = std::make_unique<SliceExpr>();
    slice->object = std::move(base);
    slice->start = std::move(first);
    return parseSliceTail(std::move(slice));
  }

  expect(Tok::RBracket, "']' after the index");
  auto index = std::make_unique<IndexExpr>();
  index->object = std::move(base);
  index->index = std::move(first);
  index->bracketSpan = Span::join(start, previousSpan());
  index->span = Span::join(index->object->span, previousSpan());
  return index;
}

std::vector<CallArg> Parser::parseCallArgs() {
  std::vector<CallArg> args;
  expect(Tok::LParen, "'('");
  if (!check(Tok::RParen)) {
    while (true) {
      CallArg arg;
      if (check(Tok::Ident) && peek(1).kind == Tok::Eq) {
        arg.name = cur().lexeme;
        arg.nameSpan = cur().span;
        advance();
        advance();  // '='
      } else if (check(Tok::KwSelf) && peek(1).kind == Tok::Eq) {
        fail(cur().span, "'self' cannot be used as an argument name.");
      }
      arg.value = parseExpression();
      args.push_back(std::move(arg));
      if (!match(Tok::Comma)) break;
      if (check(Tok::RParen)) break;
    }
  }
  expect(Tok::RParen, "')' after the arguments");
  return args;
}

}  // namespace a
