#include "parser/parser_internal.hpp"

namespace a {
// ------------------------------------------------------------- expressions

int Parser::binaryPrecedence(Tok kind) {
  switch (kind) {
    case Tok::Pipe: return 1;
    case Tok::Question: return 2;
    case Tok::KwIf: return 2;
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
    case Tok::SlashSlash:
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
    case Tok::SlashSlash: return "//";
    case Tok::Percent: return "%";
    case Tok::StarStar: return "**";
    case Tok::Amp: return "&";
    case Tok::Bar: return "|";
    case Tok::Caret: return "^";
    case Tok::Tilde: return "~";
    case Tok::Shl: return "<<";
    case Tok::Shr: return ">>";
    case Tok::Eq: return "=";
    case Tok::ColonEq: return "=";
    case Tok::PlusEq: return "+=";
    case Tok::MinusEq: return "-=";
    case Tok::StarEq: return "*=";
    case Tok::SlashEq: return "/=";
    case Tok::SlashSlashEq: return "//=";
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
    case Tok::SlashSlash: return BinOp::FloorDiv;
    case Tok::SlashSlashEq: return BinOp::FloorDiv;
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
    case Tok::ColonEq:
    case Tok::PlusEq:
    case Tok::MinusEq:
    case Tok::StarEq:
    case Tok::SlashEq:
    case Tok::SlashSlashEq:
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

namespace {

bool isChainComparisonTok(Tok kind) {
  switch (kind) {
    case Tok::Lt:
    case Tok::Gt:
    case Tok::Le:
    case Tok::Ge:
    case Tok::EqEq:
    case Tok::NotEq:
    case Tok::KwIn:
    case Tok::KwIs:
      return true;
    default:
      return false;
  }
}

// Middle operands of a chained comparison are duplicated in the AST, so only
// side-effect-free literals and names may appear there.
bool isCloneableSimple(const Expr& expr) {
  switch (expr.kind) {
    case ExprKind::Ident:
    case ExprKind::IntLit:
    case ExprKind::FloatLit:
    case ExprKind::BoolLit:
    case ExprKind::CharLit:
      return true;
    case ExprKind::StringLit: {
      const auto& lit = static_cast<const StringLitExpr&>(expr);
      for (const auto& part : lit.parts) {
        if (part.isExpr) return false;
      }
      return true;
    }
    default:
      return false;
  }
}

ExprPtr cloneSimple(const Expr& expr) {
  ExprPtr out;
  switch (expr.kind) {
    case ExprKind::Ident: {
      auto c = std::make_unique<IdentExpr>();
      c->name = static_cast<const IdentExpr&>(expr).name;
      c->span = expr.span;
      out = std::move(c);
      break;
    }
    case ExprKind::IntLit: {
      auto c = std::make_unique<IntLitExpr>();
      c->value = static_cast<const IntLitExpr&>(expr).value;
      c->span = expr.span;
      out = std::move(c);
      break;
    }
    case ExprKind::FloatLit: {
      auto c = std::make_unique<FloatLitExpr>();
      c->value = static_cast<const FloatLitExpr&>(expr).value;
      c->span = expr.span;
      out = std::move(c);
      break;
    }
    case ExprKind::BoolLit: {
      auto c = std::make_unique<BoolLitExpr>();
      c->value = static_cast<const BoolLitExpr&>(expr).value;
      c->span = expr.span;
      out = std::move(c);
      break;
    }
    case ExprKind::CharLit: {
      auto c = std::make_unique<CharLitExpr>();
      c->value = static_cast<const CharLitExpr&>(expr).value;
      c->span = expr.span;
      out = std::move(c);
      break;
    }
    case ExprKind::StringLit: {
      auto c = std::make_unique<StringLitExpr>();
      const auto& lit = static_cast<const StringLitExpr&>(expr);
      for (const auto& part : lit.parts) {
        if (part.isExpr) return nullptr;
        StringPart p;
        p.isExpr = false;
        p.text = part.text;
        c->parts.push_back(std::move(p));
      }
      c->span = expr.span;
      out = std::move(c);
      break;
    }
    default:
      break;
  }
  return out;
}

}  // namespace

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

    if (cur().kind == Tok::KwAs) {
      // `expr as T` — Python typing.cast parity. Desugars to the builtin
      // `cast<T>(expr)` call; the type argument carries the target type and
      // the runtime value passes through unchanged.
      Span asSpan = advance().span;
      TypeExprPtr target = parseTypeExpr();
      std::vector<CallArg> castArgs;
      CallArg castArg;
      castArg.value = std::move(left);
      castArgs.push_back(std::move(castArg));
      auto ident = std::make_unique<IdentExpr>();
      ident->span = asSpan;
      ident->name = "cast";
      std::vector<TypeExprPtr> castTypes;
      castTypes.push_back(std::move(target));
      left = buildCallExpr(std::move(ident), std::move(castArgs),
                           std::move(castTypes), previousSpan());
      continue;
    }

    Tok kind = cur().kind;
    int prec = binaryPrecedence(kind);
    if (prec < minPrec || prec < 0) break;
    const Token& opTok = advance();
    if (kind == Tok::KwIf) {
      // Python-style inline conditional: `value if cond else other`.
      // `left` is the then-value; the condition sits between 'if' and 'else'.
      auto cond = parseExpression();
      expect(Tok::KwElse, "'else' in the inline conditional");
      auto elseValue = parseExpression(prec);
      auto ternary = std::make_unique<TernaryExpr>();
      ternary->span = Span::join(left->span, elseValue->span);
      ternary->cond = std::move(cond);
      ternary->thenValue = std::move(left);
      ternary->elseValue = std::move(elseValue);
      left = std::move(ternary);
      continue;
    }
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
    if (isChainComparisonTok(kind) && isChainComparisonTok(cur().kind) &&
        !(cur().kind == Tok::KwIs && peek(1).kind == Tok::KwNot)) {
      // Python chained comparison: `a < b < c` means `(a < b) and (b < c)`.
      // Middle operands are duplicated, so they must be simple literals or
      // names (checked after collection, before building the 'and' chain).
      std::vector<ExprPtr> operands;
      operands.push_back(std::move(left));
      operands.push_back(std::move(right));
      std::vector<Tok> ops;
      std::vector<Span> opSpans;
      ops.push_back(kind);
      opSpans.push_back(opTok.span);
      while (isChainComparisonTok(cur().kind) &&
             !(cur().kind == Tok::KwIs && peek(1).kind == Tok::KwNot)) {
        const Token& cop = advance();
        ops.push_back(cop.kind);
        opSpans.push_back(cop.span);
        operands.push_back(parseExpression(nextMin));
      }
      for (std::size_t i = 1; i + 1 < operands.size(); ++i) {
        if (!isCloneableSimple(*operands[i])) {
          fail(operands[i]->span,
               "The middle operand of a chained comparison must be a name or literal.",
               "Give the operand a name first, e.g. `t = expr` then chain on `t`.");
        }
      }
      ExprPtr result;
      for (std::size_t i = 0; i < ops.size(); ++i) {
        ExprPtr lhs = i == 0 ? std::move(operands[0])
                             : cloneSimple(*operands[i]);
        ExprPtr rhs = i + 1 == operands.size() - 1
                          ? std::move(operands[i + 1])
                          : cloneSimple(*operands[i + 1]);
        auto cmp = std::make_unique<BinaryExpr>();
        cmp->span = Span::join(lhs->span, rhs->span);
        cmp->opSpan = opSpans[i];
        cmp->op = opName(ops[i]);
        cmp->binop = binOp(ops[i]);
        cmp->lhs = std::move(lhs);
        cmp->rhs = std::move(rhs);
        if (i == 0) {
          result = std::move(cmp);
        } else {
          auto conj = std::make_unique<BinaryExpr>();
          conj->span = Span::join(result->span, cmp->span);
          conj->opSpan = opSpans[i];
          conj->op = "and";
          conj->binop = BinOp::And;
          conj->lhs = std::move(result);
          conj->rhs = std::move(cmp);
          result = std::move(conj);
        }
      }
      left = std::move(result);
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

namespace {

// Token-level cursor for the `expr<T, ...>(` lookahead. It mirrors the
// parseTypeExpr grammar without producing diagnostics or AST nodes, so a
// failed scan simply means the '<' was a comparison operator. `gt` tracks
// the unused half of a '>>' token that was split while closing nested
// generic argument lists.
struct TypeScan {
  const std::vector<Token>* tokens = nullptr;
  std::size_t pos = 0;
  int gt = 0;

  Tok kind() const {
    if (gt > 0) return Tok::Gt;
    return (*tokens)[pos].kind;
  }
  Tok rawKind() const { return (*tokens)[pos].kind; }
  bool atEnd() const {
    switch (kind()) {
      case Tok::Eof:
      case Tok::Newline:
      case Tok::Indent:
      case Tok::Dedent:
        return true;
      default:
        return false;
    }
  }
  bool eat(Tok k) {
    if (kind() != k) return false;
    if (gt > 0) {
      --gt;
      return true;
    }
    ++pos;
    return true;
  }
  bool eatGt() {
    if (gt > 0) {
      --gt;
      return true;
    }
    Tok k = rawKind();
    if (k == Tok::Gt) {
      ++pos;
      return true;
    }
    if (k == Tok::Shr) {
      // '>>' closes two generic levels at once.
      ++pos;
      gt = 1;
      return true;
    }
    return false;
  }
};

bool scanTypeArgListTokens(TypeScan& s);

bool scanTypeExprTokens(TypeScan& s);
bool scanTypeExprAtomTokens(TypeScan& s);

// `T | U | ...` union types: members are atoms separated by '|'.
bool scanTypeExprTokens(TypeScan& s) {
  if (!scanTypeExprAtomTokens(s)) return false;
  while (s.eat(Tok::Bar)) {
    if (!scanTypeExprAtomTokens(s)) return false;
  }
  return true;
}

bool scanTypeExprAtomTokens(TypeScan& s) {
  const Tok k = s.kind();
  if (k == Tok::LBracket) {
    s.eat(Tok::LBracket);
    if (!scanTypeExprTokens(s)) return false;
    return s.eat(Tok::RBracket);
  }
  if (k == Tok::LParen) {
    s.eat(Tok::LParen);
    if (s.kind() != Tok::RParen) {
      if (!scanTypeExprTokens(s)) return false;
      while (s.eat(Tok::Comma)) {
        if (!scanTypeExprTokens(s)) return false;
      }
    }
    return s.eat(Tok::RParen);
  }
  if (k == Tok::LBrace) {
    s.eat(Tok::LBrace);
    if (!scanTypeExprTokens(s)) return false;
    if (s.eat(Tok::Colon)) {
      if (!scanTypeExprTokens(s)) return false;
    }
    return s.eat(Tok::RBrace);
  }
  if (k == Tok::KwFn) {
    s.eat(Tok::KwFn);
    if (!s.eat(Tok::LParen)) return false;
    if (s.kind() != Tok::RParen) {
      if (!scanTypeExprTokens(s)) return false;
      while (s.eat(Tok::Comma)) {
        if (!scanTypeExprTokens(s)) return false;
      }
    }
    if (!s.eat(Tok::RParen)) return false;
    if (!s.eat(Tok::Arrow)) return false;
    return scanTypeExprTokens(s);
  }
  if (k != Tok::Ident) return false;
  s.eat(Tok::Ident);
  while (s.eat(Tok::Dot)) {
    if (s.kind() != Tok::Ident) return false;
    s.eat(Tok::Ident);
  }
  if (s.kind() == Tok::Lt) {
    if (!scanTypeArgListTokens(s)) return false;
  }
  // Suffixes: fixed-size dims `i32[2][3]` (integer literal) or
  // square-bracket generic application `list[int]` / `vector[float, 4]`.
  while (s.kind() == Tok::LBracket) {
    s.eat(Tok::LBracket);
    if (s.kind() == Tok::Int) {
      s.eat(Tok::Int);
    } else {
      // Square-bracket type arguments; integer literals are comptime
      // arguments (e.g. the lane count in `vector[float, 4]`).
      while (true) {
        if (s.kind() == Tok::Int) {
          s.eat(Tok::Int);
        } else if (s.kind() == Tok::DotDot) {
          // `...` lexes as '..' + '.'; consume both halves.
          s.eat(Tok::DotDot);
          if (s.kind() == Tok::Dot) s.eat(Tok::Dot);
        } else if (!scanTypeExprTokens(s)) {
          return false;
        }
        if (!s.eat(Tok::Comma)) break;
      }
    }
    if (!s.eat(Tok::RBracket)) return false;
  }
  while (s.eat(Tok::Question)) {
  }
  return true;
}

// `<` already consumed conceptually: s sits on '<'. Integer literals are
// allowed as arguments for comptime parameters such as `vector[float, 4]`.
bool scanTypeArgListTokens(TypeScan& s) {
  if (!s.eat(Tok::Lt)) return false;
  while (s.kind() != Tok::Gt && s.rawKind() != Tok::Shr && !s.atEnd()) {
    if (s.kind() == Tok::Int) {
      s.eat(Tok::Int);
    } else if (s.kind() == Tok::DotDot) {
      // `...` lexes as '..' + '.'; consume both halves.
      s.eat(Tok::DotDot);
      if (s.kind() == Tok::Dot) s.eat(Tok::Dot);
    } else if (!scanTypeExprTokens(s)) {
      return false;
    }
    if (!s.eat(Tok::Comma)) break;
  }
  return s.eatGt();
}

}  // namespace

std::size_t Parser::scanCallTypeArgsEnd(std::size_t pos) const {
  constexpr std::size_t npos = ~std::size_t{0};
  TypeScan scan{&tokens_, pos, 0};
  if (!scanTypeArgListTokens(scan)) return npos;
  // The '<...>' must be followed by '(' to be a call's type arguments;
  // otherwise the '<' was a comparison operator.
  if (scan.kind() != Tok::LParen) return npos;
  return scan.pos;
}

void Parser::splitShrGt() {
  // tokens_[pos_] is '>>'; rewrite it as '>' and insert a second '>' token
  // so nested generic argument lists can consume them one at a time.
  Token second = tokens_[pos_];
  tokens_[pos_].kind = Tok::Gt;
  tokens_[pos_].lexeme = ">";
  second.kind = Tok::Gt;
  second.lexeme = ">";
  tokens_.insert(tokens_.begin() + static_cast<std::ptrdiff_t>(pos_) + 1,
                 std::move(second));
}

bool Parser::matchTypeArgGt() {
  if (check(Tok::Gt)) {
    advance();
    return true;
  }
  if (check(Tok::Shr)) {
    splitShrGt();
    advance();
    return true;
  }
  return false;
}

ExprPtr Parser::buildCallExpr(ExprPtr callee, std::vector<CallArg> args,
                              std::vector<TypeExprPtr> typeArgs, Span endSpan) {
  auto call = std::make_unique<CallExpr>();
  call->span = Span::join(callee->span, endSpan);
  call->callee = std::move(callee);
  call->args = std::move(args);
  call->typeArgs = std::move(typeArgs);
  if (auto* nav = dynamic_cast<SafeNavExpr*>(call->callee.get())) {
    // obj?.method(args) -> (obj == None) ? None : obj.method(args).
    // The receiver is duplicated, so it must be a plain name.
    if (dynamic_cast<IdentExpr*>(nav->object.get()) == nullptr) {
      fail(nav->object->span,
           "A method call through '?.' needs a plain-name receiver.",
           "Bind the receiver to a name first, e.g. `r = expr`.");
    }
    auto receiver = std::move(nav->object);
    std::string method = nav->field;
    Span methodSpan = nav->fieldSpan;
    Span wholeSpan = call->span;
    // Condition: receiver == None
    auto cmp = std::make_unique<BinaryExpr>();
    cmp->span = wholeSpan;
    cmp->op = "==";
    cmp->binop = BinOp::Eq;
    auto lhsIdent = std::make_unique<IdentExpr>();
    lhsIdent->name = static_cast<IdentExpr*>(receiver.get())->name;
    lhsIdent->span = receiver->span;
    cmp->lhs = std::move(lhsIdent);
    auto noneIdent = std::make_unique<IdentExpr>();
    noneIdent->name = "None";
    noneIdent->span = wholeSpan;
    cmp->rhs = std::move(noneIdent);
    // Else branch: (!receiver).method(args) — the '!' unwraps the
    // optional before dispatch. Reuse the parsed call.
    auto field = std::make_unique<FieldExpr>();
    field->span = wholeSpan;
    auto unwrap = std::make_unique<UnaryExpr>();
    unwrap->span = wholeSpan;
    unwrap->op = "!";
    unwrap->postfix = true;
    auto unwrapIdent = std::make_unique<IdentExpr>();
    unwrapIdent->name = static_cast<IdentExpr*>(receiver.get())->name;
    unwrapIdent->span = receiver->span;
    unwrap->operand = std::move(unwrapIdent);
    field->object = std::move(unwrap);
    field->field = method;
    field->fieldSpan = methodSpan;
    call->callee = std::move(field);
    auto tern = std::make_unique<TernaryExpr>();
    tern->span = wholeSpan;
    tern->cond = std::move(cmp);
    auto thenNone = std::make_unique<IdentExpr>();
    thenNone->name = "None";
    thenNone->span = wholeSpan;
    tern->thenValue = std::move(thenNone);
    tern->elseValue = std::move(call);
    return tern;
  }
  return call;
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
    if (check(Tok::Lt)) {
      // Explicit call-site type arguments: `f<i64>(x)`, `obj.m<T>(x)`.
      // The lookahead requires a well-formed type list followed by '(', so
      // `a < b` and `a < b > (c)` still parse as comparisons when the
      // '<...>' does not have call shape.
      constexpr std::size_t npos = ~std::size_t{0};
      if (scanCallTypeArgsEnd(pos_) == npos) break;
      advance();  // '<'
      std::vector<TypeExprPtr> typeArgs;
      while (!check(Tok::Gt) && !check(Tok::Shr) && !check(Tok::Eof)) {
        if (check(Tok::Int)) {
          // Comptime type argument, e.g. the lane count in `f<vector[float, 4]>`.
          const Token& lit = advance();
          auto konst = std::make_unique<ConstTypeExpr>();
          konst->span = lit.span;
          konst->value = lit.intValue;
          typeArgs.push_back(std::move(konst));
        } else {
          typeArgs.push_back(parseTypeExpr());
        }
        if (!match(Tok::Comma)) break;
        if (check(Tok::Gt) || check(Tok::Shr)) break;  // trailing comma
      }
      if (typeArgs.empty()) {
        fail(previousSpan(), "Expected a type argument in '<>'.",
             "Write the type arguments explicitly, e.g. f<i64>(x).");
      }
      if (!matchTypeArgGt()) {
        fail(cur().span,
             "Expected '>' after the type arguments, found " +
                 describeToken(cur()) + ".");
      }
      // The scan guaranteed '(' is next.
      auto args = parseCallArgs();
      expr = buildCallExpr(std::move(expr), std::move(args), std::move(typeArgs),
                           previousSpan());
      continue;
    }
    if (check(Tok::LParen)) {
      auto args = parseCallArgs();
      expr = buildCallExpr(std::move(expr), std::move(args), {}, previousSpan());
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
