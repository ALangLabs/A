// C99 emitter for the native subset, consumed by the TinyCC JIT (jit.cpp) and
// by `a build` via gcc. Every A value is an `aval` — a tagged union:
//
//   typedef struct { a_i64 i; double d; int f; } aval;
//
// `f` says which member is live, so values of unknown type (function params,
// call results, mixed arithmetic) carry their type with them and dispatch at
// runtime through the ai()/ad() helpers. Statically-known locals and globals
// still take the direct .i/.d fast path via the slot/global type tables.

#include "backend/kgen/codegen/llvm_emitter.hpp"

#include <cctype>
#include <cstdio>
#include <ostream>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace a {
namespace {

bool isStdlibModuleC(const ModuleGraph& graph, const Module& mod) {
  if (mod.id == graph.entry) return false;
  return mod.path.find("stdlib") != std::string::npos;
}

std::string extractStringC(const StringLitExpr& s) {
  std::string result;
  for (const auto& p : s.parts) {
    if (!p.isExpr) result += p.text;
  }
  return result;
}

enum class VT : char { Int = 0, Float = 1, Unknown = 2 };

class CEmitter {
 public:
  CEmitter(const ModuleGraph& graph, const SemaResult& sema, std::ostream& out)
      : graph_(graph), sema_(sema), out_(out) {}

  bool emit() {
    collectGlobals();
    pretypeGlobals();
    collectStrings();

    out_ << "typedef long long a_i64;\n";
    out_ << "typedef struct { a_i64 i; double d; int f; } aval;\n";
    out_ << "static a_i64 ai(aval v) { return v.f ? (a_i64)v.d : v.i; }\n";
    out_ << "static double ad(aval v) { return v.f ? v.d : (double)v.i; }\n";
    out_ << "int printf(const char *, ...);\n";
    out_ << "void *malloc(unsigned long);\n";
    out_ << "void free(void *);\n";
    out_ << "void *memcpy(void *, const void *, unsigned long);\n";
    out_ << "void exit(int);\n";
    out_ << "typedef unsigned long size_t;\n";
    out_ << "int snprintf(char *, unsigned long, const char *, ...);\n";
    out_ << "double strtod(const char *, char **);\n";
    out_ << "char *strchr(const char *, int);\n";
    out_ << "char *strcat(char *, const char *);\n";
    // Python-style float repr: shortest %g precision that round-trips, with a
    // trailing ".0" when the result would otherwise look like an int.
    out_ << "static void a_print_double(double v) {\n"
            "  char buf[64]; int p;\n"
            "  for (p = 1; p <= 17; ++p) {\n"
            "    snprintf(buf, sizeof buf, \"%.*g\", p, v);\n"
            "    if (strtod(buf, 0) == v) break;\n"
            "  }\n"
            "  if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E') &&\n"
            "      !strchr(buf, 'n') && !strchr(buf, 'i') && !strchr(buf, 'N') &&\n"
            "      !strchr(buf, 'I')) strcat(buf, \".0\");\n"
            "  printf(\"%s\\n\", buf);\n"
            "}\n";
    // Heap objects: aval.f == 2 means .i holds an a_obj*. type 1 = string
    // (data is char*, len bytes), type 2 = list (data is aval*, len elems).
    out_ << "typedef struct a_obj { int type; a_i64 len; a_i64 cap; void *data; } a_obj;\n";
    out_ << "static a_obj *a_new_obj(int t, a_i64 len, a_i64 cap, void *d) {\n"
            "  a_obj *o = (a_obj *)malloc(sizeof(a_obj));\n"
            "  if (!o) { printf(\"out of memory\\n\"); exit(1); }\n"
            "  o->type = t; o->len = len; o->cap = cap; o->data = d;\n"
            "  return o;\n"
            "}\n"
            "static aval a_strof(aval v);\n"
            "static aval a_mul(aval l, aval r) {\n"
            "  int ls = (l.f == 2 && ((a_obj *)l.i)->type == 1);\n"
            "  int rs = (r.f == 2 && ((a_obj *)r.i)->type == 1);\n"
            "  if (ls || rs) {\n"
            "    aval S = ls ? l : r;\n"
            "    aval N = ls ? r : l;\n"
            "    a_obj *so = (a_obj *)S.i;\n"
            "    a_i64 cnt = ai(N);\n"
            "    if (cnt < 0) cnt = 0;\n"
            "    a_i64 n = so->len * cnt;\n"
            "    char *b = (char *)malloc((size_t)n + 1);\n"
            "    if (!b) { printf(\"out of memory\\n\"); exit(1); }\n"
            "    for (a_i64 k = 0; k < cnt; ++k)\n"
            "      memcpy(b + k * so->len, so->data, (size_t)so->len);\n"
            "    b[n] = 0;\n"
            "    aval v; v.f = 2; v.i = (a_i64)a_new_obj(1, n, 0, b); v.d = 0;\n"
            "    return v;\n"
            "  }\n"
            "  if (l.f == 1 || r.f == 1) {\n"
            "    aval v; v.f = 1; v.d = ad(l) * ad(r); v.i = 0;\n"
            "    return v;\n"
            "  }\n"
            "  aval v; v.i = ai(l) * ai(r); v.d = 0; v.f = 0;\n"
            "  return v;\n"
            "}\n"
            "static aval a_cat(aval l, aval r) {\n"
            "  int ls = (l.f == 2 && ((a_obj *)l.i)->type == 1);\n"
            "  int rs = (r.f == 2 && ((a_obj *)r.i)->type == 1);\n"
            "  if (ls || rs) {\n"
            "    aval L = ls ? l : a_strof(l);\n"
            "    aval R = rs ? r : a_strof(r);\n"
            "    a_obj *lo = (a_obj *)L.i, *ro = (a_obj *)R.i;\n"
            "    char *b = (char *)malloc((size_t)(lo->len + ro->len + 1));\n"
            "    if (!b) { printf(\"out of memory\\n\"); exit(1); }\n"
            "    memcpy(b, lo->data, (size_t)lo->len);\n"
            "    memcpy(b + lo->len, ro->data, (size_t)ro->len);\n"
            "    b[lo->len + ro->len] = 0;\n"
            "    aval v; v.f = 2; v.i = (a_i64)a_new_obj(1, lo->len + ro->len, 0, b); v.d = 0;\n"
            "    return v;\n"
            "  }\n"
            "  if (l.f == 1 || r.f == 1) {\n"
            "    aval v; v.f = 1; v.d = ad(l) + ad(r); v.i = 0;\n"
            "    return v;\n"
            "  }\n"
            "  aval v; v.i = ai(l) + ai(r); v.d = 0; v.f = 0;\n"
            "  return v;\n"
            "}\n"
            "static aval a_lenof(aval v) {\n"
            "  aval r; r.f = 0; r.d = 0;\n"
            "  if (v.f == 2) r.i = ((a_obj *)v.i)->len; else r.i = 0;\n"
            "  return r;\n"
            "}\n"
            "static aval a_strof(aval v) {\n"
            "  char buf[64];\n"
            "  if (v.f == 2) {\n"
            "    a_obj *o = (a_obj *)v.i;\n"
            "    char *b = (char *)malloc((size_t)o->len + 1);\n"
            "    memcpy(b, o->data, (size_t)o->len); b[o->len] = 0;\n"
            "    aval r; r.f = 2; r.i = (a_i64)a_new_obj(1, o->len, 0, b); r.d = 0;\n"
            "    return r;\n"
            "  }\n"
            "  if (v.f) snprintf(buf, sizeof buf, \"%.*g\", 17, v.d);\n"
            "  else snprintf(buf, sizeof buf, \"%lld\", v.i);\n"
            "  a_i64 n = 0; while (buf[n]) ++n;\n"
            "  char *b = (char *)malloc((size_t)n + 1);\n"
            "  memcpy(b, buf, (size_t)n + 1);\n"
            "  aval r; r.f = 2; r.i = (a_i64)a_new_obj(1, n, 0, b); r.d = 0;\n"
            "  return r;\n"
            "}\n";
    out_ << "double sqrt(double);\n";
    out_ << "double pow(double, double);\n";
    out_ << "double fmod(double, double);\n";
    static const char* kMath1[] = {
        "floor", "ceil", "round", "exp", "log", "sin", "cos", "tan", "fabs"};
    for (const char* n : kMath1) out_ << "double " << n << "(double);\n";
    out_ << "\n";

    for (const auto& n : globalNames_) out_ << "static aval g_" << n << " = {0};\n";
    if (!globalNames_.empty()) out_ << "\n";

    // Static string objects for literals (aval.f == 2 -> a_obj*).
    for (std::size_t si = 0; si < stringLits_.size(); ++si) {
      const std::string& lit = stringLits_[si];
      out_ << "static char a_sd_" << si << "[] = \"" << escapeC(lit) << "\";\n";
      out_ << "static a_obj a_so_" << si << " = {1, " << lit.size() << ", "
           << lit.size() << ", a_sd_" << si << "};\n";
    }
    if (!stringLits_.empty()) out_ << "\n";

    std::vector<const FnDecl*> fns;
    for (std::size_t i = 0; i < graph_.modules.size(); ++i) {
      const auto& mod = graph_.modules[i];
      if (!mod.program || isStdlibModuleC(graph_, mod)) continue;
      for (const auto& stmt : mod.program->statements) {
        if (stmt->kind == StmtKind::FnDecl)
          fns.push_back(static_cast<const FnDecl*>(stmt.get()));
      }
    }
    for (const FnDecl* fn : fns) {
      out_ << "aval afn_" << cIdent(fn->name) << "(";
      for (std::size_t i = 0; i < fn->params.size(); ++i) {
        if (i) out_ << ", ";
        out_ << "aval v_" << cIdent(fn->params[i].name);
      }
      out_ << ");\n";
    }
    if (!fns.empty()) out_ << "\n";
    for (const FnDecl* fn : fns) emitFunction(*fn);
    emitMain();
    return true;
  }

 private:
  const ModuleGraph& graph_;
  const SemaResult& sema_;
  std::ostream& out_;
  bool inMain_ = false;
  int tmpTop_ = 0;
  std::set<std::string> globalNames_;
  std::unordered_map<std::string, VT> globalType_;
  std::set<std::string> locals_;
  std::unordered_map<std::string, VT> slotType_;
  std::vector<std::string> stringLits_;
  std::unordered_map<std::string, int> stringLitIdx_;

  static std::string cIdent(const std::string& name) {
    std::string r;
    for (char c : name) {
      r += (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
    }
    return r.empty() ? "_" : r;
  }

  void collectGlobals() {
    if (graph_.entry < 0 ||
        static_cast<std::size_t>(graph_.entry) >= graph_.modules.size()) return;
    const auto& root = graph_.modules[static_cast<std::size_t>(graph_.entry)];
    if (!root.program) return;
    for (const auto& stmt : root.program->statements) {
      std::string name;
      if (stmt->kind == StmtKind::Let) {
        name = static_cast<const LetStmt&>(*stmt).name;
      } else if (stmt->kind == StmtKind::Assign &&
                 static_cast<const AssignStmt&>(*stmt).target->kind == ExprKind::Ident) {
        name = static_cast<const IdentExpr&>(*static_cast<const AssignStmt&>(*stmt).target).name;
      }
      if (!name.empty()) globalNames_.insert(cIdent(name));
    }
  }

  // Static type of an expression: Int, Float, or Unknown (runtime tag decides).
  VT exprType(const Expr& e) {
    switch (e.kind) {
      case ExprKind::IntLit:
      case ExprKind::BoolLit:
      case ExprKind::Range:
        return VT::Int;
      case ExprKind::FloatLit:
        return VT::Float;
      case ExprKind::Ident: {
        std::string n = cIdent(static_cast<const IdentExpr&>(e).name);
        auto si = slotType_.find(n);
        if (si != slotType_.end()) return si->second;
        auto gi = globalType_.find(n);
        if (gi != globalType_.end()) return gi->second;
        return VT::Unknown;
      }
      case ExprKind::Binary: {
        const auto& b = static_cast<const BinaryExpr&>(e);
        static const std::set<std::string> cmpOps = {"==","!=","<","<=",">",">="};
        static const std::set<std::string> bitOps = {"&","|","^","<<",">>","and","or"};
        if (cmpOps.count(b.op) || bitOps.count(b.op)) return VT::Int;
        static const std::set<std::string> arith = {"+","-","*","/","%"};
        if (!arith.count(b.op)) return VT::Int;
        VT l = exprType(*b.lhs), r = exprType(*b.rhs);
        if (l == VT::Float || r == VT::Float) return VT::Float;
        if (l == VT::Unknown || r == VT::Unknown) return VT::Unknown;
        return VT::Int;
      }
      case ExprKind::Unary: {
        const auto& u = static_cast<const UnaryExpr&>(e);
        if (u.op == "-") return exprType(*u.operand);
        return VT::Int;
      }
      case ExprKind::Call: {
        const auto& c = static_cast<const CallExpr&>(e);
        if (c.callee->kind != ExprKind::Ident) return VT::Unknown;
        std::string n = cIdent(static_cast<const IdentExpr&>(*c.callee).name);
        static const std::set<std::string> intFns = {"print", "int"};
        if (intFns.count(n)) return VT::Int;
        static const std::set<std::string> floatFns = {
            "sqrt","pow","fmod","floor","ceil","round","exp","log","sin","cos","tan",
            "fabs","f64","float"};
        if (floatFns.count(n)) return VT::Float;
        return VT::Unknown;  // user function: tag carried in the return value
      }
      default:
        return VT::Unknown;
    }
  }

  // Pre-type globals so functions (emitted above main) know whether a global
  // holds a double. Walks top-level statements in order.
  void pretypeGlobals() {
    if (graph_.entry < 0 ||
        static_cast<std::size_t>(graph_.entry) >= graph_.modules.size()) return;
    const auto& root = graph_.modules[static_cast<std::size_t>(graph_.entry)];
    if (!root.program) return;
    for (const auto& stmt : root.program->statements) {
      if (stmt->kind == StmtKind::Let) {
        const auto& s = static_cast<const LetStmt&>(*stmt);
        if (s.init && globalNames_.count(cIdent(s.name)))
          globalType_[cIdent(s.name)] = exprType(*s.init);
      } else if (stmt->kind == StmtKind::Assign &&
                 static_cast<const AssignStmt&>(*stmt).target->kind == ExprKind::Ident) {
        const auto& s = static_cast<const AssignStmt&>(*stmt);
        std::string n = cIdent(static_cast<const IdentExpr&>(*s.target).name);
        if (!globalNames_.count(n)) continue;
        VT vt = exprType(*s.value);
        if (s.op == "=") globalType_[n] = vt;
        else if (vt == VT::Float || globalType_[n] == VT::Float ||
                 vt == VT::Unknown || globalType_[n] == VT::Unknown)
          globalType_[n] = (vt == VT::Int && globalType_[n] == VT::Int) ? VT::Int
                                                                       : (vt == VT::Float || globalType_[n] == VT::Float ? VT::Float : VT::Unknown);
      }
    }
  }

  void collectLocalNames(const std::vector<StmtPtr>& body) {
    for (const auto& sp : body) {
      const Stmt& s = *sp;
      switch (s.kind) {
        case StmtKind::Let: {
          std::string n = cIdent(static_cast<const LetStmt&>(s).name);
          if (!inMain_ || !globalNames_.count(n)) locals_.insert(n);
          break;
        }
        case StmtKind::Assign: {
          const auto& a = static_cast<const AssignStmt&>(s);
          if (a.target->kind == ExprKind::Ident) {
            std::string n = cIdent(static_cast<const IdentExpr&>(*a.target).name);
            if (!globalNames_.count(n)) locals_.insert(n);
          }
          break;
        }
        case StmtKind::For: {
          const auto& f = static_cast<const ForStmt&>(s);
          std::string v = cIdent(f.var);
          locals_.insert(v);
          locals_.insert("__iter_" + v);
          locals_.insert("__end_" + v);
          collectLocalNames(f.body);
          break;
        }
        case StmtKind::If: {
          const auto& i = static_cast<const IfStmt&>(s);
          for (const auto& br : i.branches) collectLocalNames(br.body);
          collectLocalNames(i.elseBody);
          break;
        }
        case StmtKind::While:
          collectLocalNames(static_cast<const WhileStmt&>(s).body);
          break;
        default:
          break;
      }
    }
  }

  void collectExprStrings(const Expr& e) {
    switch (e.kind) {
      case ExprKind::StringLit: {
        const auto& s = static_cast<const StringLitExpr&>(e);
        std::string lit;
        for (const auto& p : s.parts) if (!p.isExpr) lit += p.text;
        if (!stringLitIdx_.count(lit)) {
          stringLitIdx_[lit] = static_cast<int>(stringLits_.size());
          stringLits_.push_back(lit);
        }
        break;
      }
      case ExprKind::Binary: {
        const auto& b = static_cast<const BinaryExpr&>(e);
        collectExprStrings(*b.lhs);
        collectExprStrings(*b.rhs);
        break;
      }
      case ExprKind::Unary:
        collectExprStrings(*static_cast<const UnaryExpr&>(e).operand);
        break;
      case ExprKind::Range: {
        const auto& r = static_cast<const RangeExpr&>(e);
        collectExprStrings(*r.start);
        collectExprStrings(*r.end);
        if (r.step) collectExprStrings(*r.step);
        break;
      }
      case ExprKind::Call: {
        const auto& c = static_cast<const CallExpr&>(e);
        for (const auto& a : c.args) collectExprStrings(*a.value);
        break;
      }
      default:
        break;
    }
  }

  void collectStmtStrings(const Stmt& s) {
    switch (s.kind) {
      case StmtKind::Let: {
        const auto& l = static_cast<const LetStmt&>(s);
        if (l.init) collectExprStrings(*l.init);
        break;
      }
      case StmtKind::Assign:
        collectExprStrings(*static_cast<const AssignStmt&>(s).value);
        break;
      case StmtKind::Expr:
        collectExprStrings(*static_cast<const ExprStmt&>(s).expr);
        break;
      case StmtKind::Return: {
        const auto& r = static_cast<const ReturnStmt&>(s);
        if (r.value) collectExprStrings(*r.value);
        break;
      }
      case StmtKind::If: {
        const auto& i = static_cast<const IfStmt&>(s);
        for (const auto& br : i.branches) {
          collectExprStrings(*br.cond);
          for (const auto& b : br.body) collectStmtStrings(*b);
        }
        for (const auto& b : i.elseBody) collectStmtStrings(*b);
        break;
      }
      case StmtKind::While: {
        const auto& w = static_cast<const WhileStmt&>(s);
        collectExprStrings(*w.cond);
        for (const auto& b : w.body) collectStmtStrings(*b);
        break;
      }
      case StmtKind::For: {
        const auto& f = static_cast<const ForStmt&>(s);
        collectExprStrings(*f.iterable);
        for (const auto& b : f.body) collectStmtStrings(*b);
        break;
      }
      default:
        break;
    }
  }

  void collectStrings() {
    for (const auto& mod : graph_.modules) {
      if (!mod.program || isStdlibModuleC(graph_, mod)) continue;
      for (const auto& stmt : mod.program->statements) {
        if (stmt->kind == StmtKind::FnDecl) {
          for (const auto& b : static_cast<const FnDecl&>(*stmt).body)
            collectStmtStrings(*b);
        } else {
          collectStmtStrings(*stmt);
        }
      }
    }
  }

  std::string lvalue(const std::string& rawName) {
    std::string n = cIdent(rawName);
    if (locals_.count(n)) return "v_" + n;
    if (globalNames_.count(n)) return "g_" + n;
    return "v_" + n;
  }

  std::string identA(const std::string& rawName) {
    std::string n = cIdent(rawName);
    if (locals_.count(n)) return "v_" + n;
    if (globalNames_.count(n)) return "g_" + n;
    return "(aval){.i=0,.f=0}";
  }
  bool isGlobalLV(const std::string& lv) { return lv.rfind("g_", 0) == 0; }

  std::string floatLit(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    std::string s = buf;
    if (s.find_first_of(".eE") == std::string::npos) s += ".0";
    return s;
  }

  std::string escapeC(const std::string& s) {
    std::string r;
    for (char c : s) {
      switch (c) {
        case '\n': r += "\\n"; break;
        case '\t': r += "\\t"; break;
        case '\r': r += "\\r"; break;
        case '"': r += "\\\""; break;
        case '\\': r += "\\\\"; break;
        default: r += c; break;
      }
    }
    return r;
  }

  // Fresh temp name; the aval _tN slots are declared at each function top.
  std::string tmp() { return "_t" + std::to_string(tmpTop_++); }

  // aval result of `l <op> r` where l/r are aval expressions and vt is the
  // statically-known result requirement (Int/Float emit direct ops; Unknown
  // emits a runtime tag dispatch into a temp).
  std::string avalBin(const std::string& op, const std::string& l,
                      const std::string& r, VT want) {
    if (want == VT::Int) {
      static const std::set<std::string> cmp = {"==","!=","<","<=",">",">="};
      if (cmp.count(op)) {
        std::string cop = op;
        return "(aval){.i=(" + std::string("ai(") + l + ") " + cop + " ai(" + r + ")), .f=0}";
      }
      static const std::set<std::string> bit = {"&","|","^","<<",">>","and","or"};
      std::string bop;
      if (op == "&" || op == "and") bop = "&";
      else if (op == "|" || op == "or") bop = "|";
      else if (op == "^") bop = "^";
      else if (op == "<<") bop = "<<";
      else if (op == ">>") bop = ">>";
      else bop = "+";
      if (op == "+" || op == "-") bop = op;
      else if (op == "*") bop = "*";
      if (bit.count(op)) {
        if (op == "<<") return "(aval){.i=(ai(" + l + ") << (ai(" + r + ") & 63)), .f=0}";
        if (op == ">>") return "(aval){.i=(ai(" + l + ") >> (ai(" + r + ") & 63)), .f=0}";
        return "(aval){.i=(ai(" + l + ") " + bop + " ai(" + r + ")), .f=0}";
      }
      // arith int
      if (op == "+") return "(aval){.i=(ai(" + l + ") + ai(" + r + ")), .f=0}";
      if (op == "-") return "(aval){.i=(ai(" + l + ") - ai(" + r + ")), .f=0}";
      if (op == "*") return "(aval){.i=(ai(" + l + ") * ai(" + r + ")), .f=0}";
      return "(aval){.i=0,.f=0}";
    }
    if (want == VT::Float) {
      if (op == "+") return "(aval){.d=(ad(" + l + ") + ad(" + r + ")), .f=1}";
      if (op == "-") return "(aval){.d=(ad(" + l + ") - ad(" + r + ")), .f=1}";
      if (op == "*") return "(aval){.d=(ad(" + l + ") * ad(" + r + ")), .f=1}";
      if (op == "/") return "(aval){.d=(ad(" + l + ") / ad(" + r + ")), .f=1}";
      if (op == "%") return "(aval){.d=fmod(ad(" + l + "), ad(" + r + ")), .f=1}";
      return "(aval){.d=0.0,.f=1}";
    }
    // Unknown: runtime dispatch on the tags.
    static const std::set<std::string> cmp = {"==","!=","<","<=",">",">="};
    static const std::set<std::string> bit = {"&","|","^","<<",">>","and","or"};
    std::string tn = tmp();
    if (cmp.count(op)) {
      out_ << "  if ((" << l << ").f || (" << r << ").f) " << tn
           << ".i = (ad(" << l << ") " << op << " ad(" << r << ")); else " << tn
           << ".i = (ai(" << l << ") " << op << " ai(" << r << "));\n";
      out_ << "  " << tn << ".f = 0;\n";
      return tn;
    }
    if (bit.count(op)) {
      // Bitwise ops are int-valued regardless of operand tags.
      std::string bop;
      if (op == "&" || op == "and") bop = "&";
      else if (op == "|" || op == "or") bop = "|";
      else if (op == "^") bop = "^";
      else if (op == "<<") bop = "SHL";
      else bop = "SHR";
      if (bop == "SHL") out_ << "  " << tn << ".i = ai(" << l << ") << (ai(" << r << ") & 63);\n";
      else if (bop == "SHR") out_ << "  " << tn << ".i = ai(" << l << ") >> (ai(" << r << ") & 63);\n";
      else out_ << "  " << tn << ".i = ai(" << l << ") " << bop << " ai(" << r << ");\n";
      out_ << "  " << tn << ".f = 0;\n";
      return tn;
    }
    // "+" and "*" dispatch through helpers that also handle string
    // concat/repeat; the rest use the numeric tag dispatch.
    if (op == "+" || op == "*") {
      std::string tn = tmp();
      out_ << "  " << tn << " = a_" << (op == "+" ? "cat" : "mul") << "(" << l << ", " << r << ");\n";
      return tn;
    }
    std::string iop, dop;
    if (op == "+") { iop = "+"; dop = "+"; }
    else if (op == "-") { iop = "-"; dop = "-"; }
    else if (op == "*") { iop = "*"; dop = "*"; }
    else if (op == "/") { iop = "/"; dop = "/"; }
    else if (op == "%") { iop = "%"; dop = "FMOD"; }
    else { iop = "+"; dop = "+"; }
    out_ << "  if ((" << l << ").f || (" << r << ").f) {\n";
    if (dop == "FMOD")
      out_ << "    " << tn << ".d = fmod(ad(" << l << "), ad(" << r << "));\n";
    else
      out_ << "    " << tn << ".d = ad(" << l << ") " << dop << " ad(" << r << ");\n";
    out_ << "    " << tn << ".f = 1;\n  } else {\n";
    out_ << "    " << tn << ".i = ai(" << l << ") " << iop << " ai(" << r << ");\n";
    out_ << "    " << tn << ".f = 0;\n  }\n";
    return tn;
  }

  std::string evalA(const Expr& e) {
    switch (e.kind) {
      case ExprKind::IntLit:
        return "(aval){.i=" + std::to_string(static_cast<const IntLitExpr&>(e).value) + ",.f=0}";
      case ExprKind::BoolLit:
        return std::string("(aval){.i=") + (static_cast<const BoolLitExpr&>(e).value ? "1" : "0") + ",.f=0}";
      case ExprKind::FloatLit:
        return "(aval){.d=" + floatLit(static_cast<const FloatLitExpr&>(e).value) + ",.f=1}";
      case ExprKind::Ident:
        return identA(static_cast<const IdentExpr&>(e).name);
      case ExprKind::StringLit: {
        const auto& s = static_cast<const StringLitExpr&>(e);
        std::string lit;
        for (const auto& p : s.parts) if (!p.isExpr) lit += p.text;
        auto it = stringLitIdx_.find(lit);
        if (it == stringLitIdx_.end()) return "(aval){.i=0,.f=2}";
        return "(aval){.i=(a_i64)&a_so_" + std::to_string(it->second) + ",.f=2}";
      }
      case ExprKind::Binary: {
        const auto& b = static_cast<const BinaryExpr&>(e);
        std::string l = evalA(*b.lhs);
        std::string r = evalA(*b.rhs);
        return avalBin(b.op, l, r, exprType(e));
      }
      case ExprKind::Unary: {
        const auto& u = static_cast<const UnaryExpr&>(e);
        std::string o = evalA(*u.operand);
        VT t = exprType(e);
        if (u.op == "-") {
          if (t == VT::Float) return "(aval){.d=(-ad(" + o + ")), .f=1}";
          if (t == VT::Int) return "(aval){.i=(-ai(" + o + ")), .f=0}";
          std::string tn = tmp();
          out_ << "  " << tn << " = " << o << ";\n";
          out_ << "  if (" << tn << ".f) " << tn << ".d = -" << tn << ".d; else " << tn << ".i = -" << tn << ".i;\n";
          return tn;
        }
        if (u.op == "~")
          return "(aval){.i=(~ai(" + o + ")), .f=0}";
        if (u.op == "not" || u.op == "!")
          return "(aval){.i=(!ai(" + o + ")), .f=0}";
        return "(aval){.i=0,.f=0}";
      }
      case ExprKind::Call: {
        const auto& c = static_cast<const CallExpr&>(e);
        if (c.callee->kind != ExprKind::Ident) return "(aval){.i=0,.f=0}";
        std::string name = cIdent(static_cast<const IdentExpr&>(*c.callee).name);
        if (name == "print" && c.args.size() == 1) {
          emitPrint(*c.args[0].value);
          return "(aval){.i=0,.f=0}";
        }
        if (name == "int" && !c.args.empty()) {
          return "(aval){.i=ai(" + evalA(*c.args[0].value) + "),.f=0}";
        }
        if (name == "len" && !c.args.empty()) {
          std::string tn = tmp();
          out_ << "  " << tn << " = a_lenof(" << evalA(*c.args[0].value) << ");\n";
          return tn;
        }
        if (name == "str" && !c.args.empty()) {
          std::string tn = tmp();
          out_ << "  " << tn << " = a_strof(" << evalA(*c.args[0].value) << ");\n";
          return tn;
        }
        if ((name == "f64" || name == "float") && !c.args.empty()) {
          return "(aval){.d=ad(" + evalA(*c.args[0].value) + "),.f=1}";
        }
        static const std::set<std::string> libm1 = {
            "floor","ceil","round","exp","log","sin","cos","tan","fabs"};
        if (name == "sqrt" && !c.args.empty()) {
          return "(aval){.d=sqrt(ad(" + evalA(*c.args[0].value) + ")), .f=1}";
        }
        if ((name == "pow" || name == "fmod") && c.args.size() >= 2) {
          return "(aval){.d=" + name + "(ad(" + evalA(*c.args[0].value) + "), ad(" +
                 evalA(*c.args[1].value) + ")), .f=1}";
        }
        if (libm1.count(name) && !c.args.empty()) {
          return "(aval){.d=" + name + "(ad(" + evalA(*c.args[0].value) + ")), .f=1}";
        }
        // User function: evaluate args left-to-right, store result to a temp.
        std::string call = "afn_" + name + "(";
        for (std::size_t i = 0; i < c.args.size(); ++i) {
          if (i) call += ", ";
          call += evalA(*c.args[i].value);
        }
        call += ")";
        std::string tn = tmp();
        out_ << "  " << tn << " = " << call << ";\n";
        return tn;
      }
      default:
        return "(aval){.i=0,.f=0}";
    }
  }

  std::string evalI(const Expr& e) {
    std::string av = evalA(e);
    std::string tn = tmp();
    out_ << "  " << tn << " = " << av << ";\n";
    return "ai(" + tn + ")";
  }
  std::string evalD(const Expr& e) {
    std::string av = evalA(e);
    std::string tn = tmp();
    out_ << "  " << tn << " = " << av << ";\n";
    return "ad(" + tn + ")";
  }

  void emitPrint(const Expr& arg) {
    if (arg.kind == ExprKind::StringLit) {
      std::string lit = extractStringC(static_cast<const StringLitExpr&>(arg));
      out_ << "  printf(\"%s\\n\", \"" << escapeC(lit) << "\");\n";
      return;
    }
    std::string av = evalA(arg);
    std::string tn = tmp();
    out_ << "  " << tn << " = " << av << ";\n";
    out_ << "  if (" << tn << ".f == 2) {\n"
         << "    a_obj *o_ = (a_obj *)" << tn << ".i;\n"
         << "    if (o_->type == 1) printf(\"%s\\n\", (char *)o_->data);\n"
         << "    else printf(\"<obj>\\n\");\n"
         << "  } else if (" << tn << ".f) a_print_double(" << tn << ".d);"
         << " else printf(\"%lld\\n\", " << tn << ".i);\n";
  }

  void emitFunction(const FnDecl& fn) {
    locals_.clear();
    slotType_.clear();
    inMain_ = false;
    tmpTop_ = 0;
    for (const auto& p : fn.params) {
      std::string n = cIdent(p.name);
      locals_.insert(n);
      slotType_[n] = VT::Unknown;  // params carry their tag at runtime
    }
    collectLocalNames(fn.body);

    out_ << "aval afn_" << cIdent(fn.name) << "(";
    for (std::size_t i = 0; i < fn.params.size(); ++i) {
      if (i) out_ << ", ";
      out_ << "aval v_" << cIdent(fn.params[i].name);
    }
    out_ << ") {\n";
    for (const auto& n : locals_) {
      bool isParam = false;
      for (const auto& p : fn.params) if (cIdent(p.name) == n) { isParam = true; break; }
      if (!isParam) out_ << "  aval v_" << n << " = {0};\n";
    }
    for (int i = 0; i < 256; ++i) out_ << "  aval _t" << i << " = {0};\n";
    for (const auto& s : fn.body) emitStmt(*s);
    out_ << "  return (aval){.i=0,.f=0};\n";
    out_ << "}\n\n";
  }

  void setType(const std::string& lv, const std::string& name, VT vt) {
    if (isGlobalLV(lv)) globalType_[name] = vt;
    else slotType_[name] = vt;
  }

  void emitStmt(const Stmt& stmt) {
    tmpTop_ = 0;
    switch (stmt.kind) {
      case StmtKind::Let: {
        const auto& s = static_cast<const LetStmt&>(stmt);
        if (!s.init) break;
        std::string lv = lvalue(s.name);
        std::string av = evalA(*s.init);
        out_ << "  " << lv << " = " << av << ";\n";
        setType(lv, cIdent(s.name), exprType(*s.init));
        break;
      }
      case StmtKind::Assign: {
        const auto& s = static_cast<const AssignStmt&>(stmt);
        if (s.target->kind != ExprKind::Ident) break;
        std::string raw = static_cast<const IdentExpr&>(*s.target).name;
        std::string n = cIdent(raw);
        std::string lv = lvalue(raw);
        VT curT = isGlobalLV(lv) ? globalType_[n] : slotType_[n];
        VT valT = exprType(*s.value);
        if (s.op == "=") {
          std::string av = evalA(*s.value);
          out_ << "  " << lv << " = " << av << ";\n";
          setType(lv, n, valT);
          break;
        }
        std::string base = s.op.substr(0, s.op.size() - 1);
        std::string rhs = evalA(*s.value);
        // Result type of the compound op.
        VT resT;
        static const std::set<std::string> bit = {"&","|","^","<<",">>","and","or"};
        static const std::set<std::string> cmp = {"==","!=","<","<=",">",">="};
        if (bit.count(base) || cmp.count(base)) resT = VT::Int;
        else if (curT == VT::Float || valT == VT::Float) resT = VT::Float;
        else if (curT == VT::Unknown || valT == VT::Unknown) resT = VT::Unknown;
        else resT = VT::Int;
        VT want = (curT == VT::Unknown || valT == VT::Unknown) ? VT::Unknown : resT;
        std::string res = avalBin(base, lv, rhs, want);
        out_ << "  " << lv << " = " << res << ";\n";
        setType(lv, n, resT);
        break;
      }
      case StmtKind::If: {
        const auto& s = static_cast<const IfStmt&>(stmt);
        for (std::size_t i = 0; i < s.branches.size(); ++i) {
          const auto& br = s.branches[i];
          std::string av = evalA(*br.cond);
          std::string tn = tmp();
          out_ << "  " << tn << " = " << av << ";\n";
          out_ << (i == 0 ? "  if (" : "  else if (") << tn << ".f ? " << tn
               << ".d != 0.0 : " << tn << ".i != 0) {\n";
          for (const auto& b : br.body) emitStmt(*b);
          out_ << "  }";
        }
        if (!s.elseBody.empty()) {
          out_ << (s.branches.empty() ? "  {\n" : " else {\n");
          for (const auto& b : s.elseBody) emitStmt(*b);
          out_ << "  }";
        }
        out_ << "\n";
        break;
      }
      case StmtKind::While: {
        const auto& s = static_cast<const WhileStmt&>(stmt);
        std::string tn = tmp();
        out_ << "  aval " << tn << " = {0};\n";
        out_ << "  while (1) {\n";
        std::string av = evalA(*s.cond);
        out_ << "    " << tn << " = " << av << ";\n";
        out_ << "    if (!(" << tn << ".f ? " << tn << ".d != 0.0 : " << tn << ".i != 0)) break;\n";
        for (const auto& b : s.body) emitStmt(*b);
        out_ << "  }\n";
        break;
      }
      case StmtKind::For: {
        const auto& s = static_cast<const ForStmt&>(stmt);
        if (s.iterable->kind != ExprKind::Range) break;
        const auto& r = static_cast<const RangeExpr&>(*s.iterable);
        std::string v = cIdent(s.var);
        std::string itN = "v___iter_" + v;
        std::string endN = "v___end_" + v;
        std::string varN = "v_" + v;
        slotType_[v] = VT::Int;
        slotType_["__iter_" + v] = VT::Int;
        slotType_["__end_" + v] = VT::Int;
        std::string is = evalI(*r.start);
        std::string ie = evalI(*r.end);
        out_ << "  " << itN << ".i = " << is << "; " << itN << ".f = 0;\n";
        out_ << "  " << endN << ".i = " << ie << "; " << endN << ".f = 0;\n";
        out_ << "  for (; " << itN << ".i < " << endN << ".i; "
             << itN << ".i = " << itN << ".i + 1) {\n";
        out_ << "    " << varN << " = " << itN << ";\n";
        for (const auto& b : s.body) emitStmt(*b);
        out_ << "  }\n";
        break;
      }
      case StmtKind::Return: {
        const auto& s = static_cast<const ReturnStmt&>(stmt);
        if (inMain_) {
          if (s.value) {
            std::string av = evalA(*s.value);
            std::string tn = tmp();
            out_ << "  " << tn << " = " << av << ";\n";
          }
          out_ << "  return 0;\n";
          break;
        }
        std::string rv = s.value ? evalA(*s.value) : "(aval){.i=0,.f=0}";
        out_ << "  return " << rv << ";\n";
        break;
      }
      case StmtKind::Expr: {
        const auto& s = static_cast<const ExprStmt&>(stmt);
        if (s.expr->kind == ExprKind::Call) {
          const auto& c = static_cast<const CallExpr&>(*s.expr);
          if (c.callee->kind == ExprKind::Ident &&
              cIdent(static_cast<const IdentExpr&>(*c.callee).name) == "print" &&
              c.args.size() == 1) {
            emitPrint(*c.args[0].value);
            break;
          }
        }
        std::string ev = evalA(*s.expr);
        out_ << "  (void)" << ev << ";\n";
        break;
      }
      default:
        break;
    }
  }

  void emitMain() {
    locals_.clear();
    slotType_.clear();
    inMain_ = true;
    tmpTop_ = 0;

    out_ << "int main(void) {\n";
    if (graph_.entry >= 0 &&
        static_cast<std::size_t>(graph_.entry) < graph_.modules.size()) {
      const auto& root = graph_.modules[static_cast<std::size_t>(graph_.entry)];
      if (root.program) {
        collectLocalNames(root.program->statements);
        for (const auto& n : locals_) out_ << "  aval v_" << n << " = {0};\n";
        for (int i = 0; i < 256; ++i) out_ << "  aval _t" << i << " = {0};\n";
        for (const auto& stmt : root.program->statements) {
          if (stmt->kind != StmtKind::FnDecl) emitStmt(*stmt);
        }
      }
    }
    out_ << "  return 0;\n";
    out_ << "}\n";
  }
};

}  // namespace

bool emitCSource(const ModuleGraph& graph, const SemaResult& sema, std::ostream& out) {
  CEmitter emitter(graph, sema, out);
  return emitter.emit();
}

}  // namespace a
