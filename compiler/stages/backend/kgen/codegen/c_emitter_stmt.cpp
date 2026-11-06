#include "backend/kgen/codegen/c_emitter.hpp"

#include "backend/kgen/codegen/emitter_common.hpp"
#include <iosfwd>
#include <string>
#include <unordered_map>
#include <vector>
#include <set>
#include "ast/ast.hpp"
#include "name-resolution/module_loader.hpp"
#include "type-checker/sema.hpp"

namespace a {

void CEmitter::emitStmt(const Stmt& stmt)  {
    // tmpTop_ is monotonic per function: temps must stay live across nested
    // statements (e.g. a for-loop's iterable), so they are only reset when a
    // function/main body starts.
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
        if (s.target->kind == ExprKind::Index && s.op == "=") {
          const auto& ix = static_cast<const IndexExpr&>(*s.target);
          std::string ov = evalA(*ix.object);
          std::string iv = evalA(*ix.index);
          std::string vv = evalA(*s.value);
          int ti = tmpTop_++;
          std::string tn = "_t" + std::to_string(ti);
          out_ << "  {\n    " << tn << " = " << ov << ";\n";
          out_ << "    if (" << tn << ".f == 2 && ((a_obj *)" << tn
               << ".i)->type == 2) {\n";
          out_ << "      a_i64 _k" << ti << " = ai(" << iv << ");\n";
          out_ << "      a_obj *_o" << ti << " = (a_obj *)" << tn << ".i;\n";
          out_ << "      if (_k" << ti << " >= 0 && _k" << ti << " < _o" << ti
               << "->len) ((aval *)_o" << ti << "->data)[_k" << ti << "] = " << vv << ";\n";
          out_ << "    } else if (" << tn << ".f == 2 && ((a_obj *)" << tn
               << ".i)->type == 3) {\n";
          out_ << "      a_dict_set(" << tn << ", " << iv << ", " << vv << ");\n";
          out_ << "    }\n  }\n";
          break;
        }
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
          out_ << (i == 0 ? "  if (" : "  else if (") << tn << ".f == 1 ? " << tn
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
        /* aval slot predeclared at function top; do not redeclare here. */
        out_ << "  while (1) {\n";
        std::string av = evalA(*s.cond);
        out_ << "    " << tn << " = " << av << ";\n";
        out_ << "    if (!(" << tn << ".f == 1 ? " << tn << ".d != 0.0 : " << tn << ".i != 0)) break;\n";
        for (const auto& b : s.body) emitStmt(*b);
        out_ << "  }\n";
        break;
      }
      case StmtKind::For: {
        const auto& s = static_cast<const ForStmt&>(stmt);
        std::string v = cIdent(s.var);
        std::string varN = "v_" + v;
        slotType_[v] = VT::Unknown;
        if (s.iterable->kind == ExprKind::Range) {
          const auto& r = static_cast<const RangeExpr&>(*s.iterable);
          std::string itN = "v___iter_" + v;
          std::string endN = "v___end_" + v;
          slotType_[v] = VT::Int;
          slotType_["__iter_" + v] = VT::Int;
          slotType_["__end_" + v] = VT::Int;
          std::string is = evalI(*r.start);
          std::string ie = evalI(*r.end);
          out_ << "  " << itN << ".i = " << is << "; " << itN << ".f = 0;\n";
          out_ << "  " << endN << ".i = " << ie << "; " << endN << ".f = 0;\n";
          if (r.step) {
            int si = tmpTop_++;
            std::string st = "_t" + std::to_string(si);
            std::string sv = evalI(*r.step);
            out_ << "  " << st << ".i = " << sv << "; " << st << ".f = 0;\n";
            out_ << "  if (" << st << ".i <= 0) { fprintf(stderr, \"error: The range step must be positive.\\n\"); exit(2); }\n";
            out_ << "  for (; (" << st << ".i > 0 ? " << itN << ".i < " << endN
                 << ".i : " << itN << ".i > " << endN << ".i); "
                 << itN << ".i = " << itN << ".i + " << st << ".i) {\n";
          } else {
            out_ << "  for (; " << itN << ".i < " << endN << ".i; "
                 << itN << ".i = " << itN << ".i + 1) {\n";
          }
          out_ << "    " << varN << " = " << itN << ";\n";
          for (const auto& b : s.body) emitStmt(*b);
          out_ << "  }\n";
          break;
        }
        // Generic iterable (list/string): index-loop via a_index.
        int ti = tmpTop_++;
        std::string it = "_t" + std::to_string(ti);
        std::string ov = evalA(*s.iterable);
        out_ << "  " << it << " = " << ov << ";\n";
        out_ << "  if (" << it << ".f == 2) {\n";
        out_ << "    a_obj *_o" << ti << " = (a_obj *)" << it << ".i;\n";
        out_ << "    for (a_i64 _k" << ti << " = 0; _k" << ti << " < _o" << ti
             << "->len; ++_k" << ti << ") {\n";
        out_ << "      " << varN << " = a_index(" << it << ", (aval){.i=_k" << ti
             << ",.f=0});\n";
        for (const auto& b : s.body) emitStmt(*b);
        out_ << "    }\n  }\n";
        break;
      }
      case StmtKind::Loop: {
        const auto& s = static_cast<const LoopStmt&>(stmt);
        out_ << "  while (1) {\n";
        for (const auto& b : s.body) emitStmt(*b);
        out_ << "  }\n";
        break;
      }
      case StmtKind::DoWhile: {
        const auto& s = static_cast<const DoWhileStmt&>(stmt);
        std::string tn = tmp();
        out_ << "  do {\n";
        for (const auto& b : s.body) emitStmt(*b);
        std::string av = evalA(*s.cond);
        out_ << "    " << tn << " = " << av << ";\n";
        out_ << "  } while (" << tn << ".f == 1 ? " << tn << ".d != 0.0 : " << tn << ".i != 0);\n";
        break;
      }
      case StmtKind::Destructure: {
        const auto& s = static_cast<const DestructureStmt&>(stmt);
        std::string av = evalA(*s.value);
        std::string tn = tmp();
        out_ << "  " << tn << " = " << av << ";\n";
        for (std::size_t i = 0; i < s.names.size(); ++i) {
          if (s.names[i] == "_") continue;
          std::string lv = lvalue(s.names[i]);
          out_ << "  " << lv << " = a_index(" << tn << ", (aval){.i=" << i << ",.f=0});\n";
        }
        break;
      }
      case StmtKind::Defer: {
        deferStack_.push_back(&stmt);
        break;
      }
      case StmtKind::Import:
      case StmtKind::FromImport:
        /* Module binding handled by interpreter; native ignores (stdlib prelude never emitted). */
        break;
      case StmtKind::Break:
        out_ << "    break;\n";
        break;
      case StmtKind::Continue:
        out_ << "    continue;\n";
        break;
      case StmtKind::Return: {
        const auto& s = static_cast<const ReturnStmt&>(stmt);
        if (inMain_) {
          if (s.value) {
            std::string av = evalA(*s.value);
            std::string tn = tmp();
            out_ << "  " << tn << " = " << av << ";\n";
          }
          emitDefers();
          out_ << "  return 0;\n";
          break;
        }
        std::string rv = s.value ? evalA(*s.value) : "(aval){.i=0,.f=0}";
        std::string rtn = tmp();
        out_ << "  " << rtn << " = " << rv << ";\n";
        emitDefers();
        out_ << "  return " << rtn << ";\n";
        break;
      }
      case StmtKind::Expr: {
        const auto& s = static_cast<const ExprStmt&>(stmt);
        if (s.expr->kind == ExprKind::Call) {
          const auto& c = static_cast<const CallExpr&>(*s.expr);
          if (c.callee->kind == ExprKind::Ident) {
            const std::string n = cIdent(static_cast<const IdentExpr&>(*c.callee).name);
            if (n == "print" || n == "eprint") {
              emitPrintCall(c, n == "eprint");
              break;
            }
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

void CEmitter::emitDefers()  {
    if (deferStack_.empty()) return;
    for (auto it = deferStack_.rbegin(); it != deferStack_.rend(); ++it) {
      const DeferStmt& d = static_cast<const DeferStmt&>(**it);
      if (d.body) emitStmt(*d.body);
    }
    deferStack_.clear();
}

void CEmitter::emitFunction(const FnDecl& fn)  {
    deferStack_.clear();
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
    for (int i = 0; i < 2048; ++i) out_ << "  aval _t" << i << " = {0};\n";
    for (const auto& s : fn.body) emitStmt(*s);
    emitDefers();
    out_ << "  return (aval){.i=0,.f=0};\n";
    out_ << "}\n\n";
  
}

void CEmitter::emitPrint(const Expr& arg)  {
    if (arg.kind == ExprKind::StringLit) {
      bool hasExpr = false;
      for (const auto& part : static_cast<const StringLitExpr&>(arg).parts) if (part.isExpr) hasExpr = true;
      if (!hasExpr) {
        std::string lit = detail::extractString(static_cast<const StringLitExpr&>(arg));
        out_ << "  printf(\"%s\\n\", \"" << escapeC(lit) << "\");\n";
        return;
      }
    }
    std::string av = evalA(arg);
    std::string tn = tmp();
    out_ << "  " << tn << " = " << av << ";\n";
    out_ << "  if (" << tn << ".f == 2) {\n"
         << "    a_obj *o_ = (a_obj *)" << tn << ".i;\n"
         << "    if (o_->type == 1) printf(\"%s\\n\", (char *)o_->data);\n"
         << "    else a_print_obj(o_), printf(\"\\n\");\n"
         << "  } else if (" << tn << ".f == 6) printf(\"%c\\n\", (int)" << tn << ".i);"
         << "  else if (" << tn << ".f == 4) printf(\"%s\\n\", " << tn << ".i ? \"true\" : \"false\");"
         << "  else if (" << tn << ".f == 5) printf(\"None\\n\");"
         << "  else if (" << tn << ".f == 1) a_print_double(" << tn << ".d);"
         << " else printf(\"%lld\\n\", " << tn << ".i);\n";
  
}

void CEmitter::emitPrintValue(const std::string& stream, const Expr& arg)  {
    if (arg.kind == ExprKind::StringLit) {
      bool hasExpr = false;
      for (const auto& part : static_cast<const StringLitExpr&>(arg).parts) if (part.isExpr) hasExpr = true;
      if (!hasExpr) {
        std::string lit = detail::extractString(static_cast<const StringLitExpr&>(arg));
        out_ << "  fputs(\"" << escapeC(lit) << "\", " << stream << ");\n";
        return;
      }
    }
    std::string av = evalA(arg);
    std::string tn = tmp();
    out_ << "  " << tn << " = " << av << ";\n";
    out_ << "  if (" << tn << ".f == 2) {\n"
         << "    a_obj *o_ = (a_obj *)" << tn << ".i;\n"
         << "    if (o_->type == 1) fprintf(" << stream << ", \"%s\", (char *)o_->data);\n"
         << "    else a_print_obj_to(" << stream << ", o_);\n"
         << "  } else if (" << tn << ".f == 6) fprintf(" << stream << ", \"%c\", (int)" << tn << ".i);"
         << "  else if (" << tn << ".f == 4) fprintf(" << stream << ", \"%s\", " << tn << ".i ? \"true\" : \"false\");"
         << "  else if (" << tn << ".f == 5) fprintf(" << stream << ", \"None\");"
         << "  else if (" << tn << ".f == 1) { char buf_[64]; a_fmt_double(buf_, " << tn << ".d); fprintf(" << stream << ", \"%s\", buf_); }"
         << " else fprintf(" << stream << ", \"%lld\", " << tn << ".i);\n";
  
}

void CEmitter::emitPrintCall(const CallExpr& c, bool toStderr)  {
    std::vector<const Expr*> objects;
    const Expr *sep = nullptr, *end = nullptr, *file = nullptr, *flush = nullptr;
    for (const auto& arg : c.args) {
      if (arg.name.empty()) objects.push_back(arg.value.get());
      else if (arg.name == "sep") sep = arg.value.get();
      else if (arg.name == "end") end = arg.value.get();
      else if (arg.name == "file") file = arg.value.get();
      else if (arg.name == "flush") flush = arg.value.get();
    }
    // Stream handle is a void*, not an aval slot; use a scoped name to
    // avoid colliding with the 2048 predeclared aval _tN slots.
    const std::string stream = "a_stream_var_" + std::to_string(tmpTop_++);
    const std::string sepVar = "sep_" + std::to_string(tmpTop_++);
    const std::string endVar = "end_" + std::to_string(tmpTop_++);
    if (file != nullptr) {
      std::string fh = evalA(*file);
      std::string tn = tmp();
      out_ << "  " << tn << " = " << fh << ";\n";
      out_ << "  void *" << stream << " = a_stream(" << tn << ".i);\n";
    } else {
      out_ << "  void *" << stream << " = " << (toStderr ? "stderr" : "stdout") << ";\n";
    }
    auto emitTextVar = [&](const Expr* expr, const char* def, const std::string& var) {
      if (expr == nullptr) {
        out_ << "  const char *" << var << " = \"" << def << "\";\n";
      } else if (expr->kind == ExprKind::StringLit && [&]{
        for (const auto& part : static_cast<const StringLitExpr&>(*expr).parts) if (part.isExpr) return true;
        return false;
      }() == false) {
        out_ << "  const char *" << var << " = \"" << escapeC(detail::extractString(static_cast<const StringLitExpr&>(*expr))) << "\";\n";
      } else {
        std::string av = evalA(*expr);
        std::string tn = tmp();
        out_ << "  " << tn << " = " << av << ";\n";
        out_ << "  const char *" << var << " = (" << tn << ".f == 2 ? (char *)((a_obj *)" << tn << ".i)->data : \"\");\n";
      }
    };
    emitTextVar(sep, " ", sepVar);
    emitTextVar(end, "\\n", endVar);
    for (std::size_t i = 0; i < objects.size(); ++i) {
      if (i) out_ << "  fputs(" << sepVar << ", " << stream << ");\n";
      emitPrintValue(stream, *objects[i]);
    }
    out_ << "  fputs(" << endVar << ", " << stream << ");\n";
    if (flush != nullptr) {
      std::string av = evalA(*flush);
      std::string tn = tmp();
      out_ << "  " << tn << " = " << av << ";\n";
      out_ << "  if (" << tn << ".i) fflush(" << stream << ");\n";
    } else if (toStderr) {
      out_ << "  fflush(" << stream << ");\n";
    }
  
}

std::string CEmitter::avalBin(const std::string& op, const std::string& l,
                      const std::string& r, VT want)  {
    // Short-circuit / membership / power / coalesce dispatch first.
    if (op == "??") {
      std::string tn = tmp();
      out_ << "  " << tn << " = " << l << ";\n";
      out_ << "  if (" << tn << ".f == 5) " << tn << " = " << r << ";\n";
      return tn;
    }
    if (op == "in" || op == "not in") {
      std::string tn = tmp();
      // a_contains(containing, item): `x in l` -> contains(l, x)
      out_ << "  " << tn << " = a_contains(" << r << ", " << l << ");\n";
      if (op == "not in") out_ << "  " << tn << ".i = !" << tn << ".i;\n";
      return tn;
    }
    if (op == "**") {
      std::string tn = tmp();
      out_ << "  " << tn << " = a_pow(" << l << ", " << r << ");\n";
      return tn;
    }
    if (op == "and") {
      std::string tn = tmp();
      out_ << "  " << tn << ".f = 4; " << tn << ".d = 0;\n";
      out_ << "  " << tn << ".i = (ai(" << l << ") && ai(" << r << ")) ? 1 : 0;\n";
      return tn;
    }
    if (op == "or") {
      std::string tn = tmp();
      out_ << "  " << tn << ".f = 4; " << tn << ".d = 0;\n";
      out_ << "  " << tn << ".i = (ai(" << l << ") || ai(" << r << ")) ? 1 : 0;\n";
      return tn;
    }
    // "/" and "%" always go through the runtime helpers: they handle int/float
    // promotion and zero-divisor checks for every operand combination.
    if (op == "/" || op == "%") {
      std::string tn = tmp();
      out_ << "  " << tn << " = a_" << (op == "/" ? "div" : "mod") << "("
           << l << ", " << r << ");\n";
      return tn;
    }
    if (want == VT::Int) {
      static const std::set<std::string> cmp = {"==","!=","<","<=",">",">="};
      if (cmp.count(op)) {
        if (op == "==") {
          std::string tn = tmp();
          out_ << "  " << tn << " = (aval){.i=a_aval_eq(" << l << ", " << r << "),.f=4};\n";
          return tn;
        }
        if (op == "!=") {
          std::string tn = tmp();
          out_ << "  " << tn << " = (aval){.i=!a_aval_eq(" << l << ", " << r << "),.f=4};\n";
          return tn;
        }
        std::string cop = op;
        return "(aval){.i=(" + std::string("ai(") + l + ") " + cop + " ai(" + r + ")), .f=4}";
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
        if (op == "<<") { std::string tn = tmp(); out_ << "  " << tn << " = a_shift(" << l << ", " << r << ", 0);\n"; return tn; }
        if (op == ">>") { std::string tn = tmp(); out_ << "  " << tn << " = a_shift(" << l << ", " << r << ", 1);\n"; return tn; }
        return "(aval){.i=(ai(" + l + ") " + bop + " ai(" + r + ")), .f=0}";
      }
      // arith int
      if (op == "+") { std::string tn = tmp(); out_ << "  " << tn << " = a_cat(" << l << ", " << r << ");\n"; return tn; }
      if (op == "-") { std::string tn = tmp(); out_ << "  " << tn << " = a_sub(" << l << ", " << r << ");\n"; return tn; }
      if (op == "*") { std::string tn = tmp(); out_ << "  " << tn << " = a_mul(" << l << ", " << r << ");\n"; return tn; }
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
      out_ << "  if ((" << l << ").f == 1 || (" << r << ").f == 1) " << tn
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
      if (bop == "SHL") out_ << "  " << tn << " = a_shift(" << l << ", " << r << ", 0);\n";
      else if (bop == "SHR") out_ << "  " << tn << " = a_shift(" << l << ", " << r << ", 1);\n";
      else out_ << "  " << tn << ".i = ai(" << l << ") " << bop << " ai(" << r << ");\n";
      out_ << "  " << tn << ".f = 0;\n";
      return tn;
    }
    // "+" and "*" dispatch through helpers that handle strings and
    // numeric promotion.
    if (op == "+") { out_ << "  " << tn << " = a_cat(" << l << ", " << r << ");\n"; return tn; }
    if (op == "-") { out_ << "  " << tn << " = a_sub(" << l << ", " << r << ");\n"; return tn; }
    if (op == "*") {
      out_ << "  " << tn << " = a_mul(" << l << ", " << r << ");\n";
      return tn;
    }
    std::string iop, dop;
    if (op == "+") { iop = "+"; dop = "+"; }
    else if (op == "-") { iop = "-"; dop = "-"; }
    else if (op == "*") { iop = "*"; dop = "*"; }
    else if (op == "/") { iop = "/"; dop = "/"; }
    else if (op == "%") { iop = "%"; dop = "FMOD"; }
    else { iop = "+"; dop = "+"; }
    out_ << "  if ((" << l << ").f == 1 || (" << r << ").f == 1) {\n";
    if (dop == "FMOD")
      out_ << "    " << tn << ".d = fmod(ad(" << l << "), ad(" << r << "));\n";
    else
      out_ << "    " << tn << ".d = ad(" << l << ") " << dop << " ad(" << r << ");\n";
    out_ << "    " << tn << ".f = 1;\n  } else {\n";
    out_ << "    " << tn << ".i = ai(" << l << ") " << iop << " ai(" << r << ");\n";
    out_ << "    " << tn << ".f = 0;\n  }\n";
    return tn;
  
}

std::string CEmitter::evalA(const Expr& e)  {
    switch (e.kind) {
      case ExprKind::IntLit:
        return "(aval){.i=" + std::to_string(static_cast<const IntLitExpr&>(e).value) + ",.f=0}";
      case ExprKind::BoolLit:
        return std::string("(aval){.i=") + (static_cast<const BoolLitExpr&>(e).value ? "1" : "0") + ",.f=4}";
      case ExprKind::CharLit:
        return "(aval){.i=" + std::to_string(static_cast<std::int64_t>(static_cast<const CharLitExpr&>(e).value)) + ",.f=6}";
      case ExprKind::FloatLit:
        return "(aval){.d=" + floatLit(static_cast<const FloatLitExpr&>(e).value) + ",.f=1}";
      case ExprKind::Ident: {
        const std::string n = static_cast<const IdentExpr&>(e).name;
        if (n == "None") return "(aval){.i=0,.f=5}";
        return identA(n);
      }
      case ExprKind::StringLit: {
        const auto& s = static_cast<const StringLitExpr&>(e);
        bool hasExpr = false;
        for (const auto& part : s.parts) if (part.isExpr) hasExpr = true;
        if (!hasExpr) {
          std::string lit;
          for (const auto& p : s.parts) if (!p.isExpr) lit += p.text;
          auto it = stringLitIdx_.find(lit);
          if (it == stringLitIdx_.end()) return "(aval){.i=0,.f=2}";
          return "(aval){.i=(a_i64)&a_so_" + std::to_string(it->second) + ",.f=2}";
        }
        std::string tn = tmp();
        {
          std::string acc;
          bool haveAcc = false;
          for (const auto& part : s.parts) {
            if (part.isExpr) {
              std::string ev = evalA(*part.expr);
              std::string sv = tmp();
              out_ << "  " << sv << " = a_strof(" << ev << ");\n";
              if (!haveAcc) { acc = sv; haveAcc = true; }
              else {
                std::string na = tmp();
                out_ << "  " << na << " = a_cat(" << acc << ", " << sv << ");\n";
                acc = na;
              }
            } else if (!part.text.empty()) {
              std::string lit = part.text;
              auto it = stringLitIdx_.find(lit);
              std::string seg = (it != stringLitIdx_.end())
                  ? ("(aval){.i=(a_i64)&a_so_" + std::to_string(it->second) + ",.f=2}")
                  : "(aval){.i=0,.f=2}";
              if (!haveAcc) { acc = tmp(); out_ << "  " << acc << " = " << seg << ";\n"; haveAcc = true; }
              else {
                std::string na = tmp();
                out_ << "  " << na << " = a_cat(" << acc << ", " << seg << ");\n";
                acc = na;
              }
            }
          }
          if (!haveAcc) { acc = tmp(); out_ << "  " << acc << " = (aval){.i=0,.f=2};\n"; }
          out_ << "  " << tn << " = " << acc << ";\n";
        }
        return tn;
      }
      case ExprKind::Ternary: {
        const auto& tn2 = static_cast<const TernaryExpr&>(e);
        std::string cv = evalA(*tn2.cond);
        std::string ct = tmp();
        out_ << "  " << ct << " = " << cv << ";\n";
        out_ << "  if (!(" << ct << ".f == 1 ? " << ct << ".d != 0.0 : " << ct << ".i != 0)) {\n";
        std::string ev = evalA(*tn2.elseValue);
        std::string av = evalA(*tn2.thenValue);
        std::string rn = tmp();
        out_ << "  " << rn << " = " << av << "; } else {\n";
        out_ << "  " << rn << " = " << ev << "; }\n";
        return rn;
      }
      case ExprKind::Tuple: {
        const auto& a = static_cast<const TupleExpr&>(e);
        std::string tn = tmp();
        out_ << "  {\n";
        out_ << "    a_obj *la_ = a_new_obj(2, " << a.elements.size() << ", "
             << a.elements.size() << ", malloc(sizeof(aval) * " << (a.elements.size() ? a.elements.size() : 1) << "));\n";
        out_ << "    " << tn << ".f = 2; " << tn << ".i = (a_i64)la_; " << tn << ".d = 0;\n";
        for (std::size_t i = 0; i < a.elements.size(); ++i) {
          std::string av = evalA(*a.elements[i]);
          out_ << "    ((aval *)la_->data)[" << i << "] = " << av << ";\n";
        }
        out_ << "  }\n";
        return tn;
      }
      case ExprKind::Slice: {
        const auto& sl = static_cast<const SliceExpr&>(e);
        int si = tmpTop_++;
        std::string obj = evalA(*sl.object);
        std::string tn = "_t" + std::to_string(tmpTop_++);
        out_ << "  {\n";
        out_ << "    " << tn << ".f = 2; " << tn << ".i = (a_i64)a_new_obj(2, 0, 0, 0); " << tn << ".d = 0;\n";
        out_ << "    aval _src = " << obj << ";\n";
        out_ << "    if (_src.f == 2) {\n";
        out_ << "      a_obj *_o" << si << " = (a_obj *)_src.i;\n";
        out_ << "      a_i64 _len" << si << " = _o" << si << "->len;\n";
        std::string sn = sl.start ? evalA(*sl.start) : "(aval){.i=0,.f=0}";
        std::string en = sl.end ? evalA(*sl.end) : "(aval){.i=(a_i64)_len" + std::to_string(si) + ",.f=0}";
        std::string pn = sl.step ? evalA(*sl.step) : "(aval){.i=1,.f=0}";
        out_ << "      a_i64 _s" << si << " = ai(" << sn << ");\n";
        out_ << "      a_i64 _e" << si << " = ai(" << en << ");\n";
        out_ << "      a_i64 _p" << si << " = ai(" << pn << ");\n";
        out_ << "      if (_p" << si << " == 0) { fprintf(stderr, \"error: The slice step must be positive, found 0.\\n\"); exit(2); }\n";
        out_ << "      if (_p" << si << " < 0) { fprintf(stderr, \"error: The slice step must be positive, found %lld.\\n\", _p" << si << "); exit(2); }\n";
        out_ << "      if (_s" << si << " < 0) _s" << si << " += _len" << si << ";\n";
        out_ << "      if (_e" << si << " < 0) _e" << si << " += _len" << si << ";\n";
        out_ << "      if (_s" << si << " < 0) _s" << si << " = 0; if (_e" << si << " > _len" << si << ") _e" << si << " = _len" << si << ";\n";
        out_ << "      if (_o" << si << "->type == 1) {\n";
        out_ << "        a_i64 _cap" << si << " = (_e" << si << " - _s" << si << ") > 0 ? ((_e" << si << " - _s" << si << " + _p" << si << " - 1) / _p" << si << ") : 0;\n";
        out_ << "        char *b = (char *)malloc((size_t)_cap" << si << " + 1);\n";
        out_ << "        a_i64 _n" << si << " = 0; for (a_i64 _k" << si << " = _s" << si << "; _k" << si << " < _e" << si << "; _k" << si << " += _p" << si << ") b[_n" << si << "++] = ((char *)_o" << si << "->data)[_k" << si << "];\n";
        out_ << "        b[_n" << si << "] = 0;\n";
        out_ << "        " << tn << " = (aval){.i=(a_i64)a_new_obj(1, _n" << si << ", 0, b),.f=2};\n";
        out_ << "      } else if (_o" << si << "->type == 2 || _o" << si << "->type == 4) {\n";
        out_ << "        for (a_i64 _k" << si << " = _s" << si << "; _k" << si << " < _e" << si << "; _k" << si << " += _p" << si << ") (void)a_append(" << tn << ", ((aval *)_o" << si << "->data)[_k" << si << "]);\n";
        out_ << "        if (_p" << si << " == 1) ((a_obj *)" << tn << ".i)->type = 4;\n";
        out_ << "      }\n    }\n  }\n";
        return tn;
      }
      case ExprKind::ArrayLit: {
        const auto& a = static_cast<const ArrayLitExpr&>(e);
        std::string tn = tmp();
        out_ << "  {\n";
        out_ << "    a_obj *la_ = a_new_obj(2, " << a.elements.size() << ", "
             << a.elements.size() << ", malloc(sizeof(aval) * " << (a.elements.size() ? a.elements.size() : 1) << "));\n";
        out_ << "    " << tn << ".f = 2; " << tn << ".i = (a_i64)la_; " << tn << ".d = 0;\n";
        for (std::size_t i = 0; i < a.elements.size(); ++i) {
          std::string av = evalA(*a.elements[i]);
          out_ << "    ((aval *)la_->data)[" << i << "] = " << av << ";\n";
        }
        out_ << "  }\n";
        return tn;
      }
      case ExprKind::Lambda: {
        auto it = lambdaIdx_.find(static_cast<const LambdaExpr*>(&e));
        if (it == lambdaIdx_.end()) return "(aval){.i=0,.f=0}";
        return "(aval){.i=(a_i64)afn_l" + std::to_string(it->second) + ",.f=3}";
      }
      case ExprKind::Comprehension: {
        const auto& cp = static_cast<const ComprehensionExpr&>(e);
        int ti = tmpTop_++;
        std::string it = "_t" + std::to_string(ti);
        std::string res = "_t" + std::to_string(tmpTop_++);
        std::string vn = "v_" + cIdent(cp.var);
        out_ << "  {\n";
        out_ << "    aval " << vn << " = {0};\n";
        out_ << "    " << res << ".f = 2; " << res << ".i = (a_i64)a_new_obj(2, 0, 0, 0); "
             << res << ".d = 0;\n";
        bool isRange = cp.iterable->kind == ExprKind::Range;
        if (isRange) {
          const auto& r = static_cast<const RangeExpr&>(*cp.iterable);
          std::string is = evalI(*r.start);
          std::string ie = evalI(*r.end);
          if (r.step) {
            int si = tmpTop_++;
            std::string st = "_t" + std::to_string(si);
            std::string sv = evalI(*r.step);
            out_ << "    " << st << ".i = " << sv << "; " << st << ".f = 0;\n";
            out_ << "    if (" << st << ".i <= 0) { fprintf(stderr, \"error: The range step must be positive.\\n\"); exit(2); }\n";
            out_ << "    for (a_i64 _k" << ti << " = " << is << "; (" << st
                 << ".i > 0 ? _k" << ti << " < " << ie << " : _k" << ti << " > " << ie
                 << "); _k" << ti << " += " << st << ".i) {\n";
          } else {
            out_ << "    for (a_i64 _k" << ti << " = " << is << "; _k" << ti
                 << " < " << ie << "; ++_k" << ti << ") {\n";
          }
          out_ << "      " << vn << " = (aval){.i=_k" << ti << ",.f=0};\n";
        } else {
          std::string ov = evalA(*cp.iterable);
          out_ << "    " << it << " = " << ov << ";\n";
          out_ << "    if (" << it << ".f == 2) {\n";
          out_ << "      a_obj *_o" << ti << " = (a_obj *)" << it << ".i;\n";
          out_ << "      for (a_i64 _k" << ti << " = 0; _k" << ti << " < _o" << ti
               << "->len; ++_k" << ti << ") {\n";
          out_ << "        " << vn << " = a_index(" << it << ", (aval){.i=_k" << ti
               << ",.f=0});\n";
        }
        std::string indent = isRange ? "      " : "        ";
        if (cp.condition) {
          std::string cv = evalA(*cp.condition);
          out_ << indent << "if (!(" << cv << ".f == 1 ? " << cv << ".d != 0.0 : " << cv
               << ".i != 0)) continue;\n";
        }
        std::string ev = evalA(*cp.element);
        out_ << indent << "(void)a_append(" << res << ", " << ev << ");\n";
        out_ << (isRange ? "    }\n" : "      }\n    }\n");
        out_ << "  }\n";
        return res;
      }
      case ExprKind::MapLit: {
        const auto& m = static_cast<const MapLitExpr&>(e);
        int ti = tmpTop_++;
        std::string tn = "_t" + std::to_string(ti);
        out_ << "  " << tn << " = a_dict_new();\n";
        for (const auto& kv : m.entries) {
          std::string kvA = evalA(*kv.first);
          std::string vvA = evalA(*kv.second);
          out_ << "  a_dict_set(" << tn << ", " << kvA << ", " << vvA << ");\n";
        }
        return tn;
      }
      case ExprKind::Index: {
        const auto& x = static_cast<const IndexExpr&>(e);
        std::string ov = evalA(*x.object);
        std::string iv = evalA(*x.index);
        std::string tn = tmp();
        out_ << "  " << tn << " = a_index(" << ov << ", " << iv << ");\n";
        return tn;
      }
      case ExprKind::Binary: {
        const auto& b = static_cast<const BinaryExpr&>(e);
        // Short-circuit operators evaluate the RHS only when the LHS
        // demands it (interpreter semantics: no side effects, no
        // spurious division-by-zero, on the skipped side).
        if (b.op == "and" || b.op == "or") {
          std::string lt = tmp();
          out_ << "  " << lt << " = " << evalA(*b.lhs) << ";\n";
          out_ << "  " << lt << ".f = 4; " << lt << ".d = 0;\n";
          if (b.op == "and") {
            out_ << "  if (" << lt << ".i != 0) {\n";
            out_ << "    " << lt << " = " << evalA(*b.rhs) << "; " << lt << ".f = 4;\n";
            out_ << "  } else { " << lt << ".i = 0; }\n";
            out_ << "  if (" << lt << ".i != 0) " << lt << ".i = 1; else " << lt << ".i = 0;\n";
          } else {
            out_ << "  if (" << lt << ".i == 0) {\n";
            out_ << "    " << lt << " = " << evalA(*b.rhs) << "; " << lt << ".f = 4;\n";
            out_ << "  }\n";
            out_ << "  if (" << lt << ".i != 0) " << lt << ".i = 1; else " << lt << ".i = 0;\n";
          }
          return lt;
        }
        std::string l = evalA(*b.lhs);
        std::string r = evalA(*b.rhs);
        return avalBin(b.op, l, r, exprType(e));
      }
      case ExprKind::Unary: {
        const auto& u = static_cast<const UnaryExpr&>(e);
        // Postfix '!' is unwrap: error on None (f==5) like the interpreter.
        if (u.postfix && u.op == "!") {
          std::string tn = tmp();
          out_ << "  " << tn << " = " << evalA(*u.operand) << ";\n";
          out_ << "  if (" << tn << ".f == 5) { fprintf(stderr, \"error: Cannot unwrap None with '!'.\\n\"); exit(2); }\n";
          return tn;
        }
        std::string o = evalA(*u.operand);
        VT t = exprType(e);
        if (u.op == "-") {
          if (t == VT::Float) return "(aval){.d=(-ad(" + o + ")), .f=1}";
          if (t == VT::Int) return "(aval){.i=(-ai(" + o + ")), .f=0}";
          std::string tn = tmp();
          out_ << "  " << tn << " = " << o << ";\n";
          out_ << "  if (" << tn << ".f == 1) " << tn << ".d = -" << tn << ".d; else " << tn << ".i = -" << tn << ".i;\n";
          return tn;
        }
        if (u.op == "~")
          return "(aval){.i=(~ai(" + o + ")), .f=0}";
        if (u.op == "not" || u.op == "!")
          return "(aval){.i=(!ai(" + o + ")), .f=4}";
        return "(aval){.i=0,.f=0}";
      }
      case ExprKind::Call: {
        const auto& c = static_cast<const CallExpr&>(e);
        if (c.callee->kind == ExprKind::Field) {
          const auto& f = static_cast<const FieldExpr&>(*c.callee);
          if (f.field == "append" && c.args.size() == 1) {
            std::string ov = evalA(*f.object);
            std::string av = evalA(*c.args[0].value);
            std::string tn = tmp();
            out_ << "  " << tn << " = a_append(" << ov << ", " << av << ");\n";
            return tn;
          }
          return "(aval){.i=0,.f=0}";
        }
        if (c.callee->kind != ExprKind::Ident) return "(aval){.i=0,.f=0}";
        std::string name = cIdent(static_cast<const IdentExpr&>(*c.callee).name);
        if (name == "print" || name == "eprint") {
          emitPrintCall(c, name == "eprint");
          return "(aval){.i=0,.f=0}";
        }
        if (name == "open" && !c.args.empty()) {
          std::string p = evalA(*c.args[0].value);
          std::string pn = tmp();
          out_ << "  " << pn << " = " << p << ";\n";
          std::string modeLit = "\"w\"";
          if (c.args.size() >= 2 && c.args[1].value->kind == ExprKind::StringLit) {
            modeLit = "\"" + escapeC(detail::extractString(static_cast<const StringLitExpr&>(*c.args[1].value))) + "\"";
            out_ << "  return (aval){.i=a_open((char *)((a_obj *)" << pn << ".i)->data, " << modeLit << "),.f=0};\n";
          } else if (c.args.size() >= 2) {
            std::string m = evalA(*c.args[1].value);
            std::string mn = tmp();
            out_ << "  " << mn << " = " << m << ";\n";
            out_ << "  return (aval){.i=a_open((char *)((a_obj *)" << pn << ".i)->data, (char *)((a_obj *)" << mn << ".i)->data),.f=0};\n";
          } else {
            out_ << "  return (aval){.i=a_open((char *)((a_obj *)" << pn << ".i)->data, " << modeLit << "),.f=0};\n";
          }
          return "(aval){.i=0,.f=0}";
        }
        if (name == "close" && !c.args.empty()) {
          std::string h = evalA(*c.args[0].value);
          std::string tn = tmp();
          out_ << "  " << tn << " = " << h << ";\n";
          out_ << "  a_close(" << tn << ".i);\n";
          return "(aval){.i=0,.f=0}";
        }
        if (name == "int" && !c.args.empty()) {
          std::string av = evalA(*c.args[0].value);
          std::string tn = tmp();
          out_ << "  " << tn << " = a_strtoi(" << av << ");\n";
          return tn;
        }
        if (name == "len" && !c.args.empty()) {
          std::string av = evalA(*c.args[0].value);
          std::string tn = tmp();
          out_ << "  " << tn << " = a_lenof(" << av << ");\n";
          return tn;
        }
        if (name == "str" && !c.args.empty()) {
          std::string av = evalA(*c.args[0].value);
          std::string tn = tmp();
          out_ << "  " << tn << " = a_strof(" << av << ");\n";
          return tn;
        }
        if (name == "join" && c.args.size() >= 2) {
          std::string a0 = evalA(*c.args[0].value);
          std::string a1 = evalA(*c.args[1].value);
          std::string tn = tmp();
          out_ << "  " << tn << " = a_join(" << a0 << ", " << a1 << ");\n";
          return tn;
        }
        if ((name == "map" || name == "filter") && c.args.size() >= 2) {
          std::string a0 = evalA(*c.args[0].value);
          std::string a1 = evalA(*c.args[1].value);
          std::string tn = tmp();
          out_ << "  " << tn << " = a_" << name << "(" << a0 << ", " << a1 << ");\n";
          return tn;
        }
        if (name == "sum" && !c.args.empty()) {
          std::string a0 = evalA(*c.args[0].value);
          std::string tn = tmp();
          out_ << "  " << tn << " = a_sum(" << a0 << ");\n";
          return tn;
        }
        if (name == "abs" && !c.args.empty()) {
          std::string a0 = evalA(*c.args[0].value);
          std::string tn = tmp();
          out_ << "  " << tn << " = a_abs(" << a0 << ");\n";
          return tn;
        }
        if ((name == "sorted" || name == "reversed") && !c.args.empty()) {
          std::string a0 = evalA(*c.args[0].value);
          std::string tn = tmp();
          out_ << "  " << tn << " = a_" << name << "(" << a0 << ");\n";
          return tn;
        }
        if ((name == "min" || name == "max") && c.args.size() >= 2) {
          std::string acc = evalA(*c.args[0].value);
          for (std::size_t i = 1; i < c.args.size(); ++i) {
            std::string av = evalA(*c.args[i].value);
            std::string tn = tmp();
            out_ << "  " << tn << " = a_minmax(" << acc << ", " << av << ", "
                 << (name == "max" ? 1 : 0) << ");\n";
            acc = tn;
          }
          return acc;
        }
        if ((name == "f64" || name == "float") && !c.args.empty()) {
          std::string av = evalA(*c.args[0].value);
          std::string tn = tmp();
          out_ << "  " << tn << " = a_strtod_aval(" << av << ");\n";
          return tn;
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

std::string CEmitter::evalI(const Expr& e)  {
    std::string av = evalA(e);
    std::string tn = tmp();
    out_ << "  " << tn << " = " << av << ";\n";
    return "ai(" + tn + ")";
  
}

std::string CEmitter::evalD(const Expr& e)  {
    std::string av = evalA(e);
    std::string tn = tmp();
    out_ << "  " << tn << " = " << av << ";\n";
    return "ad(" + tn + ")";
  
}

std::string CEmitter::floatLit(double v)  {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    std::string s = buf;
    if (s.find_first_of(".eE") == std::string::npos) s += ".0";
    return s;
  
}

std::string CEmitter::identA(const std::string& rawName)  {
    std::string n = cIdent(rawName);
    if (locals_.count(n)) return "v_" + n;
    if (globalNames_.count(n)) return "g_" + n;
    // Stream handles 1/2 agree with the interpreter's stdout/stderr.
    if (rawName == "stdout") return "(aval){.i=1,.f=0}";
    if (rawName == "stderr") return "(aval){.i=2,.f=0}";
    // Undeclared names (e.g. comprehension vars bound in an inner block)
    // still resolve to v_<n>; genuinely undefined names fail C compilation
    // and fall back to the interpreter.
    return "v_" + n;
  
}

std::string CEmitter::lvalue(const std::string& rawName)  {
    std::string n = cIdent(rawName);
    if (locals_.count(n)) return "v_" + n;
    if (globalNames_.count(n)) return "g_" + n;
    return "v_" + n;
  
}

std::string CEmitter::tmp()  {
 return "_t" + std::to_string(tmpTop_++); 
}

std::string CEmitter::escapeC(const std::string& s)  {
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

}  // namespace a
