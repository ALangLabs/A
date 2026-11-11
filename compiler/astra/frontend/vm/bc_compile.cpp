// AST -> bytecode compiler for the slot-based VM (see bytecode.hpp).

#include "vm/bytecode.hpp"

#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <tuple>
#include <utility>

#include "ast/ast.hpp"
#include "ast/builtins.hpp"
#include "codegen/codegen_internal.hpp"  // builtinRuntimeId / builtinIdForBuiltin
#include "name-resolution/module_loader.hpp"  // stemOf
#include "name-resolution/native_modules.hpp"
#include "type-checker/sema.hpp"

extern "C" {
#include "a_runtime.h"
}

namespace a::vm::bc {

State g_bc;  // shared with bc_run.cpp

namespace {

struct BcUnsupported {
  std::string msg;
};

[[noreturn]] void bcUnsupported(const std::string& what) {
  throw BcUnsupported{what};
}
constexpr aval kMissing = (aval)A_TAG;  // padding sentinel for defaulted args

// Compile-time constants mirror codegen emitIdent (checked before bindings,
// so they cannot be shadowed).
bool constValue(const std::string& nm, aval& out) {
  if (nm == "None") { out = A_NONE; return true; }
  if (nm == "PI") { out = a_double_canon(3.14159265358979323846); return true; }
  if (nm == "E") { out = a_double_canon(2.71828182845904523536); return true; }
  if (nm == "I8_MIN") { out = a_mkint(-128); return true; }
  if (nm == "I8_MAX") { out = a_mkint(127); return true; }
  if (nm == "I16_MIN") { out = a_mkint(-32768); return true; }
  if (nm == "I16_MAX") { out = a_mkint(32767); return true; }
  if (nm == "I32_MIN") { out = a_mkint(-2147483648LL); return true; }
  if (nm == "I32_MAX") { out = a_mkint(2147483647LL); return true; }
  if (nm == "I64_MIN") { out = a_i64n(INT64_MIN); return true; }
  if (nm == "I64_MAX") { out = a_i64n(INT64_MAX); return true; }
  if (nm == "U8_MAX") { out = a_mkint(255); return true; }
  if (nm == "U16_MAX") { out = a_mkint(65535); return true; }
  if (nm == "U32_MAX") { out = a_mkint(4294967295LL); return true; }
  if (nm == "F32_MIN") { out = a_double_canon(-3.4028234663852886e38); return true; }
  if (nm == "F32_MAX") { out = a_double_canon(3.4028234663852886e38); return true; }
  if (nm == "F64_MIN") { out = a_double_canon(-1.7976931348623157e308); return true; }
  if (nm == "F64_MAX") { out = a_double_canon(1.7976931348623157e308); return true; }
  return false;
}

bool literalConst(const Expr& e, aval& out);

struct Compiler {
  const ModuleGraph& graph;
  const SemaResult& sema;
  Code& code;
  std::vector<std::unordered_map<std::string, int>> scopes;
  std::vector<int> scopeBases;
  int next = 0;
  int maxSlot = 0;
  bool topLevel = false;  // direct module statement list -> globals
  Compiler* parent = nullptr;  // enclosing fn compiler (nullptr = module top)
  std::unordered_set<int> pendingCaptures;  // own slots children capture
  std::unordered_set<int> boxed;            // emit pass: slots holding cells
  std::unordered_map<std::string, std::uint32_t> upvalMap;
  std::vector<std::vector<int>> breakPatches;
  std::vector<int> brkDepths;   // scope idx a break unwinds defers down to
  std::vector<int> contDepths;  // scope idx a continue unwinds defers down to
  std::vector<std::vector<int>> contPatches;
  std::vector<int> contTargets;  // ip each continue stack patches to

  Compiler(const ModuleGraph& g, const SemaResult& s, Code& c)
      : graph(g), sema(s), code(c) {}

  int emit(Op op, int a = 0, int b = 0, int c = 0, int d = 0, int e = 0) {
    code.ins.push_back({op, a, b, c, d, e});
    return (int)code.ins.size() - 1;
  }
  void patch(int at, int a) { code.ins[at].a = a; }
  int addConst(aval v) {
    code.consts.push_back(v);
    return (int)code.consts.size() - 1;
  }
  int addName(const std::string& s) {
    code.names.push_back(s);
    return (int)code.names.size() - 1;
  }
  int allocSlot() {
    int s = next++;
    if (next > maxSlot) maxSlot = next;
    return s;
  }
  int allocTemps(int n) {
    int base = next;
    next += n;
    if (next > maxSlot) maxSlot = next;
    return base;
  }
  void freeTemps(int mark) { next = mark; }

  std::vector<int> deferSlots;  // per-scope defer list slot (-1 = none)

  void pushScope() {
    scopeBases.push_back(next);
    scopes.emplace_back();
    deferSlots.push_back(-1);
  }
  void popScope() {
    scopes.pop_back();
    next = scopeBases.back();
    scopeBases.pop_back();
    deferSlots.pop_back();
  }

  // Emit RunDefers for scopes [scopeIdx, top] (deepest first).
  void emitRunDefersFrom(int scopeIdx) {
    for (int i = (int)deferSlots.size() - 1; i >= scopeIdx; --i)
      if (deferSlots[i] >= 0) emit(Op::RunDefers, deferSlots[i]);
  }
  // Run just the innermost scope's defers (normal block exit).
  void emitRunDefersTop() {
    if (!deferSlots.empty() && deferSlots.back() >= 0)
      emit(Op::RunDefers, deferSlots.back());
  }

  int findLocal(const std::string& name) const {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
      auto f = it->find(name);
      if (f != it->end()) return f->second;
    }
    return -1;
  }
  int findGlobal(const std::string& name) const {
    auto& m = g_bc.modules[code.moduleId];
    auto f = m.slotOf.find(name);
    return f == m.slotOf.end() ? -1 : f->second;
  }
  // 0=local slot, 1=global slot, 2=const, 3=builtin; -1 unresolved.
  int resolveRead(const std::string& name, int& out) {
    aval cv;
    if (constValue(name, cv)) {
      out = addConst(cv);
      return 2;
    }
    int s = findLocal(name);
    if (s >= 0) { out = s; return 0; }
    // Enclosing function locals (depth-1 captures only).
    if (parent) {
      int depth = 0;
      for (Compiler* p = parent; p; p = p->parent) {
        ++depth;
        for (auto it = p->scopes.rbegin(); it != p->scopes.rend(); ++it) {
          auto f = it->find(name);
          if (f != it->end()) {
            if (depth > 1)
              bcUnsupported("nested capture beyond one level");
            auto ui = upvalMap.find(name);
            std::uint32_t idx;
            if (ui == upvalMap.end()) {
              idx = (std::uint32_t)code.upvals.size();
              code.upvals.push_back((std::uint32_t)f->second);
              upvalMap[name] = idx;
            } else idx = ui->second;
            p->pendingCaptures.insert(f->second);
            out = (int)idx;
            return 4;
          }
        }
      }
    }
    int g = findGlobal(name);
    if (g >= 0) { out = g; return 1; }
    int bid = builtinRuntimeId(name);
    if (bid >= 0) { out = bid; return 3; }
    return -1;
  }

  int loadName(const std::string& name) {
    int v = 0, kind = resolveRead(name, v);
    int dst = allocSlot();
    if (kind == 0) {
      if (boxed.count(v)) emit(Op::CellGet, dst, v);
      else emit(Op::Move, dst, v);
    } else if (kind == 1) emit(Op::LoadGlobal, dst, v);
    else if (kind == 2) emit(Op::LoadConst, dst, v);
    else if (kind == 3) emit(Op::LoadBuiltin, dst, v);
    else if (kind == 4) emit(Op::GetUpval, dst, v);
    else bcUnsupported("undefined name '" + name + "'");
    return dst;
  }

  // Assignment target resolution: existing binding or fresh local/global.
  // kind out-param: 0 local slot, 1 global, 4 upvalue. isNew: slot just made.
  std::tuple<int, int, bool> lvalue(const std::string& name) {
    int v = 0;
    int kind = resolveRead(name, v);
    if (kind == 0) return {0, v, false};
    if (kind == 1) return {1, v, false};
    if (kind == 4) return {4, v, false};
    if (topLevel) {
      auto& m = g_bc.modules[code.moduleId];
      auto f = m.slotOf.find(name);
      if (f != m.slotOf.end()) return {1, f->second, false};
      bcUnsupported("assignment to undeclared module name '" + name + "'");
    }
    int s = allocSlot();
    scopes.back()[name] = s;
    return {0, s, true};
  }

  // Store `src` into the binding for `name`.
  void storeName(const std::string& name, int src) {
    auto [kind, idx, isNew] = lvalue(name);
    if (kind == 1) { emit(Op::StoreGlobal, idx, src); return; }
    if (kind == 4) { emit(Op::SetUpval, idx, src); return; }
    if (boxed.count(idx)) {
      if (isNew) {
        emit(Op::Move, idx, src);
        emit(Op::MakeCell, idx);
      } else {
        emit(Op::CellSet, idx, src);
      }
    } else {
      emit(Op::Move, idx, src);
    }
  }

  int compileExpr(const Expr* e);
  void compileExprInto(const Expr* e, int dst);
  int compileCall(const CallExpr& c);
  int emitCallBuiltin(const CallExpr& c, int id);
  Op binOpFor(BinOp b, const std::string& op);
  int compileReadTarget(const Expr* t);
  void compileStoreTarget(const Expr* t, int src);
  void compileStmts(const std::vector<StmtPtr>& stmts);
  void compileStmt(const Stmt* s);
  uint32_t compileFnDecl(const FnDecl& fn, Compiler* parentC);
  uint32_t compileAnonStmt(const Stmt* body, Compiler* parentC);
  void compilePattern(const Pattern& p, int subj, std::vector<int>& failPatches);
  uint32_t compileLambda(const LambdaExpr& lam);
};

// ---------------------------------------------------------------------------
// Functions
// ---------------------------------------------------------------------------
static void bindParams(Compiler& c, Code& sub, const FnDecl* fn,
                        const LambdaExpr* lam) {
  int pi = 0;
  auto bind = [&](const std::string& nm, const ExprPtr& def, bool variadic) {
    c.scopes.back()[nm] = pi++;
    sub.paramNames.push_back(nm);
    if (def) {
      aval dv;
      if (!literalConst(*def, dv))
        bcUnsupported("non-literal parameter default");
      sub.paramDefaults.push_back(c.addConst(dv));
    } else {
      sub.paramDefaults.push_back(-1);
    }
    if (variadic) sub.variadic = true;
  };
  if (fn) {
    for (const ComptimeParam& p : fn->comptimeParams) bind(p.name, nullptr, false);
    for (const ParamDecl& p : fn->params) bind(p.name, p.defaultValue, p.variadic);
  } else if (lam) {
    for (const ParamDecl& p : lam->params) bind(p.name, p.defaultValue, p.variadic);
  }
  c.next = pi;
  c.maxSlot = pi;
}

// Discover pass: compile the body into a scratch Code. Captures of the
// enclosing compiler's slots are recorded into enclosing.pendingCaptures
// automatically by resolveRead's parent walk; captures of OUR OWN slots by
// our children land in d.pendingCaptures, returned for the emit pass.
static std::unordered_set<int> discoverCaptures(Compiler& enclosing,
                                              const FnDecl* fn,
                                              const LambdaExpr* lam) {
  Code scratch;
  scratch.moduleId = enclosing.code.moduleId;
  Compiler d(enclosing.graph, enclosing.sema, scratch);
  d.parent = &enclosing;
  d.topLevel = false;
  d.pushScope();
  bindParams(d, scratch, fn, lam);
  if (fn) d.compileStmts(fn->body);
  else {
    int r = d.compileExpr(lam->body.get());
    d.emit(Op::Return, r);
  }
  return d.pendingCaptures;
}

uint32_t Compiler::compileLambda(const LambdaExpr& lam) {
  auto it = g_bc.lambdaCode.find(&lam);
  if (it != g_bc.lambdaCode.end()) return it->second;
  g_bc.codes.emplace_back();
  uint32_t idx = (uint32_t)g_bc.codes.size() - 1;
  g_bc.lambdaCode[&lam] = idx;
  Code& sub = g_bc.codes[idx];
  sub.moduleId = code.moduleId;
  sub.name = "<lambda>";
  sub.line = lam.span.line;
  auto ownCaptured = discoverCaptures(*this, nullptr, &lam);
  Compiler c(graph, sema, sub);
  c.parent = this;
  c.boxed = ownCaptured;
  c.pushScope();
  bindParams(c, sub, nullptr, &lam);
  // Box captured params.
  for (int i = 0; i < (int)sub.paramNames.size(); ++i)
    if (c.boxed.count(i)) c.emit(Op::MakeCell, i);
  int r = c.compileExpr(lam.body.get());
  c.emit(Op::Return, r);
  sub.frameSize = c.maxSlot;
  return idx;
}

uint32_t Compiler::compileAnonStmt(const Stmt* body, Compiler* parentC) {
  auto it = g_bc.anonCode.find(body);
  if (it != g_bc.anonCode.end()) return it->second;
  g_bc.codes.emplace_back();
  uint32_t idx = (uint32_t)g_bc.codes.size() - 1;
  g_bc.anonCode[body] = idx;
  Code& sub = g_bc.codes[idx];
  sub.moduleId = code.moduleId;
  sub.name = "<defer>";
  // Discover pass: captures of enclosing slots -> this->pendingCaptures;
  // captures of the anon body's own slots -> ownCaptured for boxing.
  std::unordered_set<int> ownCaptured;
  {
    Code scratch;
    scratch.moduleId = code.moduleId;
    Compiler d(graph, sema, scratch);
    d.parent = this;
    d.pushScope();
    d.compileStmt(body);
    d.emitRunDefersFrom(0);
    int ns = d.allocSlot();
    d.emit(Op::LoadNone, ns);
    d.emit(Op::Return, ns);
    ownCaptured = d.pendingCaptures;
  }
  Compiler c2(graph, sema, sub);
  c2.parent = parentC;
  c2.boxed = ownCaptured;
  c2.pushScope();
  c2.compileStmt(body);
  c2.emitRunDefersFrom(0);
  int ns = c2.allocSlot();
  c2.emit(Op::LoadNone, ns);
  c2.emit(Op::Return, ns);
  sub.frameSize = c2.maxSlot;
  return idx;
}

void Compiler::compilePattern(const Pattern& p, int subj,
                                std::vector<int>& failPatches) {
  switch (p.kind) {
    case Pattern::Kind::Wildcard:
      return;
    case Pattern::Kind::Binding: {
      int s = allocSlot();
      scopes.back()[p.name] = s;
      emit(Op::Move, s, subj);
      return;
    }
    case Pattern::Kind::Literal: {
      aval v;
      if (!literalConst(*p.value, v)) bcUnsupported("non-literal match pattern");
      int pc = allocSlot();
      emit(Op::LoadConst, pc, addConst(v));
      int t = allocSlot();
      emit(Op::Eq, t, subj, pc);
      failPatches.push_back(emit(Op::JumpIfFalse, t, 0));
      return;
    }
    case Pattern::Kind::Range: {
      aval lo, hi;
      if (!literalConst(*p.rangeStart, lo) || !literalConst(*p.rangeEnd, hi))
        bcUnsupported("non-literal range pattern");
      int cl = allocSlot(), ch = allocSlot();
      emit(Op::LoadConst, cl, addConst(lo));
      emit(Op::LoadConst, ch, addConst(hi));
      int t1 = allocSlot(), t2 = allocSlot();
      emit(Op::Ge, t1, subj, cl);
      failPatches.push_back(emit(Op::JumpIfFalse, t1, 0));
      emit(Op::Le, t2, subj, ch);
      failPatches.push_back(emit(Op::JumpIfFalse, t2, 0));
      return;
    }
    case Pattern::Kind::Or: {
      // Alternatives share the arm scope; failed alternatives may leave
      // stale bindings (later alternatives overwrite them).
      for (const Pattern& alt : p.subpatterns) {
        if (alt.kind == Pattern::Kind::Tuple ||
            alt.kind == Pattern::Kind::Variant)
          bcUnsupported("complex or-pattern");
      }
      std::vector<int> matchJumps;
      const std::size_t n = p.subpatterns.size();
      for (std::size_t i = 0; i < n; ++i) {
        if (i + 1 == n) {
          // Last alternative: failures propagate to the arm's fail point.
          compilePattern(p.subpatterns[i], subj, failPatches);
        } else {
          std::vector<int> altFail;
          compilePattern(p.subpatterns[i], subj, altFail);
          matchJumps.push_back(emit(Op::Jump, 0));
          int nextAlt = (int)code.ins.size();
          for (int at : altFail) code.ins[at].b = nextAlt;
        }
      }
      int armBody = (int)code.ins.size();
      for (int at : matchJumps) code.ins[at].a = armBody;
      return;
    }
    default:
      bcUnsupported("unsupported match pattern kind");
  }
}

uint32_t Compiler::compileFnDecl(const FnDecl& fn, Compiler* parentC) {
  auto it = g_bc.declCode.find(&fn);
  if (it != g_bc.declCode.end()) return it->second;
  g_bc.codes.emplace_back();
  uint32_t idx = (uint32_t)g_bc.codes.size() - 1;
  g_bc.declCode[&fn] = idx;
  Code& sub = g_bc.codes[idx];
  sub.moduleId = fn.moduleId >= 0 ? fn.moduleId : code.moduleId;
  sub.name = fn.name;
  sub.line = fn.span.line;
  auto ownCaptured = discoverCaptures(*this, &fn, nullptr);
  Compiler c(graph, sema, sub);
  c.parent = parentC;
  c.boxed = ownCaptured;
  c.pushScope();
  bindParams(c, sub, &fn, nullptr);
  for (int i = 0; i < (int)sub.paramNames.size(); ++i)
    if (c.boxed.count(i)) c.emit(Op::MakeCell, i);
  c.compileStmts(fn.body);
  c.emitRunDefersFrom(0);
  int ns = c.allocSlot();
  c.emit(Op::LoadNone, ns);
  c.emit(Op::Return, ns);
  sub.frameSize = c.maxSlot;
  return idx;
}

bool literalConst(const Expr& e, aval& out) {
  switch (e.kind) {
    case ExprKind::IntLit: {
      int64_t v = static_cast<const IntLitExpr&>(e).value;
      out = a_in_inline_range(v) ? a_mkint(v) : a_i64n((long long)v);
      return true;
    }
    case ExprKind::FloatLit:
      out = a_double_canon(static_cast<const FloatLitExpr&>(e).value);
      return true;
    case ExprKind::BoolLit:
      out = static_cast<const BoolLitExpr&>(e).value ? A_TRUE : A_FALSE;
      return true;
    case ExprKind::CharLit: {
      char32_t cp = static_cast<const CharLitExpr&>(e).value;
      char buf[4];
      int n = 0;
      if (cp < 0x80) buf[n++] = (char)cp;
      else if (cp < 0x800) {
        buf[n++] = (char)(0xC0 | (cp >> 6));
        buf[n++] = (char)(0x80 | (cp & 0x3F));
      } else if (cp < 0x10000) {
        buf[n++] = (char)(0xE0 | (cp >> 12));
        buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[n++] = (char)(0x80 | (cp & 0x3F));
      } else {
        buf[n++] = (char)(0xF0 | (cp >> 18));
        buf[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[n++] = (char)(0x80 | (cp & 0x3F));
      }
      out = a_char_new(buf, (uint64_t)n);
      return true;
    }
    case ExprKind::StringLit: {
      const auto& x = static_cast<const StringLitExpr&>(e);
      if (x.parts.size() == 1 && !x.parts[0].isExpr) {
        out = a_str_new(x.parts[0].text.data(), x.parts[0].text.size());
        return true;
      }
      return false;
    }
    default:
      return false;
  }
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------
void Compiler::compileExprInto(const Expr* e, int dst) {
  int t = compileExpr(e);
  if (t != dst) emit(Op::Move, dst, t);
}

int Compiler::compileExpr(const Expr* e) {
  if (!e) {
    int d = allocSlot();
    emit(Op::LoadNone, d);
    return d;
  }
  aval lit;
  switch (e->kind) {
    case ExprKind::IntLit:
    case ExprKind::FloatLit:
    case ExprKind::BoolLit:
    case ExprKind::CharLit:
      if (!literalConst(*e, lit)) bcUnsupported("literal");
      {
        int d = allocSlot();
        emit(Op::LoadConst, d, addConst(lit));
        return d;
      }
    case ExprKind::StringLit: {
      const auto& x = static_cast<const StringLitExpr&>(*e);
      if (x.parts.empty() ||
          (x.parts.size() == 1 && !x.parts[0].isExpr)) {
        std::string txt =
            x.parts.empty() ? "" : x.parts[0].text;
        int d = allocSlot();
        emit(Op::LoadConst, d,
             addConst(a_str_new(txt.data(), txt.size())));
        return d;
      }
      int base = allocTemps((int)x.parts.size());
      int strId = builtinRuntimeId("str");
      for (std::size_t i = 0; i < x.parts.size(); ++i) {
        const StringPart& p = x.parts[i];
        int s = base + (int)i;
        if (p.isExpr) {
          compileExprInto(p.expr.get(), s);
          emit(Op::CallBuiltin, s, strId, s, 1);
        } else {
          emit(Op::LoadConst, s,
               addConst(a_str_new(p.text.data(), p.text.size())));
        }
      }
      int d = allocSlot();
      emit(Op::StrJoin, d, base, (int)x.parts.size());
      return d;
    }
    case ExprKind::Ident:
      return loadName(static_cast<const IdentExpr&>(*e).name);
    case ExprKind::Unary: {
      const auto& x = static_cast<const UnaryExpr&>(*e);
      if (x.op == "++" || x.op == "--") {
        if (x.operand->kind != ExprKind::Ident)
          bcUnsupported("operand of '" + x.op + "' must be a variable");
        const std::string& nm =
            static_cast<const IdentExpr*>(x.operand.get())->name;
        auto [kind, idx, isNew] = lvalue(nm);
        int cur = allocSlot();
        if (kind == 1) emit(Op::LoadGlobal, cur, idx);
        else if (kind == 4) emit(Op::GetUpval, cur, idx);
        else if (boxed.count(idx)) emit(Op::CellGet, cur, idx);
        else emit(Op::Move, cur, idx);
        int delta = allocSlot();
        emit(Op::LoadConst, delta, addConst(a_mkint(x.op == "++" ? 1 : -1)));
        int nxt = allocSlot();
        emit(Op::Add, nxt, cur, delta);
        if (kind == 1) emit(Op::StoreGlobal, idx, nxt);
        else if (kind == 4) emit(Op::SetUpval, idx, nxt);
        else if (boxed.count(idx)) emit(Op::CellSet, idx, nxt);
        else emit(Op::Move, idx, nxt);
        return x.postfix ? cur : nxt;
      }
      int v = compileExpr(x.operand.get());
      int d = allocSlot();
      if (x.op == "-") emit(Op::Neg, d, v);
      else if (x.op == "~") emit(Op::BNot, d, v);
      else if (x.op == "not") emit(Op::Not, d, v);
      else if (x.op == "!") emit(Op::Unwrap, d, v);
      else bcUnsupported("unsupported unary operator '" + x.op + "'");
      return d;
    }
    case ExprKind::Binary: {
      const auto& x = static_cast<const BinaryExpr&>(*e);
      switch (x.binop) {
        case BinOp::And: {
          int ls = compileExpr(x.lhs.get());
          int jf = emit(Op::JumpIfFalse, ls, 0);
          int rs = compileExpr(x.rhs.get());
          int d = allocSlot();
          emit(Op::Move, d, rs);
          int jend = emit(Op::Jump, 0);
          int lfalse = (int)code.ins.size();
          emit(Op::Move, d, ls);
          code.ins[jf].b = lfalse;
          code.ins[jend].a = (int)code.ins.size();
          return d;
        }
        case BinOp::Or: {
          int ls = compileExpr(x.lhs.get());
          int jt = emit(Op::JumpIfTrue, ls, 0);
          int rs = compileExpr(x.rhs.get());
          int d = allocSlot();
          emit(Op::Move, d, rs);
          int jend = emit(Op::Jump, 0);
          int ltrue = (int)code.ins.size();
          emit(Op::Move, d, ls);
          code.ins[jt].b = ltrue;
          code.ins[jend].a = (int)code.ins.size();
          return d;
        }
        case BinOp::Coalesce: {
          int ls = compileExpr(x.lhs.get());
          int nc = allocSlot();
          emit(Op::LoadConst, nc, addConst(A_NONE));
          int eq = allocSlot();
          emit(Op::Eq, eq, ls, nc);
          int jrhs = emit(Op::JumpIfFalse, eq, 0);
          int rs = compileExpr(x.rhs.get());
          int d = allocSlot();
          emit(Op::Move, d, rs);
          int jend = emit(Op::Jump, 0);
          int lnone = (int)code.ins.size();
          emit(Op::Unwrap, d, ls);
          code.ins[jrhs].b = lnone;
          code.ins[jend].a = (int)code.ins.size();
          return d;
        }
        case BinOp::In:
        case BinOp::NotIn: {
          int item = compileExpr(x.lhs.get());
          int cont = compileExpr(x.rhs.get());
          int d = allocSlot();
          emit(Op::In, d, item, cont);
          if (x.binop == BinOp::NotIn) {
            int d2 = allocSlot();
            emit(Op::Not, d2, d);
            return d2;
          }
          return d;
        }
        case BinOp::Range: {
          int lo = compileExpr(x.lhs.get());
          int hi = compileExpr(x.rhs.get());
          int one = allocSlot();
          emit(Op::LoadConst, one, addConst(a_mkint(1)));
          int d = allocSlot();
          emit(Op::MakeRange, d, lo, hi, one);
          return d;
        }
        default: break;
      }
      int l = compileExpr(x.lhs.get());
      int r = compileExpr(x.rhs.get());
      int d = allocSlot();
      emit(binOpFor(x.binop, x.op), d, l, r);
      return d;
    }
    case ExprKind::Index: {
      const auto& x = static_cast<const IndexExpr&>(*e);
      int o = compileExpr(x.object.get());
      int i = compileExpr(x.index.get());
      int d = allocSlot();
      emit(Op::GetItem, d, o, i);
      return d;
    }
    case ExprKind::Slice: {
      const auto& x = static_cast<const SliceExpr&>(*e);
      int base = allocTemps(4);
      compileExprInto(x.object.get(), base);
      if (x.start) compileExprInto(x.start.get(), base + 1);
      else emit(Op::LoadNone, base + 1);
      if (x.end) compileExprInto(x.end.get(), base + 2);
      else emit(Op::LoadNone, base + 2);
      if (x.step) compileExprInto(x.step.get(), base + 3);
      else emit(Op::LoadNone, base + 3);
      int d = allocSlot();
      emit(Op::Slice, d, base, 4);
      return d;
    }
    case ExprKind::Field: {
      const auto& x = static_cast<const FieldExpr&>(*e);
      int o = compileExpr(x.object.get());
      int d = allocSlot();
      emit(Op::GetField, d, o, addName(x.field));
      return d;
    }
    case ExprKind::SafeNav: {
      const auto& x = static_cast<const SafeNavExpr&>(*e);
      int obj = compileExpr(x.object.get());
      int dst = allocSlot();
      int nidx = addName(x.field);
      emit(Op::SafeNav, dst, obj, nidx, x.fieldOptional ? 1 : 0);
      return dst;
    }
    case ExprKind::ArrayLit: {
      const auto& x = static_cast<const ArrayLitExpr&>(*e);
      int base = allocTemps((int)x.elements.size());
      for (std::size_t i = 0; i < x.elements.size(); ++i)
        compileExprInto(x.elements[i].get(), base + (int)i);
      int d = allocSlot();
      emit(Op::NewList, d, base, (int)x.elements.size());
      return d;
    }
    case ExprKind::Tuple: {
      const auto& x = static_cast<const TupleExpr&>(*e);
      int base = allocTemps((int)x.elements.size());
      for (std::size_t i = 0; i < x.elements.size(); ++i)
        compileExprInto(x.elements[i].get(), base + (int)i);
      int d = allocSlot();
      emit(Op::NewTuple, d, base, (int)x.elements.size());
      return d;
    }
    case ExprKind::MapLit: {
      const auto& x = static_cast<const MapLitExpr&>(*e);
      int d = allocSlot();
      emit(Op::NewDict, d, 0, 0);
      for (const auto& kv : x.entries) {
        int k = compileExpr(kv.first.get());
        int v = compileExpr(kv.second.get());
        emit(Op::SetItem, d, k, v);
      }
      return d;
    }
    case ExprKind::SetLit: {
      const auto& x = static_cast<const SetLitExpr&>(*e);
      int base = allocTemps((int)x.elements.size());
      for (std::size_t i = 0; i < x.elements.size(); ++i)
        compileExprInto(x.elements[i].get(), base + (int)i);
      int d = allocSlot();
      emit(Op::NewSet, d, base, (int)x.elements.size());
      return d;
    }
    case ExprKind::Range: {
      const auto& x = static_cast<const RangeExpr&>(*e);
      int lo = compileExpr(x.start.get());
      int hi = compileExpr(x.end.get());
      int st;
      if (x.step) st = compileExpr(x.step.get());
      else {
        st = allocSlot();
        emit(Op::LoadConst, st, addConst(a_mkint(1)));
      }
      int d = allocSlot();
      emit(Op::MakeRange, d, lo, hi, st);
      return d;
    }
    case ExprKind::Ternary: {
      const auto& x = static_cast<const TernaryExpr&>(*e);
      int cond = compileExpr(x.cond.get());
      int jf = emit(Op::JumpIfFalse, cond, 0);
      int t = compileExpr(x.thenValue.get());
      int d = allocSlot();
      emit(Op::Move, d, t);
      int jend = emit(Op::Jump, 0);
      int lelse = (int)code.ins.size();
      int ev = compileExpr(x.elseValue.get());
      emit(Op::Move, d, ev);
      code.ins[jf].b = lelse;
      code.ins[jend].a = (int)code.ins.size();
      return d;
    }
    case ExprKind::Lambda: {
      const auto& x = static_cast<const LambdaExpr&>(*e);
      uint32_t ci = compileLambda(x);
      int d = allocSlot();
      emit(Op::Closure, d, (int)ci, 0);
      return d;
    }
    case ExprKind::Comprehension: {
      const auto& x = static_cast<const ComprehensionExpr&>(*e);
      int isrc = compileExpr(x.iterable.get());
      int iter = allocSlot();
      emit(Op::IterInit, iter, isrc);
      int acc = allocSlot();
      emit(x.isSet ? Op::NewSet : Op::NewList, acc, 0, 0);
      pushScope();
      int varSlot = allocSlot();
      scopes.back()[x.var] = varSlot;
      bool cvarBoxed = boxed.count(varSlot);
      if (cvarBoxed) {
        emit(Op::LoadNone, varSlot);
        emit(Op::MakeCell, varSlot);
      }
      int cvarTmp = cvarBoxed ? allocSlot() : varSlot;
      int top = (int)code.ins.size();
      int iterNext = emit(Op::IterNext, cvarTmp, iter, 0);
      if (cvarBoxed) emit(Op::CellSet, varSlot, cvarTmp);
      int jf = -1;
      if (x.condition) {
        int cond = compileExpr(x.condition.get());
        jf = emit(Op::JumpIfFalse, cond, 0);
      }
      int elem = compileExpr(x.element.get());
      emit(x.isSet ? Op::SetAdd : Op::ListAppend, acc, elem);
      int lskip = (int)code.ins.size();
      if (jf >= 0) code.ins[jf].b = lskip;
      emit(Op::Jump, top);
      int end = (int)code.ins.size();
      code.ins[iterNext].c = end;
      popScope();
      return acc;
    }
    case ExprKind::Match: {
      const auto& m = static_cast<const MatchExpr&>(*e);
      int subj = compileExpr(m.subject.get());
      int dst = allocSlot();
      std::vector<int> endJumps;
      for (const MatchArm& arm : m.arms) {
        pushScope();
        std::vector<int> nextArm;
        compilePattern(arm.pattern, subj, nextArm);
        if (arm.guard) {
          int g = compileExpr(arm.guard.get());
          nextArm.push_back(emit(Op::JumpIfFalse, g, 0));
        }
        compileExprInto(arm.result.get(), dst);
        endJumps.push_back(emit(Op::Jump, 0));
        int next = (int)code.ins.size();
        for (int at : nextArm) code.ins[at].b = next;
        popScope();
      }
      emit(Op::MatchFail, subj);
      int end = (int)code.ins.size();
      for (int at : endJumps) code.ins[at].a = end;
      return dst;
    }
    case ExprKind::Try: {
      const auto& x = static_cast<const TryExpr&>(*e);
      int v = compileExpr(x.operand.get());
      int dst = allocSlot();
      emit(Op::TryQ, dst, v);
      return dst;
    }
    case ExprKind::Call:
      return compileCall(*static_cast<const CallExpr*>(e));
    default:
      bcUnsupported("unsupported expression kind");
  }
}

Op Compiler::binOpFor(BinOp b, const std::string& op) {
  switch (b) {
    case BinOp::Add: return Op::Add;
    case BinOp::Sub: return Op::Sub;
    case BinOp::Mul: return Op::Mul;
    case BinOp::Div: return Op::Div;
    case BinOp::FloorDiv: return Op::FloorDiv;
    case BinOp::Mod: return Op::Mod;
    case BinOp::Pow: return Op::Pow;
    case BinOp::Eq: return Op::Eq;
    case BinOp::Ne: return Op::Ne;
    case BinOp::Lt: return Op::Lt;
    case BinOp::Le: return Op::Le;
    case BinOp::Gt: return Op::Gt;
    case BinOp::Ge: return Op::Ge;
    case BinOp::BitAnd: return Op::BitAnd;
    case BinOp::BitOr: return Op::BitOr;
    case BinOp::BitXor: return Op::BitXor;
    case BinOp::Shl: return Op::Shl;
    case BinOp::Shr: return Op::Shr;
    default: break;
  }
  if (op == "||") return Op::BitOr;
  if (op == "&&") return Op::BitAnd;
  bcUnsupported("unsupported binary operator '" + op + "'");
}

int Compiler::compileCall(const CallExpr& c) {
  if (c.builtinId != BuiltinId::None) {
    int id = builtinIdForBuiltin(c.builtinId);
    if (id >= 0) return emitCallBuiltin(c, id);
  }
  if (c.calleeIsBuiltin) {
    std::string nm = c.calleeBuiltin;
    if (nm.empty() && c.callee->kind == ExprKind::Ident)
      nm = static_cast<const IdentExpr*>(c.callee.get())->name;
    int bid = builtinRuntimeId(nm);
    if (bid >= 0) return emitCallBuiltin(c, bid);
  }
  if (c.calleeStruct)
    bcUnsupported("struct constructor calls are not supported in --vm mode");

  if (c.calleeFunction) {
    uint32_t ci;
    auto it = g_bc.declCode.find(c.calleeFunction);
    if (it != g_bc.declCode.end()) ci = it->second;
    else ci = compileFnDecl(*c.calleeFunction, nullptr);
    const Code& callee = g_bc.codes[ci];
    std::vector<const Expr*> pos;
    std::vector<std::pair<std::string, const Expr*>> kw;
    for (const CallArg& a : c.args) {
      if (a.name.empty()) pos.push_back(a.value.get());
      else kw.emplace_back(a.name, a.value.get());
    }
    if (!kw.empty()) {
      std::vector<const Expr*> expanded(callee.paramNames.size(), nullptr);
      for (std::size_t i = 0; i < pos.size() && i < expanded.size(); ++i)
        expanded[i] = pos[i];
      for (auto& k : kw) {
        bool done = false;
        for (std::size_t i = 0; i < callee.paramNames.size(); ++i) {
          if (callee.paramNames[i] == k.first) {
            if (i >= expanded.size()) expanded.resize(i + 1);
            expanded[i] = k.second;
            done = true;
            break;
          }
        }
        if (!done) bcUnsupported("no parameter named '" + k.first + "'");
      }
      pos = std::move(expanded);
    }
    int nargs = (int)pos.size();
    int argBase = allocTemps(nargs);
    for (int i = 0; i < nargs; ++i) {
      if (pos[i]) compileExprInto(pos[i], argBase + i);
      else emit(Op::LoadConst, argBase + i, addConst(kMissing));
    }
    int d = allocSlot();
    if (callee.upvals.empty()) {
      emit(Op::CallKnown, d, (int)ci, argBase, nargs);
    } else {
      // Fn has upvalues: must call through its closure binding.
      int fnv = loadName(c.callee->kind == ExprKind::Ident
                             ? static_cast<const IdentExpr*>(c.callee.get())->name
                             : "");
      emit(Op::Call, d, fnv, argBase, nargs);
    }
    return d;
  }

  if (c.callee->kind == ExprKind::Field) {
    const FieldExpr& f = static_cast<const FieldExpr&>(*c.callee);
    std::vector<const Expr*> pos;
    std::vector<std::pair<std::string, const Expr*>> kw;
    for (const CallArg& a : c.args) {
      if (a.name.empty()) pos.push_back(a.value.get());
      else kw.emplace_back(a.name, a.value.get());
    }
    int nkw = (int)kw.size();
    int base = allocTemps(1 + (int)pos.size() + nkw * 2);
    compileExprInto(f.object.get(), base);
    for (std::size_t i = 0; i < pos.size(); ++i)
      compileExprInto(pos[i], base + 1 + (int)i);
    for (int i = 0; i < nkw; ++i) {
      emit(Op::LoadConst, base + 1 + (int)pos.size() + i * 2,
           addConst(a_str_new(kw[i].first.data(), kw[i].first.size())));
      compileExprInto(kw[i].second,
                      base + 1 + (int)pos.size() + i * 2 + 1);
    }
    int d = allocSlot();
    emit(Op::MethodCall, d, base, addName(f.field), (int)pos.size(), nkw);
    return d;
  }

  int fn = compileExpr(c.callee.get());
  std::vector<const Expr*> pos;
  for (const CallArg& a : c.args) {
    if (!a.name.empty())
      bcUnsupported("named arguments in --vm mode");
    pos.push_back(a.value.get());
  }
  int nargs = (int)pos.size();
  int argBase = allocTemps(nargs);
  for (int i = 0; i < nargs; ++i) compileExprInto(pos[i], argBase + i);
  int d = allocSlot();
  emit(Op::Call, d, fn, argBase, nargs);
  return d;
}

int Compiler::emitCallBuiltin(const CallExpr& c, int id) {
  int nargs = (int)c.args.size();
  int argBase = allocTemps(nargs);
  for (int i = 0; i < nargs; ++i) compileExprInto(c.args[i].value.get(), argBase + i);
  int d = allocSlot();
  emit(Op::CallBuiltin, d, id, argBase, nargs);
  return d;
}

// ---------------------------------------------------------------------------
// Assignment targets
// ---------------------------------------------------------------------------
int Compiler::compileReadTarget(const Expr* t) {
  if (t->kind == ExprKind::Ident) return loadName(static_cast<const IdentExpr*>(t)->name);
  if (t->kind == ExprKind::Index) {
    const auto& x = static_cast<const IndexExpr&>(*t);
    int o = compileExpr(x.object.get());
    int i = compileExpr(x.index.get());
    int d = allocSlot();
    emit(Op::GetItem, d, o, i);
    return d;
  }
  if (t->kind == ExprKind::Field) {
    const auto& x = static_cast<const FieldExpr&>(*t);
    int o = compileExpr(x.object.get());
    int d = allocSlot();
    emit(Op::GetField, d, o, addName(x.field));
    return d;
  }
  bcUnsupported("unsupported assignment target");
}

void Compiler::compileStoreTarget(const Expr* t, int src) {
  if (t->kind == ExprKind::Ident) {
    storeName(static_cast<const IdentExpr*>(t)->name, src);
    return;
  }
  if (t->kind == ExprKind::Index) {
    const auto& x = static_cast<const IndexExpr&>(*t);
    int o = compileExpr(x.object.get());
    int i = compileExpr(x.index.get());
    emit(Op::SetItem, o, i, src);
    return;
  }
  if (t->kind == ExprKind::Field) {
    const auto& x = static_cast<const FieldExpr&>(*t);
    int o = compileExpr(x.object.get());
    emit(Op::SetField, o, addName(x.field), src);
    return;
  }
  bcUnsupported("unsupported assignment target");
}

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------
void Compiler::compileStmts(const std::vector<StmtPtr>& stmts) {
  for (const StmtPtr& s : stmts) {
    if (!s) continue;
    compileStmt(s.get());
  }
}

void Compiler::compileStmt(const Stmt* s) {
  if (!s) return;
  switch (s->kind) {
    case StmtKind::Let: {
      const auto& st = static_cast<const LetStmt&>(*s);
      int v;
      if (st.init) v = compileExpr(st.init.get());
      else { v = allocSlot(); emit(Op::LoadNone, v); }
      if (topLevel) {
        auto& m = g_bc.modules[code.moduleId];
        auto f = m.slotOf.find(st.name);
        if (f == m.slotOf.end())
          bcUnsupported("undeclared let at module scope");
        emit(Op::StoreGlobal, f->second, v);
      } else {
        scopes.back()[st.name] = v;
        if (boxed.count(v)) emit(Op::MakeCell, v);
      }
      return;
    }
    case StmtKind::Assign: {
      const auto& st = static_cast<const AssignStmt&>(*s);
      if (st.op == "=") {
        int v = compileExpr(st.value.get());
        compileStoreTarget(st.target.get(), v);
      } else {
        int cur = compileReadTarget(st.target.get());
        int rhs = compileExpr(st.value.get());
        int d = allocSlot();
        emit(binOpFor(st.binop, st.op.substr(0, st.op.size() - 1)), d, cur, rhs);
        compileStoreTarget(st.target.get(), d);
      }
      return;
    }
    case StmtKind::Expr: {
      const auto& st = static_cast<const ExprStmt&>(*s);
      std::string dummy;
      if (plainStringText(st.expr, dummy)) return;
      compileExpr(st.expr.get());
      return;
    }
    case StmtKind::Destructure: {
      const auto& st = static_cast<const DestructureStmt&>(*s);
      int v = compileExpr(st.value.get());
      int i = 0;
      for (const std::string& nm : st.names) {
        if (nm != "_") {
          int idx = allocSlot();
          emit(Op::LoadConst, idx, addConst(a_mkint(i)));
          int el = allocSlot();
          emit(Op::GetItem, el, v, idx);
          if (topLevel) {
            auto& m = g_bc.modules[code.moduleId];
            auto f = m.slotOf.find(nm);
            if (f == m.slotOf.end())
              bcUnsupported("undeclared destructure at module scope");
            emit(Op::StoreGlobal, f->second, el);
          } else {
            scopes.back()[nm] = el;
            if (boxed.count(el)) emit(Op::MakeCell, el);
          }
        }
        ++i;
      }
      return;
    }
    case StmtKind::Return: {
      emitRunDefersFrom(0);
      const auto& st = static_cast<const ReturnStmt&>(*s);
      int v = st.value ? compileExpr(st.value.get()) : -1;
      emit(Op::Return, v);
      return;
    }
    case StmtKind::Break: {
      if (breakPatches.empty()) bcUnsupported("'break' outside of a loop");
      emitRunDefersFrom(brkDepths.back());
      breakPatches.back().push_back(emit(Op::Jump, 0));
      return;
    }
    case StmtKind::Continue: {
      if (contPatches.empty()) bcUnsupported("'continue' outside of a loop");
      emitRunDefersFrom(contDepths.back());
      contPatches.back().push_back(emit(Op::Jump, 0));
      return;
    }
    case StmtKind::Defer: {
      const auto& st = static_cast<const DeferStmt&>(*s);
      if (deferSlots.back() < 0) {
        int ds = allocSlot();
        emit(Op::LoadNone, ds);
        deferSlots.back() = ds;
        code.deferSlots.push_back({ds, (int)scopes.size()});
      }
      uint32_t ci = compileAnonStmt(st.body.get(), this);
      int d = allocSlot();
      emit(Op::Closure, d, (int)ci, 0);
      emit(Op::PushDefer, deferSlots.back(), d);
      return;
    }
    case StmtKind::If: {
      const auto& st = static_cast<const IfStmt&>(*s);
      std::vector<int> endJumps;
      for (const auto& br : st.branches) {
        int cond = compileExpr(br.cond.get());
        int jf = emit(Op::JumpIfFalse, cond, 0);
        pushScope();
        compileStmts(br.body);
        emitRunDefersTop();
        popScope();
        endJumps.push_back(emit(Op::Jump, 0));
        code.ins[jf].b = (int)code.ins.size();
      }
      if (!st.elseBody.empty()) {
        pushScope();
        compileStmts(st.elseBody);
        emitRunDefersTop();
        popScope();
      }
      int end = (int)code.ins.size();
      for (int j : endJumps) code.ins[j].a = end;
      return;
    }
    case StmtKind::While: {
      const auto& st = static_cast<const WhileStmt&>(*s);
      int top = (int)code.ins.size();
      int cond = compileExpr(st.cond.get());
      int jf = emit(Op::JumpIfFalse, cond, 0);
      int bd = (int)deferSlots.size();
      pushScope();
      breakPatches.emplace_back();
      contPatches.emplace_back();
      brkDepths.push_back(bd);
      contDepths.push_back(bd);
      contTargets.push_back(top);
      compileStmts(st.body);
      emitRunDefersTop();
      int contTarget = contTargets.back();
      contTargets.pop_back();
      for (int at : contPatches.back()) code.ins[at].a = contTarget;
      contPatches.pop_back();
      for (int at : breakPatches.back()) code.ins[at].a = 0;  // patch later
      std::vector<int> brk = std::move(breakPatches.back());
      breakPatches.pop_back();
      brkDepths.pop_back();
      contDepths.pop_back();
      popScope();
      emit(Op::Jump, top);
      int end = (int)code.ins.size();
      code.ins[jf].b = end;
      for (int at : brk) code.ins[at].a = end;
      return;
    }
    case StmtKind::DoWhile: {
      const auto& st = static_cast<const DoWhileStmt&>(*s);
      int top = (int)code.ins.size();
      int bd = (int)deferSlots.size();
      pushScope();
      breakPatches.emplace_back();
      contPatches.emplace_back();
      brkDepths.push_back(bd);
      contDepths.push_back(bd);
      contTargets.push_back((int)code.ins.size());  // continue -> cond eval
      compileStmts(st.body);
      emitRunDefersTop();
      int condPos = (int)code.ins.size();
      contTargets.pop_back();
      // Continue should jump to condPos (just emitted position).
      for (int at : contPatches.back()) code.ins[at].a = condPos;
      contPatches.pop_back();
      int cond = compileExpr(st.cond.get());
      int jt = emit(Op::JumpIfTrue, cond, top);
      std::vector<int> brk = std::move(breakPatches.back());
      breakPatches.pop_back();
      brkDepths.pop_back();
      contDepths.pop_back();
      popScope();
      int end = (int)code.ins.size();
      for (int at : brk) code.ins[at].a = end;
      (void)jt;
      return;
    }
    case StmtKind::For: {
      const auto& st = static_cast<const ForStmt&>(*s);
      int isrc = compileExpr(st.iterable.get());
      int iter = allocSlot();
      emit(Op::IterInit, iter, isrc);
      int fbd = (int)deferSlots.size();
      pushScope();  // iter-var scope
      int varSlot = allocSlot();
      scopes.back()[st.var] = varSlot;
      bool varBoxed = boxed.count(varSlot);
      if (varBoxed) {
        emit(Op::LoadNone, varSlot);
        emit(Op::MakeCell, varSlot);
      }
      int iterTmp = varBoxed ? allocSlot() : varSlot;
      int top = (int)code.ins.size();
      int iterNext = emit(Op::IterNext, iterTmp, iter, 0);
      if (varBoxed) emit(Op::CellSet, varSlot, iterTmp);
      pushScope();  // body scope
      breakPatches.emplace_back();
      contPatches.emplace_back();
      brkDepths.push_back(fbd);
      contDepths.push_back(fbd + 1);
      contTargets.push_back(top);
      compileStmts(st.body);
      emitRunDefersTop();
      contTargets.pop_back();
      for (int at : contPatches.back()) code.ins[at].a = top;
      contPatches.pop_back();
      std::vector<int> brk = std::move(breakPatches.back());
      breakPatches.pop_back();
      brkDepths.pop_back();
      contDepths.pop_back();
      popScope();
      emit(Op::Jump, top);
      int end = (int)code.ins.size();
      code.ins[iterNext].c = end;
      for (int at : brk) code.ins[at].a = end;
      emitRunDefersTop();  // iter-var scope defers at loop exit
      popScope();
      return;
    }
    case StmtKind::Loop: {
      const auto& st = static_cast<const LoopStmt&>(*s);
      int top = (int)code.ins.size();
      int bd = (int)deferSlots.size();
      pushScope();
      breakPatches.emplace_back();
      contPatches.emplace_back();
      brkDepths.push_back(bd);
      contDepths.push_back(bd);
      contTargets.push_back(top);
      compileStmts(st.body);
      emitRunDefersTop();
      int contTarget = contTargets.back();
      contTargets.pop_back();
      for (int at : contPatches.back()) code.ins[at].a = contTarget;
      contPatches.pop_back();
      std::vector<int> brk = std::move(breakPatches.back());
      breakPatches.pop_back();
      brkDepths.pop_back();
      contDepths.pop_back();
      popScope();
      emit(Op::Jump, top);
      int end = (int)code.ins.size();
      for (int at : brk) code.ins[at].a = end;
      return;
    }
    case StmtKind::FnDecl: {
      const auto& st = static_cast<const FnDecl&>(*s);
      uint32_t ci = compileFnDecl(st, topLevel ? nullptr : this);
      if (topLevel) {
        auto& m = g_bc.modules[code.moduleId];
        auto f = m.slotOf.find(st.name);
        if (f == m.slotOf.end())
          bcUnsupported("undeclared fn at module scope");
        int tmp = allocSlot();
        emit(Op::Closure, tmp, (int)ci, 1);
        // Closure with c=1 stores into global slot a; use a=global idx.
        code.ins.back().a = f->second;
        code.ins.back().d = 0;
      } else {
        int d = allocSlot();
        emit(Op::Closure, d, (int)ci, 0);
        scopes.back()[st.name] = d;
        if (boxed.count(d)) emit(Op::MakeCell, d);
      }
      return;
    }
    case StmtKind::Import:
    case StmtKind::FromImport:
      // Handled by the module prologue (bindModuleImports equivalent).
      if (!topLevel) return;  // nested imports are no-ops (tree-walker parity)
      return;
    case StmtKind::StructDecl:
      bcUnsupported("struct declarations are not supported in --vm mode");
    case StmtKind::EnumDecl:
      bcUnsupported("enum declarations are not supported in --vm mode");
    case StmtKind::TraitDecl:
      bcUnsupported("trait declarations are not supported in --vm mode");
    case StmtKind::ImplDecl:
      bcUnsupported("impl declarations are not supported in --vm mode");
    case StmtKind::TypeAlias:
      bcUnsupported("type alias declarations are not supported in --vm mode");
  }
  bcUnsupported("unsupported statement kind");
}

// ---------------------------------------------------------------------------
// Module globals pre-scan + import binding
// ---------------------------------------------------------------------------
void prescanGlobals(const Module& mod) {
  BcModule& m = g_bc.modules[mod.id];
  auto declare = [&](const std::string& name) {
    if (m.slotOf.find(name) == m.slotOf.end()) {
      m.slotOf[name] = (int)m.vars.size();
      m.vars.push_back(A_NONE);
    }
  };
  for (const StmtPtr& s : mod.program->statements) {
    if (!s) continue;
    switch (s->kind) {
      case StmtKind::FnDecl:
        declare(static_cast<const FnDecl*>(s.get())->name);
        break;
      case StmtKind::Assign: {
        const auto& st = static_cast<const AssignStmt&>(*s);
        if (st.target->kind == ExprKind::Ident)
          declare(static_cast<const IdentExpr*>(st.target.get())->name);
        break;
      }
      case StmtKind::Destructure: {
        const auto& st = static_cast<const DestructureStmt&>(*s);
        for (const std::string& nm : st.names)
          if (nm != "_") declare(nm);
        break;
      }
      case StmtKind::Let:
        declare(static_cast<const LetStmt&>(*s).name);
        break;
      case StmtKind::Import: {
        const auto& im = static_cast<const ImportStmt&>(*s);
        if (im.pythonModuleImport) continue;  // unsupported later
        std::string alias = !im.alias.empty() ? im.alias
                            : !im.bareName.empty() ? im.bareName
                            : stemOf(im.path);
        declare(alias);
        break;
      }
      case StmtKind::FromImport: {
        const auto& im = static_cast<const FromImportStmt&>(*s);
        if (im.pythonModuleImport) continue;
        for (const auto& item : im.items) declare(item.alias);
        break;
      }
      default:
        break;
    }
  }
}

// Emit prologue ops binding imports, mirroring bindModuleImports.
void emitImportBinds(Compiler& c, const Module& mod) {
  if (!mod.program) return;
  int curMid = mod.id;
  for (const StmtPtr& s : mod.program->statements) {
    if (!s) continue;
    if (s->kind == StmtKind::Import) {
      const ImportStmt* im = static_cast<const ImportStmt*>(s.get());
      if (im->pythonModuleImport)
        bcUnsupported("python module imports are not supported in --vm mode");
      std::string alias = !im->alias.empty() ? im->alias
                          : !im->bareName.empty() ? im->bareName
                          : stemOf(im->path);
      int gslot = -1;
      auto& m = g_bc.modules[curMid];
      auto f = m.slotOf.find(alias);
      if (f != m.slotOf.end()) gslot = f->second;
      if (gslot < 0) bcUnsupported("import alias not pre-scanned");
      int mid = im->moduleId;
      if (im->nativeModule) {
        // A package with the same name shadows the native namespace.
        for (const Module& cand : c.graph.modules) {
          std::string pkg = stemOf(cand.path);
          if (pkg == "__init__" || pkg.empty()) {
            auto slash = cand.path.find_last_of('/');
            if (slash != std::string::npos) {
              auto slash2 = cand.path.find_last_of('/', slash - 1);
              pkg = stemOf(cand.path.substr(
                  slash2 == std::string::npos ? 0 : slash2 + 1,
                  slash - (slash2 == std::string::npos ? 0 : slash2 + 1)));
            }
          }
          if (cand.name == im->bareName || stemOf(cand.path) == im->bareName ||
              pkg == im->bareName) {
            mid = cand.id;
            break;
          }
        }
      }
      if (mid >= 0) {
        c.emit(Op::BindModule, gslot, c.addName(alias), mid, -1);
      } else if (im->nativeModule) {
        c.emit(Op::BindModule, gslot, c.addName(alias), -1,
               c.addName(im->bareName));
      } else {
        bcUnsupported("cannot resolve import '" + alias + "'");
      }
    } else if (s->kind == StmtKind::FromImport) {
      const FromImportStmt* im = static_cast<const FromImportStmt*>(s.get());
      if (im->pythonModuleImport)
        bcUnsupported("python module imports are not supported in --vm mode");
      auto& m = g_bc.modules[curMid];
      if (im->moduleId >= 0) {
        for (const auto& item : im->items) {
          auto f = m.slotOf.find(item.alias);
          if (f == m.slotOf.end())
            bcUnsupported("from-import alias not pre-scanned");
          c.emit(Op::ImportMember, f->second, im->moduleId,
                 c.addName(item.name));
        }
      } else if (im->nativeModule) {
        for (const auto& item : im->items) {
          auto f = m.slotOf.find(item.alias);
          if (f == m.slotOf.end()) continue;
          if (const std::string* bn = nativeFunction(im->bareName, item.name)) {
            int bid = builtinRuntimeId(*bn);
            if (bid < 0) continue;
            int tmp = c.allocSlot();
            c.emit(Op::LoadBuiltin, tmp, bid);
            c.emit(Op::StoreGlobal, f->second, tmp);
          } else if (const std::string* cn =
                         nativeConstant(im->bareName, item.name)) {
            double v = *cn == "PI"   ? 3.14159265358979323846
                      : *cn == "E"    ? 2.71828182845904523536
                                      : 0.0;
            int tmp = c.allocSlot();
            c.emit(Op::LoadConst, tmp, c.addConst(a_double_canon(v)));
            c.emit(Op::StoreGlobal, f->second, tmp);
          } else {
            int bid = builtinRuntimeId(item.name);
            if (bid >= 0) {
              int tmp = c.allocSlot();
              c.emit(Op::LoadBuiltin, tmp, bid);
              c.emit(Op::StoreGlobal, f->second, tmp);
            }
          }
        }
      }
    }
  }
}

// Compile one module: prologue (import binds) + all FnDecl bodies are
// compiled lazily via compileFnDecl during the statement walk; the main
// code holds the top-level statement list.
void compileModule(int mid, const ModuleGraph& graph, const SemaResult& sema) {
  const Module& mod = graph.at(mid);
  if (!mod.program) return;
  Code main;
  main.moduleId = mid;
  main.isMain = true;
  Compiler c(graph, sema, main);
  c.topLevel = true;
  c.pushScope();
  emitImportBinds(c, mod);
  {
    Code scratch;
    scratch.moduleId = mid;
    Compiler d(graph, sema, scratch);
    d.topLevel = true;
    d.pushScope();
    emitImportBinds(d, mod);
    for (const StmtPtr& s : mod.program->statements) {
      if (s && s->kind == StmtKind::FnDecl)
        d.compileFnDecl(*static_cast<const FnDecl*>(s.get()), nullptr);
    }
    d.compileStmts(mod.program->statements);
    c.boxed = d.pendingCaptures;
  }
  // Compile top-level statements; FnDecl stmts register their code eagerly
  // so recursion/forward references resolve (global slots pre-scanned).
  // First pass: compile every FnDecl body so declCode is complete before any
  // call site needs it.
  for (const StmtPtr& s : mod.program->statements) {
    if (s && s->kind == StmtKind::FnDecl)
      c.compileFnDecl(*static_cast<const FnDecl*>(s.get()), nullptr);
  }
  c.compileStmts(mod.program->statements);
  c.emitRunDefersFrom(0);
  int ns = c.allocSlot();
  c.emit(Op::LoadNone, ns);
  c.emit(Op::Return, ns);
  main.frameSize = c.maxSlot;
  g_bc.codes.push_back(std::move(main));
  if ((int)g_bc.moduleMain.size() <= mid) g_bc.moduleMain.resize(mid + 1, -1);
  g_bc.moduleMain[mid] = (int)g_bc.codes.size() - 1;
}

}  // namespace

bool compileAll(const ModuleGraph& graph, const SemaResult& sema) {
  g_bc = State{};
  g_bc.modules.resize(graph.modules.size());
  g_bc.moduleMain.assign(graph.modules.size(), -1);
  try {
    for (const Module& mod : graph.modules) {
      // Mirror compileModule: the auto-loaded prelude stays an empty module
      // for non-entry programs, so it must not get pre-scanned globals.
      if (mod.id != graph.entry && mod.name == "prelude") continue;
      prescanGlobals(mod);
    }
    std::vector<int> order = graph.order;
    if (order.empty() && graph.entry >= 0) order.push_back(graph.entry);
    for (int mid : order) {
      if (mid < 0 || mid >= (int)graph.modules.size()) continue;
      const Module& mod = graph.at(mid);
      if (!mod.program) continue;
      if (mod.id != graph.entry && mod.name == "prelude") continue;
      compileModule(mid, graph, sema);
    }
    return true;
  } catch (BcUnsupported&) {
    g_bc = State{};
    return false;
  }
}

}  // namespace a::vm::bc
