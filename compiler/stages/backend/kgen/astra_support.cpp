#include "backend/kgen/astra_support.hpp"

#include <unordered_map>
#include <unordered_set>
#include <set>
#include <string>
#include <vector>

#include "backend/kgen/codegen/emitter_common.hpp"

#include "name-resolution/native_modules.hpp"

namespace a::detail {
// stdlib/prelude modules (stdlib/**, incl. the auto-loaded prelude) use
// language features the native subset cannot lower; they are never emitted,
// so they must not gate user programs either.
bool checkSupportedExpr(const Expr& expr);
bool checkSupportedStmt(const Stmt& stmt, bool isGlobal = false) {
  auto fail = [&](const char* what) {
    if (std::getenv("A_DEBUG_NATIVE"))
      std::fprintf(stderr, "[native-gate] unsupported %s (stmt kind %d, global %d)\n",
                   what, static_cast<int>(stmt.kind), static_cast<int>(isGlobal));
    return false;
  };
  switch (stmt.kind) {
    case StmtKind::Let: {
      const auto& s = static_cast<const LetStmt&>(stmt);
      return !s.init || checkSupportedExpr(*s.init);
    }
    case StmtKind::Assign: {
      const auto& s = static_cast<const AssignStmt&>(stmt);
      if (s.target->kind == ExprKind::Index) {
        const auto& ix = static_cast<const IndexExpr&>(*s.target);
        return checkSupportedExpr(*ix.object) && checkSupportedExpr(*ix.index) &&
               checkSupportedExpr(*s.value);
      }
      if (s.target->kind == ExprKind::Field) {
        const auto& fd = static_cast<const FieldExpr&>(*s.target);
        return checkSupportedExpr(*fd.object) && checkSupportedExpr(*s.value);
      }
      return s.target->kind == ExprKind::Ident && checkSupportedExpr(*s.value);
    }
    case StmtKind::Expr: {
      const auto& s = static_cast<const ExprStmt&>(stmt);
      return checkSupportedExpr(*s.expr);
    }
    case StmtKind::Return: {
      const auto& s = static_cast<const ReturnStmt&>(stmt);
      return !s.value || checkSupportedExpr(*s.value);
    }
    case StmtKind::If: {
      const auto& s = static_cast<const IfStmt&>(stmt);
      for (const auto& br : s.branches) {
        if (!checkSupportedExpr(*br.cond)) return false;
        for (const auto& st : br.body) if (!checkSupportedStmt(*st, false)) return false;
      }
      for (const auto& st : s.elseBody) if (!checkSupportedStmt(*st, false)) return false;
      return true;
    }
    case StmtKind::While: {
      const auto& s = static_cast<const WhileStmt&>(stmt);
      if (!checkSupportedExpr(*s.cond)) return false;
      for (const auto& st : s.body) if (!checkSupportedStmt(*st, false)) return false;
      return true;
    }
    case StmtKind::For: {
      const auto& s = static_cast<const ForStmt&>(stmt);
      if (s.iterable->kind == ExprKind::Range) {
        const auto& r = static_cast<const RangeExpr&>(*s.iterable);
        if (!checkSupportedExpr(*r.start) || !checkSupportedExpr(*r.end)) return false;
        if (r.step && !checkSupportedExpr(*r.step)) return false;
      } else if (!checkSupportedExpr(*s.iterable)) {
        return false;  // lists/strings iterate via a_index in the C emitter
      }
      // Scoped shadowing check: if any nested statement declares a variable with loop var's name,
      // fallback to interpreter
      std::vector<const Stmt*> queue;
      for (const auto& st : s.body) queue.push_back(st.get());
      while (!queue.empty()) {
        const Stmt* cur = queue.back();
        queue.pop_back();
        if (cur->kind == StmtKind::Let) {
          const auto& let = static_cast<const LetStmt&>(*cur);
          if (let.name == s.var) return false;
        } else if (cur->kind == StmtKind::If) {
          const auto& ifs = static_cast<const IfStmt&>(*cur);
          for (const auto& br : ifs.branches) {
            for (const auto& st : br.body) queue.push_back(st.get());
          }
          for (const auto& st : ifs.elseBody) queue.push_back(st.get());
        } else if (cur->kind == StmtKind::While) {
          const auto& wh = static_cast<const WhileStmt&>(*cur);
          for (const auto& st : wh.body) queue.push_back(st.get());
        }
      }
      for (const auto& st : s.body) {
        if (!checkSupportedStmt(*st, false)) return false;
      }
      return true;
    }
    case StmtKind::Break:
    case StmtKind::Continue:
      return true;  // emitted as C break/continue inside loop bodies
    case StmtKind::Loop: {
      const auto& s = static_cast<const LoopStmt&>(stmt);
      for (const auto& st : s.body) if (!checkSupportedStmt(*st, false)) return false;
      return true;
    }
    case StmtKind::DoWhile: {
      const auto& s = static_cast<const DoWhileStmt&>(stmt);
      if (!checkSupportedExpr(*s.cond)) return false;
      for (const auto& st : s.body) if (!checkSupportedStmt(*st, false)) return false;
      return true;
    }
    case StmtKind::Destructure: {
      const auto& s = static_cast<const DestructureStmt&>(stmt);
      return checkSupportedExpr(*s.value);
    }
    case StmtKind::Defer: {
      const auto& s = static_cast<const DeferStmt&>(stmt);
      return !s.body || checkSupportedStmt(*s.body, false);
    }
    case StmtKind::Import:
    case StmtKind::FromImport:
      return true;  // module bindings stay in the interpreter; stdlib modules are never emitted
    case StmtKind::StructDecl:
    case StmtKind::EnumDecl:
    case StmtKind::TypeAlias:
    case StmtKind::TraitDecl:
    case StmtKind::ImplDecl:
      return true;  // type declarations lower to nothing; methods checked via FnDecl where emitted
    case StmtKind::FnDecl: {
      const auto& fn = static_cast<const FnDecl&>(stmt);
      // If function assigns to a global (defined at top-level), fallback (globals not yet implemented in Astra native without global storage)
      // Collect globals from graph not possible here without graph ref; instead: if function has Assign to a name that is not a param and not Let in same fn, treat as unsupported.
      // We approximate by checking: any Assign target name that never appears as Let or param in fn is a global mutation.
      {
        std::set<std::string> localNames;
        for (auto& prm: fn.params) localNames.insert(prm.name);
        // collect Lets recursively via stack
        std::vector<const Stmt*> q; for (auto& s: fn.body) q.push_back(s.get());
        while(!q.empty()){auto* cur=q.back(); q.pop_back();
          if(cur->kind==StmtKind::Let){ localNames.insert(static_cast<const LetStmt&>(*cur).name); }
          // In A's Python-style semantics the first `name = value` inside a
          // function declares a local, so assign targets count as locals too.
          else if(cur->kind==StmtKind::Assign){ auto& a=static_cast<const AssignStmt&>(*cur); if(a.target->kind==ExprKind::Ident) localNames.insert(static_cast<const IdentExpr&>(*a.target).name); }
          else if(cur->kind==StmtKind::If){ auto& ifs=static_cast<const IfStmt&>(*cur); for(auto& br: ifs.branches) for(auto& s: br.body) q.push_back(s.get()); for(auto& s: ifs.elseBody) q.push_back(s.get()); }
          else if(cur->kind==StmtKind::While){ auto& wh=static_cast<const WhileStmt&>(*cur); for(auto& s: wh.body) q.push_back(s.get()); }
          else if(cur->kind==StmtKind::For){ auto& fo=static_cast<const ForStmt&>(*cur); localNames.insert(fo.var); for(auto& s: fo.body) q.push_back(s.get()); }
        }
        // now check assigns
        std::vector<const Stmt*> q2; for (auto& s: fn.body) q2.push_back(s.get());
        while(!q2.empty()){auto* cur=q2.back(); q2.pop_back();
          if(cur->kind==StmtKind::Assign){ auto& a=static_cast<const AssignStmt&>(*cur); if(a.target->kind==ExprKind::Ident){ auto& id=static_cast<const IdentExpr&>(*a.target); if(localNames.count(id.name)==0) return false; } }
          else if(cur->kind==StmtKind::If){ auto& ifs=static_cast<const IfStmt&>(*cur); for(auto& br: ifs.branches) for(auto& s: br.body) q2.push_back(s.get()); for(auto& s: ifs.elseBody) q2.push_back(s.get()); }
          else if(cur->kind==StmtKind::While){ auto& wh=static_cast<const WhileStmt&>(*cur); for(auto& s: wh.body) q2.push_back(s.get()); }
          else if(cur->kind==StmtKind::For){ auto& fo=static_cast<const ForStmt&>(*cur); for(auto& s: fo.body) q2.push_back(s.get()); }
        }
      }
      for (const auto& st : fn.body) if (!checkSupportedStmt(*st, false)) return false;
      return true;
    }
    default:
      return fail("stmt");
  }
}

// True when the expression is provably a double at runtime (contains a float
// literal or a float builtin), making / and % safe from integer div-by-zero.
bool exprProvablyFloat(const Expr& expr) {
  switch (expr.kind) {
    case ExprKind::FloatLit:
      return true;
    case ExprKind::Binary: {
      const auto& b = static_cast<const BinaryExpr&>(expr);
      static const std::set<std::string> arith = {"+","-","*","/","%"};
      return arith.count(b.op) &&
             (exprProvablyFloat(*b.lhs) || exprProvablyFloat(*b.rhs));
    }
    case ExprKind::Unary:
      return static_cast<const UnaryExpr&>(expr).op == "-" &&
             exprProvablyFloat(*static_cast<const UnaryExpr&>(expr).operand);
    case ExprKind::Call: {
      const auto& c = static_cast<const CallExpr&>(expr);
      if (c.callee->kind != ExprKind::Ident) return false;
      static const std::set<std::string> fns = {
          "sqrt","pow","fmod","floor","ceil","round","exp","log","sin","cos",
          "tan","fabs","f64","float"};
      return fns.count(static_cast<const IdentExpr&>(*c.callee).name) != 0;
    }
    default:
      return false;
  }
}

bool checkSupportedExpr(const Expr& expr) {
  switch (expr.kind) {
    case ExprKind::IntLit:
    case ExprKind::FloatLit:
    case ExprKind::BoolLit:
    case ExprKind::CharLit:
      return true;
    case ExprKind::Ident: {
      return true;  // None/stdout/stderr/globals all have emitter mappings
    }
    case ExprKind::Range: {
      const auto& r = static_cast<const RangeExpr&>(expr);
      return checkSupportedExpr(*r.start) && checkSupportedExpr(*r.end) && (!r.step || checkSupportedExpr(*r.step));
    }
    case ExprKind::Ternary: {
      const auto& x = static_cast<const TernaryExpr&>(expr);
      return checkSupportedExpr(*x.cond) && checkSupportedExpr(*x.thenValue) && checkSupportedExpr(*x.elseValue);
    }
    case ExprKind::Tuple: {
      const auto& a = static_cast<const TupleExpr&>(expr);
      for (const auto& el : a.elements) if (!checkSupportedExpr(*el)) return false;
      return true;
    }
    case ExprKind::Slice: {
      const auto& x = static_cast<const SliceExpr&>(expr);
      return checkSupportedExpr(*x.object)
             && (!x.start || checkSupportedExpr(*x.start))
             && (!x.end || checkSupportedExpr(*x.end))
             && (!x.step || checkSupportedExpr(*x.step));
    }
    case ExprKind::Field: {
      const auto& x = static_cast<const FieldExpr&>(expr);
      return checkSupportedExpr(*x.object);
    }
    case ExprKind::ArrayLit: {
      const auto& a = static_cast<const ArrayLitExpr&>(expr);
      for (const auto& el : a.elements)
        if (!checkSupportedExpr(*el)) return false;
      return true;
    }
    case ExprKind::Index: {
      const auto& x = static_cast<const IndexExpr&>(expr);
      return checkSupportedExpr(*x.object) && checkSupportedExpr(*x.index);
    }
    case ExprKind::Comprehension: {
      const auto& cp = static_cast<const ComprehensionExpr&>(expr);
      if (cp.isSet) return false;  // sets need a hash runtime
      return checkSupportedExpr(*cp.element) && checkSupportedExpr(*cp.iterable) &&
             (!cp.condition || checkSupportedExpr(*cp.condition));
    }
    case ExprKind::MapLit: {
      const auto& m = static_cast<const MapLitExpr&>(expr);
      for (const auto& kv : m.entries)
        if (!checkSupportedExpr(*kv.first) || !checkSupportedExpr(*kv.second))
          return false;
      return true;
    }
    case ExprKind::Lambda: {
      const auto& l = static_cast<const LambdaExpr&>(expr);
      return checkSupportedExpr(*l.body);
    }
    case ExprKind::StringLit: {
      // Static + interpolated literals both lower via a_cat/a_strof in the C emitter.
      const auto& sl = static_cast<const StringLitExpr&>(expr);
      for (const auto& part : sl.parts) if (part.isExpr) {
        if (!part.expr || !checkSupportedExpr(*part.expr)) return false;
      }
      return true;
    }
    case ExprKind::Binary: {
      const auto& b = static_cast<const BinaryExpr&>(expr);
      // Division/remainder are safe natively: the C runtime helpers check
      // for zero divisors and exit(2) like the interpreter.
      // Shifts are checked at run time by a_shift (out-of-range -> error),
      // so variable shift counts are natively supported.
      return checkSupportedExpr(*b.lhs) && checkSupportedExpr(*b.rhs);
    }
    case ExprKind::Unary: {
      const auto& u = static_cast<const UnaryExpr&>(expr);
      return checkSupportedExpr(*u.operand);
    }
    case ExprKind::Call: {
      const auto& c = static_cast<const CallExpr&>(expr);
      if (c.callee->kind == ExprKind::Field) {
        // Method calls: allow list.append natively; allow module/member calls
        // (obj.field(...)) to pass the gate so the Astra backend can attempt them.
        const auto& f = static_cast<const FieldExpr&>(*c.callee);
        if (f.field == "append" && c.args.size() == 1)
          return checkSupportedExpr(*f.object) && checkSupportedExpr(*c.args[0].value);
        if (!checkSupportedExpr(*f.object)) return false;
        for (const auto& a : c.args) if (!checkSupportedExpr(*a.value)) return false;
        return true;
      }
      if (c.callee->kind != ExprKind::Ident) return false;
      const std::string& name = static_cast<const IdentExpr&>(*c.callee).name;
      // Standard-library builtins (json, binary, testing, log, metrics,
      // trace, net, http) only exist in the interpreter.
      if (a::isStdlibBuiltin(name)) return false;
      // print/eprint/int/float/f64/i64/open/close all lower via the C emitter helpers.
      if (name == "open" || name == "close") {
        for (const auto& a : c.args) if (!checkSupportedExpr(*a.value)) return false;
        return true;
      }
      if (name == "print" || name == "eprint") {
        for (const auto& a : c.args) if (!checkSupportedExpr(*a.value)) return false;
        return true;
      }
      if (name == "int" || name == "float" || name == "f64" || name == "i64") {
        for (const auto& a : c.args) if (!checkSupportedExpr(*a.value)) return false;
        return true;
      }
      // Remaining runtime/stdlib surface needs the interpreter.
      if (name == "write_file" || name == "read_file" || name == "append_file" ||
          name == "file_exists" || name == "remove_file" || name == "flush" ||
          name == "input" || name == "read_line" ||
          name == "set" ||
          name == "unsafe_get" || name == "unsafe_set" || name == "move" ||
          name == "arena_new" || name == "arena_pool" || name == "arena_region" ||
          name == "arena_alloc" || name == "arena_release" || name == "arena_reset" ||
          name == "arena_used" || name == "arena_total" || name == "copy" ||
          name == "borrow" || name == "borrow_mut" || name == "unsafe_address_of" ||
          name == "unsafe_load" || name == "unsafe_store" || name == "unsafe_offset") {
        return false;
      }
      if (name == "ic" || name == "__file__" ||
          name == "__line__" || name == "__func__" || name == "__function__" ||
          name == "__source_file__" || name == "__source_line__" || name == "__repr__" ||
          name == "__debug_repr__" || name == "__debug_time__" || name == "__debug_context__" ||
          name == "__ic_format__") {
        // Debug intrinsics have interpreter-only formatting/context semantics.
        return false;
      }
      for (const auto& a : c.args) if (!checkSupportedExpr(*a.value)) return false;
      return true;
    }
    default:
      return false;
  }
}
bool isNativeSupported(const ModuleGraph& graph) {
  if (graph.modules.empty()) return false;
  for (const auto& mod : graph.modules) {
    if (!mod.program) continue;
    for (const auto& st : mod.program->statements) {
      if (st->kind == StmtKind::StructDecl || st->kind == StmtKind::ImplDecl) {
        if (std::getenv("A_DEBUG_NATIVE"))
          std::fprintf(stderr, "[native-gate] unsupported type declaration\n");
        return false;
      }
    }
  }
  for (const auto& mod : graph.modules) {
    if (!mod.program || detail::isStdlibModule(graph,mod)) continue;
    for (const auto& st : mod.program->statements) {
      if (!checkSupportedStmt(*st, true)) return false;
    }
  }
  return true;
}

}  // namespace a::detail

namespace a {
bool isNativeSupported(const ModuleGraph& graph) { return detail::isNativeSupported(graph); }
}  // namespace a
