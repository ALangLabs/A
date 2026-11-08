#include "codegen/codegen.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast/ast.hpp"
#include "codegen/codegen_internal.hpp"
#include "name-resolution/module_loader.hpp"
#include "name-resolution/native_modules.hpp"
#include "type-checker/sema.hpp"
#include "type-checker/method_signatures.hpp"
#include "type-checker/types.hpp"

namespace a {
namespace {

// Runtime builtin ids; order must match the enum in a_runtime.h.
int builtinRuntimeId(const std::string& name) {
  static const std::unordered_map<std::string, int> kMap = {
      {"print", 0}, {"eprint", 1}, {"len", 2}, {"str", 3}, {"int", 4},
      {"float", 5}, {"f64", 5}, {"i64", 4}, {"bool", 6}, {"abs", 7},
      {"min", 8}, {"max", 9}, {"sum", 10}, {"pow", 11}, {"sqrt", 12},
      {"sin", 13}, {"cos", 14}, {"tan", 15}, {"asin", 16}, {"acos", 17},
      {"atan", 18}, {"exp", 19}, {"log", 20}, {"floor", 21}, {"ceil", 22},
      {"round", 23}, {"trunc", 24}, {"random", 25}, {"random_int", 26},
      {"range", 27}, {"set", 28}, {"tuple", 29}, {"list", 30}, {"dict", 31},
      {"map", 32}, {"filter", 33}, {"any", 34}, {"all", 35}, {"some", 36},
      {"ok", 37}, {"err", 38}, {"panic", 39}, {"assert", 40},
      {"assert_eq", 41}, {"input", 42}, {"read_file", 43},
      {"write_file", 44}, {"append_file", 45}, {"file_exists", 46},
      {"list_dir", 47}, {"remove_file", 48}, {"create_dir", 49},
      {"remove_dir", 50}, {"rename", 51}, {"current_dir", 52}, {"repr", 53},
      {"type", 54}, {"isinstance", 55}, {"join", 56}, {"sorted", 57},
      {"reversed", 58}, {"enumerate", 59}, {"zip", 60}, {"reduce", 61},
      {"log2", 62}, {"log10", 63}, {"atan2", 64}, {"flush", 65}, {"i8", 66},
      {"i16", 67}, {"i32", 68}, {"u8", 69}, {"u16", 70}, {"u32", 71}, {"f32", 72},
      {"ctz", 73}, {"clz", 74}, {"popcount", 75}, {"bswap", 76}, {"rotl", 77},
      {"rotr", 78}, {"fma", 79}, {"fabs", 80}, {"copysign", 81},
      {"smin", 82}, {"smax", 83}, {"umin", 84}, {"umax", 85},
      {"sadd_overflow", 86}, {"ssub_overflow", 87}, {"smul_overflow", 88},
      {"uadd_overflow", 89}, {"usub_overflow", 90}, {"umul_overflow", 91},
      {"expect", 92}, {"likely", 93}, {"unlikely", 94}, {"env_get", 95},
      {"now", 96}, {"sleep", 97},
      {"terminal_isatty", 98}, {"terminal_raw_mode", 99},
      {"terminal_read_char", 100}, {"terminal_read_char_ms", 101},
      {"terminal_write", 102}, {"terminal_flush", 103},
      {"terminal_size", 104}, {"terminal_home", 105},
      {"repl_eval", 106},
      {"json_parse", 107}, {"json_stringify", 108}, {"json_pretty", 109},
      {"json_is_valid", 110}, {"json_get", 111}, {"json_at", 112},
      {"json_len", 113}, {"json_has", 114}, {"json_keys", 115},
      {"json_type", 116}, {"json_as_str", 117}, {"json_as_int", 118},
      {"json_as_float", 119}, {"json_as_bool", 120},
      {"json_as_array", 121}, {"json_as_map", 122},
      {"repl_quiet", 123},
      {"binary_encode", 124}, {"binary_decode", 125},
      {"log_set_level", 126}, {"log_set_json", 127},
      {"log_debug", 128}, {"log_info", 129}, {"log_warn", 130},
      {"log_error", 131},
      {"metrics_counter", 132}, {"metrics_gauge", 133},
      {"metrics_histogram", 134}, {"metrics_render", 135},
      {"metrics_reset", 136},
      {"trace_start", 137}, {"trace_end", 138}, {"trace_current", 139},
      {"trace_set_attribute", 140}, {"trace_add_event", 141},
      {"trace_inject", 142}, {"trace_extract", 143}, {"trace_spans", 144},
      {"trace_reset", 145},
      {"net_resolve", 146}, {"net_tls_available", 147},
      {"arena_new", 148}, {"arena_pool", 149}, {"arena_region", 150},
      {"arena_alloc", 151}, {"arena_release", 152}, {"arena_reset", 153},
      {"arena_used", 154}, {"arena_total", 155},
      {"unsafe_get", 156}, {"unsafe_set", 157},
      {"unsafe_address_of", 158}, {"unsafe_load", 159},
      {"unsafe_store", 160}, {"unsafe_offset", 161},
      {"py_call", 162}, {"py_buffer", 163},
      {"py_import_module", 164}, {"py_try_import_module", 165},
      {"py_getattr", 166}, {"py_try_getattr", 167},
      {"py_setattr", 168}, {"py_hasattr", 169}, {"py_delattr", 170},
      {"py_getitem", 171}, {"py_setitem", 172}, {"py_delitem", 173},
      {"py_call_obj", 174}, {"py_try_call_obj", 175},
      {"py_call_method", 176}, {"py_try_call_method", 177},
      {"py_is_pyobject", 178}, {"py_str", 179}, {"py_repr", 180},
      {"py_len", 181}, {"py_contains", 182}, {"py_truthy", 183},
      {"py_type", 184}, {"py_buffer_info", 185},
      {"json_set", 186}, {"json_delete", 187},
      {"struct_name", 188}, {"struct_methods", 189},
      {"fn_signature", 190}, {"fn_doc", 191},
      {"file_size", 192},
      {"linalg_dot", 193}, {"linalg_matvec", 194}, {"linalg_matvec_t", 195},
      {"dot", 193}, {"matvec", 194}, {"matvec_t", 195},
      {"linalg_matmul", 219}, {"linalg_softmax", 220}, {"linalg_attention", 221},
      {"matmul", 219}, {"softmax", 220}, {"attention", 221},
      {"net_tcp_connect", 196}, {"net_send", 197}, {"net_recv", 198},
      {"net_close", 199}, {"net_set_timeout", 200}, {"net_local_port", 201},
      {"net_tls_connect", 202},
      {"net_recv_status", 203},
      {"read_bytes", 204}, {"write_bytes", 205},
      {"net_tcp_listen", 206}, {"net_accept", 207},
      {"net_set_nonblocking", 208}, {"net_poll", 209},
      {"net_udp_socket", 210}, {"net_udp_bind", 211},
      {"net_udp_send", 212}, {"net_udp_recv", 213},
      {"net_send_bytes", 214}, {"net_recv_bytes", 215},
      {"net_tls_wrap", 216}, {"net_inflate_bytes", 217},
            {"net_inflate_available", 218},
      {"bytes_decode_utf8", 222}, {"str_encode_utf8", 223},
      {"net_recv_exact", 224},
  };
  auto it = kMap.find(name);
  return it == kMap.end() ? -1 : it->second;
}

std::string cEscape(const std::string& s) {
  std::string out;
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\r': out += "\\r"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\%03o", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  return out;
}

std::string utf8Encode(char32_t cp) {
  std::string out;
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
  return out;
}

struct FnInfo {
  const FnDecl* decl = nullptr;
  std::string cname;
  std::string globalVar;  // aval global holding the FNON value
  std::vector<std::string> paramNames;
  // paramDefaults[i] is the default-value Expr for paramNames[i] (nullptr
  // for comptime params and params without defaults); variadic marks a
  // trailing `*args` param whose slot receives a packed list.
  std::vector<const Expr*> paramDefaults;
  bool variadic = false;
  bool isMethod = false;
  std::string structName;
  int moduleId = -1;
  std::string name;
};

struct StructInfo {
  const StructDecl* decl = nullptr;
  std::string ctorCname;
  std::string globalVar;
  std::vector<std::string> fieldNames;
  std::unordered_map<std::string, FnInfo*> methods;
  int moduleId = -1;
  std::string name;
};

struct EnumVariantInfo {
  std::string globalVar;
  std::int64_t value = 0;
  bool algebraic = false;
  int tag = 0;
  std::vector<std::string> payloadFields;
};

enum class BKind { Local, Global, Param, Capture, FnValue, Builtin, StructCtor, EnumValue, Module, PyModule, PyFunc, NativeConst };

struct Binding {
  BKind kind = BKind::Local;
  std::string cname;      // C variable / global name
  int envIdx = -1;        // Capture
  int builtinId = -1;     // Builtin (runtime id)
  FnInfo* fn = nullptr;   // FnValue / StructCtor (via StructCtorInfo)
  StructInfo* st = nullptr;
  FnInfo* dropFn = nullptr;  // struct drop method for RAII locals
  EnumVariantInfo* en = nullptr;
  int moduleId = -1;      // Module
  std::string aux;        // native module name / python module (mirrors ABinding::aux)
  std::string pyMember;   // PyFunc: python member name
};

struct LambdaJob {
  const LambdaExpr* decl = nullptr;
  std::string cname;
  std::vector<std::pair<std::string, Binding>> captures;  // name -> outer binding
  int moduleId = -1;  // module owning the lambda (for traceback file info)
};

class Emitter {
 public:
  Emitter(const ModuleGraph& graph, const SemaResult& sema, bool testMode = false)
      : graph_(graph), sema_(sema), testMode_(testMode) {}

  bool run(std::string& out, std::string& error) {
    error_ = &error;
    collectGlobals();
    buildGlobalScope();
    header_ << "#include \"a_runtime.h\"\n#include <stdint.h>\n#include <stddef.h>\n";
    if (testMode_) {
      header_ << "#include <stdio.h>\n#include <string.h>\n#include <sys/wait.h>\n#include <unistd.h>\n#include <fcntl.h>\n";
    }
    // Forward declarations for all functions.
    for (auto& kv : fns_) {
      FnInfo* f = kv.second;
      header_ << "aval " << f->cname << "(aval *argv);\n";
    }
    for (StructInfo* st : structList_) {
      header_ << "aval " << st->ctorCname << "(aval *argv);\n";
    }
    // Globals.
    for (auto& g : globals_) header_ << "aval " << g << ";\n";
    for (FnInfo* f : fnList_) {
      if (!f->isMethod) header_ << "aval " << f->globalVar << ";\n";
    }
    for (StructInfo* st : structList_) header_ << "aval " << st->globalVar << ";\n";
    for (auto& kv : enums_) {
      if (!kv.second.algebraic) header_ << "aval " << kv.second.globalVar << ";\n";
    }
    body_ << "int main(int argc, char **argv, char **envp);\n";
    // Struct constructors and methods, then functions.
    for (StructInfo* st : structList_) emitStructCtor(st);
    for (auto& kv : fns_) {
      if (!failed_) emitFunction(kv.second);
    }
    // Main: initialize fn/struct/enum globals, then top-level statements of
    // every module in dependency order (imports first, entry last).
    emitMain();
    // Lambdas queued during function/main emission.
    std::string lambdas;
    while (!lambdaQueue_.empty() && !failed_) {
      LambdaJob job = std::move(lambdaQueue_.front());
      lambdaQueue_.erase(lambdaQueue_.begin());
      lambdas += emitLambda(job);
    }
    if (failed_) return false;
    out = header_.str() + lambdas + body_.str();
    return true;
  }

 private:
  const ModuleGraph& graph_;
  const SemaResult& sema_;
  std::string* error_ = nullptr;
  bool failed_ = false;
  bool testMode_ = false;
  std::ostringstream header_;
  std::ostringstream body_;
  int counter_ = 0;
  int tempCounter_ = 0;

  std::unordered_map<const FnDecl*, FnInfo*> fns_;
  std::vector<FnInfo*> fnList_;
  std::unordered_map<std::string, StructInfo*> structs_;  // by short name
  std::vector<StructInfo*> structList_;
  std::unordered_map<std::string, EnumVariantInfo> enums_;  // "Enum.Variant"
  std::vector<std::string> globals_;  // top-level let globals (C names)
  std::unordered_map<std::string, std::string> topLevelGlobals_;  // module:name -> cname

  std::vector<std::unordered_map<std::string, Binding>> scopes_;
  std::vector<std::vector<std::string>> scopeDeclOrder_;
  std::string pre_;  // statements hoisted before the current statement
  std::vector<std::vector<const Stmt*>> deferStack_;
  std::vector<int> loopDeferDepth_;
  std::vector<LambdaJob> lambdaQueue_;
  std::string curStruct_;  // struct whose method is being emitted (for `self` type)
  int curModuleId_ = -1;
  bool inFunction_ = false;

  std::string fresh(const std::string& prefix) {
    return "__a_" + prefix + std::to_string(counter_++);
  }
  std::string temp(const std::string& prefix = "t") {
    return "__a_" + prefix + std::to_string(tempCounter_++);
  }
  void fail(const std::string& msg) {
    if (!failed_) {
      failed_ = true;
      if (error_) *error_ = msg;
    }
  }
  void pre(const std::string& stmt) { pre_ += stmt; if (!stmt.empty() && stmt.back() != '\n') pre_ += "\n"; }

  // ---- registration -------------------------------------------------------
  void collectGlobals() {
    int fnId = 0, stId = 0;
    for (const Module& mod : graph_.modules) {
      if (!mod.program) continue;
      for (const StmtPtr& s : mod.program->statements) {
        if (s->kind == StmtKind::FnDecl) {
          const FnDecl* fn = static_cast<const FnDecl*>(s.get());
          FnInfo* info = new FnInfo();
          info->decl = fn;
          info->name = fn->name;
          info->moduleId = mod.id;
          info->cname = "__a_f" + std::to_string(fnId++);
          info->globalVar = "__a_gf" + std::to_string(fnId);
          for (const ComptimeParam& p : fn->comptimeParams) { info->paramNames.push_back(p.name); info->paramDefaults.push_back(nullptr); }
          for (const ParamDecl& p : fn->params) { info->paramNames.push_back(p.name); info->paramDefaults.push_back(p.defaultValue.get()); info->variadic = info->variadic || p.variadic; }
          fns_[fn] = info;
          fnList_.push_back(info);
        } else if (s->kind == StmtKind::StructDecl) {
          const StructDecl* st = static_cast<const StructDecl*>(s.get());
          StructInfo* info = new StructInfo();
          info->decl = st;
          info->name = st->name;
          info->moduleId = mod.id;
          info->ctorCname = "__a_ctor" + std::to_string(stId);
          info->globalVar = "__a_gs" + std::to_string(stId++);
          for (const FieldDecl& f : st->fields) info->fieldNames.push_back(f.name);
          structs_[st->name] = info;
          structList_.push_back(info);
          for (const auto& m : st->methods) {
            FnInfo* mi = new FnInfo();
            mi->decl = m.get();
            mi->name = m->name;
            mi->moduleId = mod.id;
            mi->isMethod = true;
            mi->structName = st->name;
            mi->cname = "__a_m" + std::to_string(fnId++);
            mi->globalVar = "__a_gf" + std::to_string(fnId);
            for (const ComptimeParam& p : m->comptimeParams) { mi->paramNames.push_back(p.name); mi->paramDefaults.push_back(nullptr); }
            for (const ParamDecl& p : m->params) { mi->paramNames.push_back(p.name); mi->paramDefaults.push_back(p.defaultValue.get()); mi->variadic = mi->variadic || p.variadic; }
            fns_[m.get()] = mi;
            fnList_.push_back(mi);
            info->methods[m->name] = mi;
          }
        } else if (s->kind == StmtKind::EnumDecl) {
          const EnumDecl* e = static_cast<const EnumDecl*>(s.get());
          std::int64_t next = 0;
          int tag = 0;
          for (const EnumVariant& v : e->variants) {
            EnumVariantInfo ev;
            ev.algebraic = e->isAlgebraic();
            ev.tag = tag++;
            if (v.value) {
              // Evaluate simple int literal / unary minus.
              ev.value = evalConstInt(v.value.get(), next);
            } else {
              ev.value = next;
            }
            next = ev.value + 1;
            ev.globalVar = "__a_ev_" + e->name + "_" + v.name + "_" +
                           (ev.value < 0 ? "m" : "") + std::to_string(ev.value < 0 ? -ev.value : ev.value);
            for (const FieldDecl& f : v.fields) ev.payloadFields.push_back(f.name);
            enums_[e->name + "." + v.name] = ev;
          }
        } else if (s->kind == StmtKind::Let) {
          const LetStmt* let = static_cast<const LetStmt*>(s.get());
          std::string cname = "__a_top_" + std::to_string(counter_++);
          globals_.push_back(cname);
          topLevelGlobals_[std::to_string(mod.id) + ":" + let->name] = cname;
        } else if (s->kind == StmtKind::Assign) {
          const AssignStmt* as = static_cast<const AssignStmt*>(s.get());
          if (as->target->kind == ExprKind::Ident) {
            const std::string& nm = static_cast<const IdentExpr*>(as->target.get())->name;
            std::string key = std::to_string(mod.id) + ":" + nm;
            if (!topLevelGlobals_.count(key)) {
              std::string cname = "__a_top_" + std::to_string(counter_++);
              globals_.push_back(cname);
              topLevelGlobals_[key] = cname;
            }
          }
        }
      }
    }
  }

  std::int64_t evalConstInt(const Expr* e, std::int64_t fallback) {
    if (!e) return fallback;
    if (e->kind == ExprKind::IntLit)
      return static_cast<const IntLitExpr*>(e)->value;
    if (e->kind == ExprKind::Unary) {
      const UnaryExpr* u = static_cast<const UnaryExpr*>(e);
      if (u->op == "-") return -evalConstInt(u->operand.get(), 0);
    }
    if (e->kind == ExprKind::Ident) {
      const std::string& n = static_cast<const IdentExpr*>(e)->name;
      auto it = enums_.find(n);
      if (it != enums_.end()) return it->second.value;
      // Enum.Variant handled via FieldExpr below.
    }
    return fallback;
  }

  void buildGlobalScope() {
    scopes_.clear();
    scopeDeclOrder_.clear();
    scopes_.emplace_back();
    scopeDeclOrder_.emplace_back();
    auto& g = scopes_.back();
    // Top-level lets as globals (entry + imported modules).
    for (const Module& mod : graph_.modules) {
      if (!mod.program) continue;
      for (const StmtPtr& s : mod.program->statements) {
        if (s->kind == StmtKind::Let) {
          const LetStmt* let = static_cast<const LetStmt*>(s.get());
          auto it = topLevelGlobals_.find(std::to_string(mod.id) + ":" + let->name);
          if (it != topLevelGlobals_.end()) {
            Binding b; b.kind = BKind::Global; b.cname = it->second;
            g[let->name] = b;
          }
        } else if (s->kind == StmtKind::Assign) {
          const AssignStmt* as = static_cast<const AssignStmt*>(s.get());
          if (as->target->kind == ExprKind::Ident) {
            const std::string& nm = static_cast<const IdentExpr*>(as->target.get())->name;
            auto it = topLevelGlobals_.find(std::to_string(mod.id) + ":" + nm);
            if (it != topLevelGlobals_.end()) {
              Binding b; b.kind = BKind::Global; b.cname = it->second;
              g[nm] = b;
            }
          }
        }
      }
    }
    for (FnInfo* f : fnList_) {
      if (f->isMethod) continue;
      Binding b; b.kind = BKind::FnValue; b.cname = f->globalVar; b.fn = f;
      // First definition wins for duplicate short names across modules.
      if (!g.count(f->name)) g[f->name] = b;
    }
    for (StructInfo* st : structList_) {
      Binding b; b.kind = BKind::StructCtor; b.cname = st->globalVar; b.st = st;
      if (!g.count(st->name)) g[st->name] = b;
    }
    for (auto& kv : enums_) {
      Binding b; b.kind = BKind::EnumValue; b.cname = kv.second.globalVar; b.en = &kv.second;
      g[kv.first] = b;
      // Bare variant name if unambiguous.
      auto dot = kv.first.find('.');
      std::string shortName = kv.first.substr(dot + 1);
      if (!g.count(shortName)) g[shortName] = b;
    }
    // Native modules (std:math, core, ...) are not graph modules; register
    // their names so qualified calls resolve to builtins.
    for (const char* nm : {"math","io","fs","strings","collections","core","time",
                           "json","binary","testing","log","metrics","trace","net",
                           "http","terminal","env","repl","kernels","unsafe","arena","std"}) {
      Binding b; b.kind = BKind::Module; b.moduleId = -1; b.aux = nm;
      g[nm] = b;
    }
    // Module names (stems) for qualified access.
    for (const Module& mod : graph_.modules) {
      Binding b; b.kind = BKind::Module; b.moduleId = mod.id;
      // Never shadow a builtin or an existing binding (e.g. a file named
      // input.a must not turn the `input` builtin into a module).
      if (!g.count(mod.name) && builtinRuntimeId(mod.name) < 0) g[mod.name] = b;
      std::string stem = stemOf(mod.path);
      if (!g.count(stem) && builtinRuntimeId(stem) < 0) g[stem] = b;
    }
  }

  // ---- scope helpers ------------------------------------------------------
  void pushScope() { scopes_.emplace_back(); scopeDeclOrder_.emplace_back(); }
  void popScope() { scopes_.pop_back(); scopeDeclOrder_.pop_back(); }
  const Binding* lookup(const std::string& name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      auto f = it->find(name);
      if (f != it->end()) return &f->second;
    }
    return nullptr;
  }
  Binding* lookupMut(const std::string& name) {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      auto f = it->find(name);
      if (f != it->end()) return &f->second;
    }
    return nullptr;
  }
  void bindLocal(const std::string& name, const std::string& cname, BKind kind = BKind::Local) {
    Binding b; b.kind = kind; b.cname = cname;
    scopes_.back()[name] = b;
    scopeDeclOrder_.back().push_back(name);
  }
  FnInfo* findDrop(const Expr* init) {
    if (!init) return nullptr;
    TypePtr ty = resolveType(init->inferredType);
    if (!ty || ty->kind != TypeKind::Struct) return nullptr;
    auto it = structs_.find(shortName(ty->structName));
    if (it == structs_.end()) return nullptr;
    auto mit = it->second->methods.find("drop");
    return mit == it->second->methods.end() ? nullptr : mit->second;
  }
  void emitDropCall(std::ostringstream& out, const Binding& b) {
    if (!b.dropFn) return;
    std::string arr = temp("argv");
    out << "aval " << arr << "[2]; " << arr << "[0] = A_NONE; " << arr << "[1] = " << b.cname << ";\n";
    out << b.dropFn->cname << "(" << arr << ");\n";
  }

  std::string bindingExpr(const Binding& b) {
    if (b.kind == BKind::Capture) {
      return "a_env_get(__a_env, " + std::to_string(b.envIdx) + ")";
    }
    if (b.kind == BKind::NativeConst) {
      // aux holds the native constant name (PI, E, ...); same lowering as
      // `math.PI` in emitField.
      double v = b.aux == "PI"   ? 3.14159265358979323846
                : b.aux == "E"    ? 2.71828182845904523536
                                    : 0.0;
      return "a_mkdouble(" + floatText(v) + ")";
    }
    if (b.kind == BKind::Module || b.kind == BKind::PyModule ||
        b.kind == BKind::PyFunc) {
      fail("module value is not a first-class value");
      return "A_NONE";
    }
    return b.cname;
  }

  // Register a module's import aliases (import x / from x import y,
  // including native `std:` modules and `python:` modules) into the
  // current scope so bodies can resolve module.member calls.
  void bindModuleImportStmts(const Module& mod) {
    if (!mod.program) return;
    for (const StmtPtr& s : mod.program->statements) {
      if (s->kind == StmtKind::Import) {
        const ImportStmt* im = static_cast<const ImportStmt*>(s.get());
        std::string alias = !im->alias.empty() ? im->alias
                            : !im->bareName.empty() ? im->bareName
                            : !im->pythonModule.empty() ? stemOf(im->pythonModule)
                            : stemOf(im->path);
        if (im->pythonModuleImport) {
          // import python:<mod> [as x]: dynamic module, calls go through
          // the runtime py_call builtin.
          Binding b; b.kind = BKind::PyModule; b.aux = im->pythonModule;
          scopes_.back()[alias] = b;
        } else if (im->nativeModule) {
          Binding b; b.kind = BKind::Module; b.moduleId = -1; b.aux = im->bareName;
          scopes_.back()[alias] = b;
        } else if (im->moduleId >= 0) {
          Binding b; b.kind = BKind::Module; b.moduleId = im->moduleId;
          scopes_.back()[alias] = b;
        }
      } else if (s->kind == StmtKind::FromImport) {
        const FromImportStmt* im = static_cast<const FromImportStmt*>(s.get());
        if (im->pythonModuleImport) {
          // from python:<mod> import name [as x]: dynamic member call.
          for (const auto& item : im->items) {
            Binding b; b.kind = BKind::PyFunc; b.aux = im->pythonModule;
            b.pyMember = item.name;
            scopes_.back()[item.alias.empty() ? item.name : item.alias] = b;
          }
          continue;
        }
        if (im->nativeModule) {
          // from <native> import member: bind member -> native builtin.
          for (const auto& item : im->items) {
            std::string alias = item.alias.empty() ? item.name : item.alias;
            if (const std::string* bn = nativeFunction(im->bareName, item.name)) {
              int bid = builtinRuntimeId(*bn);
              if (bid >= 0) {
                Binding b; b.kind = BKind::Builtin; b.builtinId = bid;
                scopes_.back()[alias] = b;
              }
            } else if (const std::string* cn = nativeConstant(im->bareName, item.name)) {
              Binding b; b.kind = BKind::NativeConst; b.aux = *cn;
              scopes_.back()[alias] = b;
            }
          }
          continue;
        }
        if (im->moduleId >= 0) {
          const Module& src = graph_.at(im->moduleId);
          if (src.program) {
            for (const StmtPtr& ss : src.program->statements) {
              if (ss->kind == StmtKind::FnDecl) {
                const FnDecl* fd = static_cast<const FnDecl*>(ss.get());
                // find FnInfo
                for (FnInfo* f : fnList_) {
                  if (f->decl == fd) {
                    Binding b; b.kind = BKind::FnValue; b.cname = f->globalVar; b.fn = f;
                    scopes_.back()[fd->name] = b;
                  }
                }
              } else if (ss->kind == StmtKind::StructDecl) {
                const StructDecl* sd = static_cast<const StructDecl*>(ss.get());
                for (StructInfo* st : structList_) {
                  if (st->decl == sd) {
                    Binding b; b.kind = BKind::StructCtor; b.cname = st->globalVar; b.st = st;
                    scopes_.back()[sd->name] = b;
                  }
                }
              }
            }
          }
        }
      }
    }
  }

  // Follow `from "x.a" import name` re-exports (package __init__ modules) to
  // the module that actually declares `name`. Returns {moduleId, name};
  // unchanged when there is no re-export. Native/python re-exports are not
  // followed (shortcut: module values have no runtime representation).
  // Implementation shared with the emitAsm backend (codegen_internal.hpp).
  std::pair<int, std::string> resolveModuleMember(int moduleId,
                                                  const std::string& name) {
    return a::resolveModuleMember(graph_, moduleId, name);
  }

  // ---- function / main emission ------------------------------------------
  // Traceback frame info: module path + def line for a_frame_push().
  std::string frameFile(int moduleId) const {
    if (moduleId >= 0 && moduleId < (int)graph_.modules.size())
      return graph_.at(moduleId).path;
    return "";
  }
  std::string framePush(const std::string& name, int moduleId, int line) {
    return "a_frame_push(\"" + cEscape(name) + "\", \"" + cEscape(frameFile(moduleId)) +
           "\", " + std::to_string(line) + ");\n";
  }
  static std::string frameName(const FnInfo* f) {
    return f->isMethod ? f->structName + "." + f->name : f->name;
  }
  static int frameLine(const FnDecl* decl) { return decl ? decl->span.line : 0; }

  void emitFunction(FnInfo* f) {
    std::ostringstream fn;
    fn << "aval " << f->cname << "(aval *argv) {\n";
    fn << framePush(frameName(f), f->moduleId, frameLine(f->decl));
    fn << "aval __a_env = argv[0]; (void)__a_env;\n";
    scopes_.clear();
    buildGlobalScope();
    pushScope();
    // Module-level imports are visible inside every function of that module.
    if (f->moduleId >= 0 && f->moduleId < (int)graph_.modules.size())
      bindModuleImportStmts(graph_.at(f->moduleId));
    for (std::size_t i = 0; i < f->paramNames.size(); ++i) {
      std::string cname = fresh("p");
      fn << "aval " << cname << " = argv[" << (i + 1) << "];\n";
      // Callers pad omitted args with A_TAG (the kMissing sentinel); a param
      // with a default substitutes its default expression in that case.
      if (i < f->paramDefaults.size() && f->paramDefaults[i]) {
        std::string dv = emitExpr(f->paramDefaults[i], fn);
        flushPre(fn);
        fn << "if (" << cname << " == A_TAG) " << cname << " = " << dv << ";\n";
      }
      bindLocal(f->paramNames[i], cname, BKind::Param);
    }
    std::string savedStruct = curStruct_;
    int savedModule = curModuleId_;
    curModuleId_ = f->moduleId;
    curStruct_ = f->isMethod ? f->structName : "";
    deferStack_.clear();
    loopDeferDepth_.clear();
    bool savedInFn = inFunction_;
    inFunction_ = true;
    emitBody(f->decl->body, fn);
    fn << "a_frame_pop();\nreturn A_NONE;\n}\n";
    curStruct_ = savedStruct;
    curModuleId_ = savedModule;
    inFunction_ = savedInFn;
    body_ << fn.str();
  }

  void emitStructCtor(StructInfo* st) {
    std::ostringstream fn;
    fn << "aval " << st->ctorCname << "(aval *argv) {\n";
    fn << "aval __a_d = a_dict_new();\n";
    for (std::size_t i = 0; i < st->fieldNames.size(); ++i) {
      fn << "a_dict_set(__a_d, a_str_new(\"" << st->fieldNames[i] << "\", "
         << st->fieldNames[i].size() << "), argv[" << (i + 1) << "]);\n";
    }
    fn << "return __a_d;\n}\n";
    body_ << fn.str();
  }

  std::string emitLambda(const LambdaJob& job) {
    const LambdaExpr* lam = job.decl;
    std::ostringstream fn;
    fn << "aval " << job.cname << "(aval *argv) {\n";
    fn << framePush("<lambda>", job.moduleId, lam ? lam->span.line : 0);
    fn << "aval __a_env = argv[0];\n";
    scopes_.clear();
    buildGlobalScope();
    pushScope();
    for (std::size_t i = 0; i < job.captures.size(); ++i) {
      Binding b;
      b.kind = BKind::Capture;
      b.envIdx = static_cast<int>(i);
      scopes_.back()[job.captures[i].first] = b;
    }
    for (std::size_t i = 0; i < lam->params.size(); ++i) {
      std::string cname = fresh("p");
      fn << "aval " << cname << " = argv[" << (i + 1) << "];\n";
      bindLocal(lam->params[i].name, cname, BKind::Param);
    }
    std::string savedStruct = curStruct_;
    curStruct_ = "";
    deferStack_.clear();
    loopDeferDepth_.clear();
    bool savedInFn = inFunction_;
    inFunction_ = false;
    pre_.clear();
    std::string result = emitExpr(lam->body.get(), fn);
    fn << pre_;
    pre_.clear();
    fn << "a_frame_pop();\nreturn " << result << ";\n}\n";
    curStruct_ = savedStruct;
    inFunction_ = savedInFn;
    return fn.str();
  }

  void emitMain() {
    std::ostringstream fn;
    fn << "int main(int argc, char **argv, char **envp) {\n";
    fn << "(void)argc; (void)argv; (void)envp;\n";
    fn << "a_runtime_init();\n";
    fn << framePush("<module>", graph_.entry, 0);
    for (StructInfo* st : structList_) {
      fn << st->globalVar << " = a_fnon_new((uint64_t)(uintptr_t)&" << st->ctorCname
         << ", A_NONE, \"" << st->name << "\");\n";
    }
    for (FnInfo* f : fnList_) {
      if (f->isMethod) continue;
      fn << f->globalVar << " = a_fnon_new((uint64_t)(uintptr_t)&" << f->cname
         << ", A_NONE, \"" << f->name << "\");\n";
    }
    for (auto& kv : enums_) {
      if (kv.second.algebraic) continue;
      fn << kv.second.globalVar << " = a_mkint(" << kv.second.value << ");\n";
    }
    scopes_.clear();
    buildGlobalScope();
    pushScope();
    deferStack_.clear();
    loopDeferDepth_.clear();
    inFunction_ = false;

    if (testMode_) {
      // Discover top-level test_* functions in the entry module, in
      // declaration order, and run each in a forked child so assertion
      // failures (which panic/exit) don't stop the suite.
      std::vector<FnInfo*> tests;
      for (FnInfo* f : fnList_) {
        if (!f->isMethod && f->moduleId == graph_.entry && f->name.rfind("test_", 0) == 0)
          tests.push_back(f);
      }
      std::sort(tests.begin(), tests.end(), [](FnInfo* a, FnInfo* b){ return a->name < b->name; });
      fn << "int __a_passed = 0, __a_failed = 0;\n";
      for (FnInfo* f : tests) {
        fn << "{\n";
        fn << "  int __a_pipe[2]; if (pipe(__a_pipe) != 0) { perror(\"pipe\"); return 1; }\n";
        fn << "  fflush(stdout);\n";
      fn << "  pid_t __a_pid = fork();\n";
        fn << "  if (__a_pid == 0) {\n";
        fn << "    close(__a_pipe[0]); dup2(__a_pipe[1], 2); close(__a_pipe[1]);\n";
        fn << "    { int __a_dn = open(\"/dev/null\", 1); if (__a_dn >= 0) { dup2(__a_dn, 1); close(__a_dn); } }\n";
        fn << "    aval __a_argv[1]; __a_argv[0] = A_NONE;\n";
        fn << "    " << f->cname << "(__a_argv);\n";
        fn << "    fflush(stdout); fflush(stderr); _exit(0);\n";
        fn << "  }\n";
        fn << "  close(__a_pipe[1]);\n";
        fn << "  char __a_msg[4096]; size_t __a_off = 0;\n";
        fn << "  while (__a_off < sizeof(__a_msg)-1) {\n";
        fn << "    ssize_t __a_n = read(__a_pipe[0], __a_msg + __a_off, sizeof(__a_msg)-1-__a_off);\n";
        fn << "    if (__a_n <= 0) break;\n";
        fn << "    __a_off += (size_t)__a_n;\n";
        fn << "  }\n";
        fn << "  __a_msg[__a_off] = 0;\n";
        fn << "  { char *__a_tb = strstr(__a_msg, \"\\nTraceback\"); if (__a_tb) *(__a_tb + 1) = 0; }\n";
        fn << "  close(__a_pipe[0]);\n";
        fn << "  int __a_st = 0; waitpid(__a_pid, &__a_st, 0);\n";
        fn << "  if (WIFEXITED(__a_st) && WEXITSTATUS(__a_st) == 0) {\n";
        fn << "    printf(\"ok " << f->name << "\\n\"); __a_passed++;\n";
        fn << "  } else {\n";
        fn << "    /* Strip \"error: \" prefix and trailing newline from panic output. */\n";
        fn << "    char *__a_m = __a_msg;\n";
        fn << "    if (strncmp(__a_m, \"error: \", 7) == 0) __a_m += 7;\n";
        fn << "    size_t __a_ml = strlen(__a_m);\n";
        fn << "    while (__a_ml > 0 && (__a_m[__a_ml-1] == '\\n' || __a_m[__a_ml-1] == '\\r')) __a_m[--__a_ml] = 0;\n";
        fn << "    printf(\"FAILED " << f->name << ": %s\\n\", __a_m);\n";
        fn << "    __a_failed++;\n";
        fn << "  }\n";
        fn << "}\n";
      }
      fn << "printf(\"\\n%d passed, %d failed\\n\", __a_passed, __a_failed);\n";
      fn << "return __a_failed ? 1 : 0;\n}\n";
      body_ << fn.str();
      return;
    }

    std::vector<int> order = graph_.order;
    if (order.empty()) order.push_back(graph_.entry);
    for (int mid : order) {
      if (mid < 0) continue;
      const Module& mod = graph_.at(mid);
      if (!mod.program) continue;
      // The auto-loaded prelude and stdlib modules contribute declarations
      // (already registered) but their top-level statements are not run.
      if (mod.name == "prelude" || mod.path.find("stdlib/") != std::string::npos ||
          mod.path.find("/stdlib/") != std::string::npos) continue;
      // Per-module imports: register aliases.
      bindModuleImportStmts(mod);
      curModuleId_ = mod.id;
      emitBody(mod.program->statements, fn, true);
    }
    // `fn main()` entry point: invoked once after all module top-levels,
    // entry module only. Never runs for imports or under `a test`.
    for (FnInfo* f : fnList_) {
      if (f->isMethod || f->moduleId != graph_.entry || f->name != "main") continue;
      int np = (int)f->paramNames.size();
      fn << "{ aval __a_main_argv[" << (np + 1) << "]; __a_main_argv[0] = A_NONE;\n";
      for (int i = 0; i < np; ++i) {
        if (f->variadic && i == np - 1)
          fn << "__a_main_argv[" << (i + 1) << "] = a_list_new(0, NULL);\n";
        else
          fn << "__a_main_argv[" << (i + 1) << "] = A_TAG;\n";
      }
      fn << f->cname << "(__a_main_argv); }\n";
      break;
    }
    if (testMode_) {
      // Run a single named test (argv[1]) or all tests.
      fn << "if (argc > 1) {\n";
      int entryId = graph_.entry;
      if (entryId >= 0 && graph_.at(entryId).program) {
        for (const StmtPtr& s : graph_.at(entryId).program->statements) {
          if (s->kind != StmtKind::FnDecl) continue;
          const FnDecl* fd = static_cast<const FnDecl*>(s.get());
          if (fd->name.rfind("test_", 0) != 0) continue;
          FnInfo* fi = nullptr;
          for (FnInfo* f : fnList_) if (f->decl == fd) fi = f;
          if (!fi) continue;
          std::string arr = "__a_targv";
          fn << "if (!strcmp(argv[1], \"" << fd->name << "\")) { aval " << arr << "[1] = { A_NONE }; "
             << fi->cname << "(" << arr << "); return 0; }\n";
        }
      }
      fn << "return 1; }\n";
    }
    // Drop top-level struct globals at program exit.
    for (auto& kv : scopes_.front()) emitDropCall(fn, kv.second);
    fn << "return 0;\n}\n";
    body_ << fn.str();
  }

  // ---- statements ---------------------------------------------------------
  void emitBody(const std::vector<StmtPtr>& stmts, std::ostringstream& out, bool topLevel = false) {
    pushScope();
    deferStack_.emplace_back();
    for (const StmtPtr& s : stmts) {
      if (failed_) break;
      emitStmt(s.get(), out, topLevel);
    }
    // Deferred statements, LIFO.
    auto frame = deferStack_.back();
    deferStack_.pop_back();
    for (auto it = frame.rbegin(); it != frame.rend(); ++it) {
      emitStmt(*it, out, false);
    }
    // RAII: drop struct locals in reverse declaration order, plus any
    // other droppable bindings in this scope (e.g. top-level globals).
    if (!scopeDeclOrder_.empty()) {
      auto decls = scopeDeclOrder_.back();
      std::unordered_set<std::string> seen;
      for (auto it = decls.rbegin(); it != decls.rend(); ++it) {
        seen.insert(*it);
        auto found = scopes_.back().find(*it);
        if (found != scopes_.back().end()) emitDropCall(out, found->second);
      }
      for (auto& kv : scopes_.back()) {
        if (!seen.count(kv.first)) emitDropCall(out, kv.second);
      }
    }
    popScope();
  }

  void emitDeferredForReturn(std::ostringstream& out) {
    // All active defer frames, innermost first, LIFO within a frame.
    for (int fi = static_cast<int>(deferStack_.size()) - 1; fi >= 0; --fi) {
      auto& frame = deferStack_[fi];
      for (auto it = frame.rbegin(); it != frame.rend(); ++it) {
        emitStmt(*it, out, false);
      }
    }
  }

  void emitDeferredDownTo(std::ostringstream& out, int depth) {
    for (int fi = static_cast<int>(deferStack_.size()) - 1; fi >= depth; --fi) {
      auto& frame = deferStack_[fi];
      for (auto it = frame.rbegin(); it != frame.rend(); ++it) {
        emitStmt(*it, out, false);
      }
    }
  }

  void flushPre(std::ostringstream& out) {
    out << pre_;
    pre_.clear();
  }

  void emitStmt(const Stmt* s, std::ostringstream& out, bool topLevel = false) {
    if (failed_) return;
    pre_.clear();
    switch (s->kind) {
      case StmtKind::Let: {
        const LetStmt& st = static_cast<const LetStmt&>(*s);
        std::string value = st.init ? emitExpr(st.init.get(), out) : "A_NONE";
        flushPre(out);
        if (topLevel) {
          // Top-level lets are pre-registered globals.
          std::string key = std::to_string(curModuleId_) + ":" + st.name;
          auto it = topLevelGlobals_.find(key);
          std::string cname;
          if (it != topLevelGlobals_.end()) {
            cname = it->second;
          } else {
            fail("top-level variable '" + st.name + "' was not registered");
            return;
          }
          out << cname << " = " << value << ";\n";
          Binding b; b.kind = BKind::Global; b.cname = cname;
          b.dropFn = findDrop(st.init.get());
          scopes_.back()[st.name] = b;
          scopeDeclOrder_.back().push_back(st.name);
        } else {
          std::string cname = fresh("v");
          out << "aval " << cname << " = " << value << ";\n";
          bindLocal(st.name, cname);
          if (FnInfo* df = findDrop(st.init.get())) {
            Binding* nb = lookupMut(st.name);
            if (nb) nb->dropFn = df;
          }
        }
        return;
      }
      case StmtKind::Assign: {
        emitAssign(static_cast<const AssignStmt&>(*s), out);
        return;
      }
      case StmtKind::Expr: {
        const ExprStmt& st = static_cast<const ExprStmt&>(*s);
        // Docstring: plain string literal statement.
        // Skip plain string expression statements (docstrings).
        std::string v = emitExpr(st.expr.get(), out);
        flushPre(out);
        out << "(void)(" << v << ");\n";
        return;
      }
      case StmtKind::Destructure: {
        const DestructureStmt& st = static_cast<const DestructureStmt&>(*s);
        std::string v = emitExpr(st.value.get(), out);
        std::string tv = temp("sv");
        flushPre(out);
        out << "aval " << tv << " = " << v << ";\n";
        for (std::size_t i = 0; i < st.names.size(); ++i) {
          if (st.names[i] == "_") continue;
          std::string cname = fresh("v");
          out << "aval " << cname << " = a_getitem(" << tv << ", a_mkint(" << i << "));\n";
          bindLocal(st.names[i], cname);
        }
        return;
      }
      case StmtKind::Return: {
        const ReturnStmt& st = static_cast<const ReturnStmt&>(*s);
        std::string v = st.value ? emitExpr(st.value.get(), out) : "A_NONE";
        flushPre(out);
        if (inFunction_) {
          std::string rv = temp("ret");
          out << "aval " << rv << " = " << v << ";\n";
          emitDeferredForReturn(out);
          out << "a_frame_pop();\nreturn " << rv << ";\n";
        } else {
          out << "return 0;\n";
        }
        return;
      }
      case StmtKind::Break: {
        int depth = loopDeferDepth_.empty() ? 0 : loopDeferDepth_.back();
        emitDeferredDownTo(out, depth);
        out << "break;\n";
        return;
      }
      case StmtKind::Continue: {
        int depth = loopDeferDepth_.empty() ? 0 : loopDeferDepth_.back();
        emitDeferredDownTo(out, depth);
        out << "continue;\n";
        return;
      }
      case StmtKind::Defer: {
        const DeferStmt& st = static_cast<const DeferStmt&>(*s);
        if (!deferStack_.empty()) deferStack_.back().push_back(st.body.get());
        return;
      }
      case StmtKind::If: {
        emitIf(static_cast<const IfStmt&>(*s), out, 0);
        return;
      }
      case StmtKind::While: {
        const WhileStmt& st = static_cast<const WhileStmt&>(*s);
        out << "while (1) {\n";
        loopDeferDepth_.push_back(static_cast<int>(deferStack_.size()));
        pre_.clear();
        std::string cond = emitExpr(st.cond.get(), out);
        flushPre(out);
        out << "if (!a_truthy_raw(" << cond << ")) break;\n";
        emitBody(st.body, out);
        loopDeferDepth_.pop_back();
        out << "}\n";
        return;
      }
      case StmtKind::DoWhile: {
        const DoWhileStmt& st = static_cast<const DoWhileStmt&>(*s);
        out << "while (1) {\n";
        loopDeferDepth_.push_back(static_cast<int>(deferStack_.size()));
        emitBody(st.body, out);
        pre_.clear();
        std::string cond = emitExpr(st.cond.get(), out);
        flushPre(out);
        out << "if (!a_truthy_raw(" << cond << ")) break;\n";
        loopDeferDepth_.pop_back();
        out << "}\n";
        return;
      }
      case StmtKind::For: {
        const ForStmt& st = static_cast<const ForStmt&>(*s);
        // Fast path: `for x in lo..hi` / `for x in lo..hi..step` over an
        // integer range. Emit a counted loop with int64_t counters instead
        // of heap-allocating an ARange + AIter and dispatching per element.
        {
          const Expr* loE = nullptr;
          const Expr* hiE = nullptr;
          const Expr* stE = nullptr;
          const Expr* iter = st.iterable.get();
          if (iter && iter->kind == ExprKind::Range) {
            const RangeExpr& re = static_cast<const RangeExpr&>(*iter);
            loE = re.start.get();
            hiE = re.end.get();
            stE = re.step.get();
          } else if (iter && iter->kind == ExprKind::Binary &&
                     static_cast<const BinaryExpr&>(*iter).op == "..") {
            const BinaryExpr& be = static_cast<const BinaryExpr&>(*iter);
            loE = be.lhs.get();
            hiE = be.rhs.get();
          }
          if (loE && hiE) {
            std::string loExpr = emitExpr(loE, out);
            std::string hiExpr = emitExpr(hiE, out);
            std::string stExpr = stE ? emitExpr(stE, out) : std::string("a_mkint(1)");
            std::string loT = temp("rlo");
            std::string hiT = temp("rhi");
            std::string stT = temp("rst");
            std::string cur = fresh("rcur");
            std::string endv = fresh("rend");
            std::string stepv = fresh("rstep");
            std::string var = fresh("v");
            pre("aval " + loT + " = " + loExpr + ";");
            pre("aval " + hiT + " = " + hiExpr + ";");
            pre("aval " + stT + " = " + stExpr + ";");
            flushPre(out);
            out << "int64_t " << cur << " = a_int_full(" << loT << ");\n";
            out << "int64_t " << endv << " = a_int_full(" << hiT << ");\n";
            out << "int64_t " << stepv << " = a_int_full(" << stT << ");\n";
            out << "if (" << stepv << " == 0) { a_panic_msg(\"range() step must be positive, found 0.\"); }\n";
            out << "aval " << var << ";\n";
            out << "for (; (" << stepv << " > 0 ? " << cur << " < " << endv
                << " : " << cur << " > " << endv << "); "
                << cur << " = (" << stepv << " > 0 ? ("
                << cur << " > INT64_MAX - " << stepv << " ? " << endv << " : " << cur << " + " << stepv
                << ") : ("
                << cur << " < INT64_MIN - " << stepv << " ? " << endv << " : " << cur << " + " << stepv
                << "))) {\n";
            out << var << " = a_mkint(" << cur << ");\n";
            loopDeferDepth_.push_back(static_cast<int>(deferStack_.size()));
            pushScope();
            bindLocal(st.var, var);
            deferStack_.emplace_back();
            for (const StmtPtr& bs : st.body) emitStmt(bs.get(), out, false);
            auto frame = deferStack_.back(); deferStack_.pop_back();
            for (auto it2 = frame.rbegin(); it2 != frame.rend(); ++it2) emitStmt(*it2, out, false);
            popScope();
            loopDeferDepth_.pop_back();
            out << "}\n";
            return;
          }
        }
        std::string it = emitExpr(st.iterable.get(), out);
        std::string itv = temp("it");
        std::string var = fresh("v");
        flushPre(out);
        out << "aval " << itv << " = a_iter_new(" << it << ");\n";
        out << "aval " << var << ";\n";
        out << "while (a_iter_next(" << itv << ", &" << var << ")) {\n";
        loopDeferDepth_.push_back(static_cast<int>(deferStack_.size()));
        pushScope();
        bindLocal(st.var, var);
        deferStack_.emplace_back();
        for (const StmtPtr& bs : st.body) emitStmt(bs.get(), out, false);
        auto frame = deferStack_.back(); deferStack_.pop_back();
        for (auto it2 = frame.rbegin(); it2 != frame.rend(); ++it2) emitStmt(*it2, out, false);
        popScope();
        loopDeferDepth_.pop_back();
        out << "}\n";
        return;
      }
      case StmtKind::Loop: {
        const LoopStmt& st = static_cast<const LoopStmt&>(*s);
        out << "while (1) {\n";
        loopDeferDepth_.push_back(static_cast<int>(deferStack_.size()));
        emitBody(st.body, out);
        loopDeferDepth_.pop_back();
        out << "}\n";
        return;
      }
      case StmtKind::FnDecl:
      case StmtKind::StructDecl:
      case StmtKind::EnumDecl:
      case StmtKind::TypeAlias:
      case StmtKind::TraitDecl:
      case StmtKind::ImplDecl:
      case StmtKind::Import:
      case StmtKind::FromImport:
        return;
    }
  }

  void emitIf(const IfStmt& st, std::ostringstream& out, std::size_t branchIdx) {
    if (branchIdx < st.branches.size()) {
      pre_.clear();
      std::string cond = emitExpr(st.branches[branchIdx].cond.get(), out);
      flushPre(out);
      out << "if (a_truthy_raw(" << cond << ")) {\n";
      emitBody(st.branches[branchIdx].body, out);
      out << "} else {\n";
      emitIf(st, out, branchIdx + 1);
      out << "}\n";
    } else if (!st.elseBody.empty()) {
      emitBody(st.elseBody, out);
    }
  }

  void emitAssign(const AssignStmt& st, std::ostringstream& out) {
    std::string value = emitExpr(st.value.get(), out);
    if (st.op != "=") {
      // Compound: read current, apply base op, store.
      std::string cur = emitExpr(st.target.get(), out);
      std::string baseOp = st.op.substr(0, st.op.size() - 1);
      std::string rhs = value;
      value = "(" + binaryRuntime(baseOp, cur, rhs) + ")";
    }
    if (st.target->kind == ExprKind::Ident) {
      const IdentExpr& id = static_cast<const IdentExpr&>(*st.target);
      flushPre(out);
      Binding* b = lookupMut(id.name);
      if (!b) {
        // Assignment to undeclared name: create local.
        std::string cname = fresh("v");
        out << "aval " << cname << " = " << value << ";\n";
        bindLocal(id.name, cname);
        if (FnInfo* df = findDrop(st.value.get())) {
          Binding* nb = lookupMut(id.name);
          if (nb) nb->dropFn = df;
        }
        return;
      }
      if (b->kind == BKind::Capture) {
        out << "a_env_set(__a_env, " << b->envIdx << ", " << value << ");\n";
      } else {
        FnInfo* oldDrop = b->dropFn;
        if (oldDrop) { Binding tmp = *b; tmp.dropFn = oldDrop; emitDropCall(out, tmp); }
        out << b->cname << " = " << value << ";\n";
        if (FnInfo* df = findDrop(st.value.get())) b->dropFn = df; else b->dropFn = nullptr;
      }
      return;
    }
    if (st.target->kind == ExprKind::Index) {
      const IndexExpr& idx = static_cast<const IndexExpr&>(*st.target);
      std::string obj = emitExpr(idx.object.get(), out);
      std::string ix = emitExpr(idx.index.get(), out);
      flushPre(out);
      out << "a_setitem(" << obj << ", " << ix << ", " << value << ");\n";
      return;
    }
    if (st.target->kind == ExprKind::Field) {
      const FieldExpr& f = static_cast<const FieldExpr&>(*st.target);
      // Module attribute assignment: counter.value = 99
      if (f.object->kind == ExprKind::Ident) {
        const Binding* mb = lookup(static_cast<const IdentExpr&>(*f.object).name);
        if (mb && mb->kind == BKind::Module && mb->moduleId >= 0) {
          auto it = topLevelGlobals_.find(std::to_string(mb->moduleId) + ":" + f.field);
          if (it != topLevelGlobals_.end()) {
            flushPre(out);
            out << it->second << " = " << value << ";\n";
            return;
          }
        }
      }
      std::string obj = emitExpr(f.object.get(), out);
      flushPre(out);
      out << "a_setattr(" << obj << ", \"" << cEscape(f.field) << "\", " << value << ");\n";
      return;
    }
    fail("unsupported assignment target");
  }

  std::string binaryRuntime(const std::string& op, const std::string& l, const std::string& r) {
    static const std::unordered_map<std::string, std::string> kMap = {
        {"+", "a_add"}, {"-", "a_sub"}, {"*", "a_mul"}, {"/", "a_div"},
        {"%", "a_mod"}, {"**", "a_pow"}, {"==", "a_eq"}, {"!=", "a_ne"},
        {"<", "a_lt"}, {"<=", "a_le"}, {">", "a_gt"}, {">=", "a_ge"},
        {"&", "a_band"}, {"|", "a_bor"}, {"^", "a_bxor"}, {"<<", "a_shl"},
        {">>", "a_shr"},
    };
    auto it = kMap.find(op);
    if (it != kMap.end()) return it->second + "(" + l + ", " + r + ")";
    if (op == "in") return "a_contains(" + r + ", " + l + ")";
    if (op == "not in") return "a_not(a_contains(" + r + ", " + l + "))";
    fail("unsupported operator '" + op + "'");
    return "A_NONE";
  }

  // ---- expressions --------------------------------------------------------
  std::string emitExpr(const Expr* e, std::ostringstream& out) {
    if (!e) return "A_NONE";
    switch (e->kind) {
      case ExprKind::IntLit: {
        auto& x = static_cast<const IntLitExpr&>(*e);
        return "a_mkint(" + std::to_string(x.value) + ")";
      }
      case ExprKind::FloatLit: {
        auto& x = static_cast<const FloatLitExpr&>(*e);
        return "a_mkdouble(" + floatText(x.value) + ")";
      }
      case ExprKind::BoolLit: {
        auto& x = static_cast<const BoolLitExpr&>(*e);
        return x.value ? "A_TRUE" : "A_FALSE";
      }
      case ExprKind::CharLit: {
        auto& x = static_cast<const CharLitExpr&>(*e);
        std::string u = utf8Encode(x.value);
        return "a_char_new(\"" + cEscape(u) + "\", " + std::to_string(u.size()) + ")";
      }
      case ExprKind::StringLit: {
        auto& x = static_cast<const StringLitExpr&>(*e);
        if (x.parts.size() == 1 && !x.parts[0].isExpr) {
          const std::string& t = x.parts[0].text;
          return "a_str_new(\"" + cEscape(t) + "\", " + std::to_string(t.size()) + ")";
        }
        if (x.parts.empty()) return "a_str_new(\"\", 0)";
        // Interpolation: join parts with a_str_joinsep("", n, argv).
        std::vector<std::string> segs;
        for (auto& p : x.parts) {
          if (p.isExpr) {
            std::string v = emitExpr(p.expr.get(), out);
            segs.push_back("a_str(" + v + ")");
          } else {
            segs.push_back("a_str_new(\"" + cEscape(p.text) + "\", " +
                           std::to_string(p.text.size()) + ")");
          }
        }
        std::string arr = temp("argv");
        std::string sep = temp("sep");
        pre("aval " + sep + " = a_str_new(\"\", 0);");
        pre("aval " + arr + "[" + std::to_string(segs.size()) + "] = { " +
            join(segs, ", ") + " };");
        return "a_str_joinsep(" + sep + ", " + std::to_string(segs.size()) + ", " + arr + ")";
      }
      case ExprKind::Ident: {
        auto& x = static_cast<const IdentExpr&>(*e);
        if (x.name == "None") return "A_NONE";
        if (x.name == "PI") return "a_mkdouble(3.141592653589793)";
        if (x.name == "E") return "a_mkdouble(2.718281828459045)";
        if (x.name == "I8_MIN") return "a_mkint(-128)";
        if (x.name == "I8_MAX") return "a_mkint(127)";
        if (x.name == "I16_MIN") return "a_mkint(-32768)";
        if (x.name == "I16_MAX") return "a_mkint(32767)";
        if (x.name == "I32_MIN") return "a_mkint(-2147483648)";
        if (x.name == "I32_MAX") return "a_mkint(2147483647)";
        if (x.name == "I64_MIN") return "a_mkint(-9223372036854775807LL-1)";
        if (x.name == "I64_MAX") return "a_mkint(9223372036854775807LL)";
        if (x.name == "U8_MAX") return "a_mkint(255)";
        if (x.name == "U16_MAX") return "a_mkint(65535)";
        if (x.name == "U32_MAX") return "a_mkint(4294967295LL)";
        if (x.name == "F32_MIN") return "a_mkdouble(-3.4028234663852886e38)";
        if (x.name == "F32_MAX") return "a_mkdouble(3.4028234663852886e38)";
        if (x.name == "F64_MIN") return "a_mkdouble(-1.7976931348623157e308)";
        if (x.name == "F64_MAX") return "a_mkdouble(1.7976931348623157e308)";
        if (x.name == "Some" || x.name == "Ok" || x.name == "Err") {
          // Used as a value (e.g. in a pattern/call handled elsewhere);
          // as a bare value materialize the builtin.
          int id = builtinRuntimeId(x.name);
          if (id >= 0) return "a_builtin(" + std::to_string(id) + ")";
        }
        const Binding* b = lookup(x.name);
        if (b) {
          if (b->kind == BKind::Builtin) return "a_builtin(" + std::to_string(b->builtinId) + ")";
          return bindingExpr(*b);
        }
        int id = builtinRuntimeId(x.name);
        if (id >= 0) return "a_builtin(" + std::to_string(id) + ")";
        fail("undefined name '" + x.name + "'");
        return "A_NONE";
      }
      case ExprKind::Unary: {
        auto& x = static_cast<const UnaryExpr&>(*e);
        if (x.op == "!") {
          // Postfix unwrap (the parser does not set `postfix` for '!').
          return "a_unwrap(" + emitExpr(x.operand.get(), out) + ")";
        }
        if (x.op == "++" || x.op == "--") {
          std::string delta = x.op == "++" ? "1" : "-1";
          std::string cur = emitExpr(x.operand.get(), out);
          std::string old = temp("old");
          std::string nw = temp("new");
          pre("aval " + old + " = " + cur + ";");
          pre("aval " + nw + " = a_add(" + old + ", a_mkint(" + delta + "));");
          // Store back if ident.
          if (x.operand->kind == ExprKind::Ident) {
            const Binding* b = lookup(static_cast<const IdentExpr&>(*x.operand).name);
            if (b) {
              if (b->kind == BKind::Capture)
                pre("a_env_set(__a_env, " + std::to_string(b->envIdx) + ", " + nw + ");");
              else
                pre(b->cname + " = " + nw + ";");
            }
          }
          return x.postfix ? old : nw;
        }
        std::string v = emitExpr(x.operand.get(), out);
        if (x.op == "-") return "a_neg(" + v + ")";
        if (x.op == "~") return "a_bnot(" + v + ")";
        if (x.op == "not" || x.op == "!") return "a_not(" + v + ")";
        fail("unsupported unary operator '" + x.op + "'");
        return "A_NONE";
      }
      case ExprKind::Binary: {
        auto& x = static_cast<const BinaryExpr&>(*e);
        std::string op = x.op;
        if (op == "and" || op == "or" || op == "??") {
          std::string l = emitExpr(x.lhs.get(), out);
          std::string lt = temp("lhs");
          pre("aval " + lt + " = " + l + ";");
          std::string r = emitExpr(x.rhs.get(), out);
          if (op == "and") return "(a_truthy_raw(" + lt + ") ? " + r + " : " + lt + ")";
          if (op == "or") return "(a_truthy_raw(" + lt + ") ? " + lt + " : " + r + ")";
          return "(a_is_none(" + lt + ") ? " + r + " : a_unwrap(" + lt + "))";
        }
        std::string l = emitExpr(x.lhs.get(), out);
        std::string r = emitExpr(x.rhs.get(), out);
        if (op == "..") {
          // Binary range spelling.
          return "a_range_new(" + l + ", " + r + ", a_mkint(1))";
        }
        return binaryRuntime(op, l, r);
      }
      case ExprKind::Range: {
        auto& x = static_cast<const RangeExpr&>(*e);
        std::string lo = emitExpr(x.start.get(), out);
        std::string hi = emitExpr(x.end.get(), out);
        std::string step = x.step ? emitExpr(x.step.get(), out) : "a_mkint(1)";
        return "a_range_new(" + lo + ", " + hi + ", " + step + ")";
      }
      case ExprKind::Ternary: {
        auto& x = static_cast<const TernaryExpr&>(*e);
        std::string c = emitExpr(x.cond.get(), out);
        std::string ct = temp("cond");
        pre("aval " + ct + " = " + c + ";");
        std::string tv = emitExpr(x.thenValue.get(), out);
        std::string ev = emitExpr(x.elseValue.get(), out);
        return "(a_truthy_raw(" + ct + ") ? " + tv + " : " + ev + ")";
      }
      case ExprKind::Index: {
        auto& x = static_cast<const IndexExpr&>(*e);
        std::string o = emitExpr(x.object.get(), out);
        std::string i = emitExpr(x.index.get(), out);
        return "a_getitem(" + o + ", " + i + ")";
      }
      case ExprKind::Slice: {
        auto& x = static_cast<const SliceExpr&>(*e);
        std::string o = emitExpr(x.object.get(), out);
        std::string lo = x.start ? emitExpr(x.start.get(), out) : "A_NONE";
        std::string hi = x.end ? emitExpr(x.end.get(), out) : "A_NONE";
        std::string st = x.step ? emitExpr(x.step.get(), out) : "A_NONE";
        return "a_slice(" + o + ", " + lo + ", " + hi + ", " + st + ")";
      }
      case ExprKind::Field: {
        auto& x = static_cast<const FieldExpr&>(*e);
        // Module-qualified chain: c.Color.Red, c.LIMIT, mod.fn (as value)
        {
          std::vector<std::string> chain;
          const Expr* cur = e;
          while (cur && cur->kind == ExprKind::Field) {
            chain.push_back(static_cast<const FieldExpr*>(cur)->field);
            cur = static_cast<const FieldExpr*>(cur)->object.get();
          }
          if (cur && cur->kind == ExprKind::Ident && chain.size() >= 1) {
            std::reverse(chain.begin(), chain.end());
            const std::string& first = static_cast<const IdentExpr*>(cur)->name;
            const Binding* mb = lookup(first);
            if (mb && mb->kind == BKind::Module && mb->moduleId >= 0) {
              // Enum variant via module: c.Color.Red
              if (chain.size() >= 2) {
                auto it = enums_.find(chain[chain.size()-2] + "." + chain.back());
                if (it != enums_.end()) {
                  if (it->second.algebraic) {
                    std::string d = temp("ev");
                    pre("aval " + d + " = a_dict_new();");
                    pre("a_dict_set(" + d + ", a_str_new(\"__tag\", 5), a_mkint(" +
                        std::to_string(it->second.tag) + "));");
                    return d;
                  }
                  return it->second.globalVar;
                }
              }
              // Top-level let / fn / struct in that module: c.LIMIT
              // (following `from x import y` re-exports in __init__ files).
              if (chain.size() == 1) {
                auto rm = resolveModuleMember(mb->moduleId, chain[0]);
                auto it = topLevelGlobals_.find(std::to_string(rm.first) + ":" + rm.second);
                if (it != topLevelGlobals_.end()) return it->second;
                for (FnInfo* fi : fnList_) {
                  if (!fi->isMethod && fi->moduleId == rm.first && fi->name == rm.second)
                    return fi->globalVar;
                }
                for (StructInfo* st : structList_) {
                  if (st->moduleId == rm.first && st->name == rm.second)
                    return st->globalVar;
                }
              }
            } else if (mb && mb->kind == BKind::Module && mb->moduleId < 0 &&
                       !mb->aux.empty()) {
              // Native module member as a value: math.PI, math.sqrt.
              if (chain.size() == 1) {
                if (const std::string* cn = nativeConstant(mb->aux, chain[0])) {
                  double v = *cn == "PI"   ? 3.14159265358979323846
                            : *cn == "E"    ? 2.71828182845904523536
                                            : 0.0;
                  return "a_mkdouble(" + floatText(v) + ")";
                }
                if (const std::string* fn = nativeFunction(mb->aux, chain[0])) {
                  int bid = builtinRuntimeId(*fn);
                  if (bid >= 0) return "a_builtin(" + std::to_string(bid) + ")";
                }
                int bid = builtinRuntimeId(chain[0]);
                if (bid >= 0) return "a_builtin(" + std::to_string(bid) + ")";
              }
              fail("unknown native module member '" + chain.back() + "'");
              return "A_NONE";
            }
          }
        }
        // Enum variant: Color.Red
        if (x.object->kind == ExprKind::Ident) {
          const std::string& oname = static_cast<const IdentExpr&>(*x.object).name;
          auto it = enums_.find(oname + "." + x.field);
          if (it != enums_.end()) {
            if (it->second.algebraic) {
              std::string d = temp("ev");
              pre("aval " + d + " = a_dict_new();");
              pre("a_dict_set(" + d + ", a_str_new(\"__tag\", 5), a_mkint(" +
                  std::to_string(it->second.tag) + "));");
              pre("a_dict_set(" + d + ", a_str_new(\"__name\", 6), a_str_new(\"" +
                  cEscape(x.field) + "\", " + std::to_string(x.field.size()) + "));");
              return d;
            }
            return it->second.globalVar;
          }
          const Binding* mb = lookup(oname);
          if (mb && mb->kind == BKind::Module) {
            // Qualified enum via module: fallthrough to getattr (will panic
            // at runtime unless the module exports it) - try short enum name.
            auto it2 = enums_.find(x.field);
            if (it2 != enums_.end()) return it2->second.globalVar;
          }
        }
        std::string o = emitExpr(x.object.get(), out);
        return "a_getattr(" + o + ", \"" + cEscape(x.field) + "\")";
      }
      case ExprKind::SafeNav: {
        auto& x = static_cast<const SafeNavExpr&>(*e);
        std::string o = emitExpr(x.object.get(), out);
        std::string ot = temp("obj");
        std::string inner = temp("inner");
        std::string fv = temp("field");
        std::string res = temp("res");
        pre("aval " + ot + " = " + o + ";");
        pre("aval " + res + " = A_NONE;");
        pre("if (!a_is_none(" + ot + ")) {");
        pre("aval " + inner + " = a_unwrap(" + ot + ");");
        pre("aval " + fv + " = a_getattr(" + inner + ", \"" + cEscape(x.field) + "\");");
        // Wrap in Some unless the field itself is optional (then the
        // result type is Optional<Optional<T>>).
        TypePtr t = resolveType(e->inferredType);
        TypePtr elem = t && t->kind == TypeKind::Optional ? resolveType(t->elem) : nullptr;
        bool fieldOptional = elem && elem->kind == TypeKind::Optional;
        if (fieldOptional)
          pre(res + " = " + fv + ";");
        else
          pre(res + " = a_some_new(" + fv + ");");
        pre("}");
        return res;
      }
      case ExprKind::ArrayLit: {
        auto& x = static_cast<const ArrayLitExpr&>(*e);
        std::vector<std::string> elems;
        for (auto& el : x.elements) elems.push_back(emitExpr(el.get(), out));
        if (elems.empty()) return "a_list_new(0, NULL)";
        std::string arr = temp("argv");
        pre("aval " + arr + "[" + std::to_string(elems.size()) + "] = { " + join(elems, ", ") + " };");
        return "a_list_new(" + std::to_string(elems.size()) + ", " + arr + ")";
      }
      case ExprKind::Tuple: {
        auto& x = static_cast<const TupleExpr&>(*e);
        std::vector<std::string> elems;
        for (auto& el : x.elements) elems.push_back(emitExpr(el.get(), out));
        if (elems.empty()) return "a_tuple_new(0, NULL)";
        std::string arr = temp("argv");
        pre("aval " + arr + "[" + std::to_string(elems.size()) + "] = { " + join(elems, ", ") + " };");
        return "a_tuple_new(" + std::to_string(elems.size()) + ", " + arr + ")";
      }
      case ExprKind::MapLit: {
        auto& x = static_cast<const MapLitExpr&>(*e);
        std::string d = temp("dict");
        pre("aval " + d + " = a_dict_new();");
        for (auto& kv : x.entries) {
          std::string k = emitExpr(kv.first.get(), out);
          std::string v = emitExpr(kv.second.get(), out);
          pre("a_dict_set(" + d + ", " + k + ", " + v + ");");
        }
        return d;
      }
      case ExprKind::SetLit: {
        auto& x = static_cast<const SetLitExpr&>(*e);
        std::string d = temp("set");
        pre("aval " + d + " = a_set_new();");
        for (auto& el : x.elements) {
          std::string v = emitExpr(el.get(), out);
          pre("a_set_add(" + d + ", " + v + ");");
        }
        return d;
      }
      case ExprKind::Call:
        return emitCall(static_cast<const CallExpr&>(*e), out);
      case ExprKind::Lambda:
        return emitLambdaExpr(static_cast<const LambdaExpr&>(*e), out);
      case ExprKind::Comprehension:
        return emitComprehension(static_cast<const ComprehensionExpr&>(*e), out);
      case ExprKind::Match:
        return emitMatch(static_cast<const MatchExpr&>(*e), out);
      case ExprKind::Try: {
        auto& x = static_cast<const TryExpr&>(*e);
        std::string v = emitExpr(x.operand.get(), out);
        std::string t = temp("try");
        pre("aval " + t + " = a_try_result(" + v + ");");
        if (inFunction_) {
          pre("if (a_is_propagate(" + t + ")) {");
          // Run defers, then return the propagating value.
          std::string defers;
          std::string savedPre = pre_;
          for (int fi = static_cast<int>(deferStack_.size()) - 1; fi >= 0; --fi) {
            auto& frame = deferStack_[fi];
            for (auto it = frame.rbegin(); it != frame.rend(); ++it) {
              std::ostringstream tmp;
              emitStmt(*it, tmp, false);
              defers += tmp.str();
            }
          }
          pre_ = savedPre;
          pre(defers + "a_frame_pop(); return " + t + "; }");
        }
        return t;
      }
    }
    fail("unsupported expression");
    return "A_NONE";
  }

  std::string join(const std::vector<std::string>& parts, const std::string& sep) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
      if (i) out += sep;
      out += parts[i];
    }
    return out;
  }

  // Build a C argv array in the prelude. If withEnv, slot 0 is A_NONE and
  // arguments start at 1 (direct function calls); otherwise arguments start
  // at 0 (a_call / a_method).
  std::string buildArgv(const std::vector<std::string>& argExprs, bool withEnv) {
    std::string arr = temp("argv");
    int n = static_cast<int>(argExprs.size()) + (withEnv ? 1 : 0);
    if (n == 0) n = 1;
    std::string decl = "aval " + arr + "[" + std::to_string(n) + "];";
    pre(decl);
    int idx = 0;
    if (withEnv) pre(arr + "[0] = A_NONE;");
    for (auto& a : argExprs) {
      pre(arr + "[" + std::to_string(idx + (withEnv ? 1 : 0)) + "] = " + a + ";");
      ++idx;
    }
    return arr;
  }

  // Reorder call args according to a known parameter list (named args).
  // `variadicIdx` >= 0 marks a trailing `*args` param: positional args at or
  // past that index are packed into a list. Unfilled params get A_TAG (the
  // kMissing sentinel) so callees substitute defaults; struct ctors pass
  // "A_NONE" since fields have no defaults.
  std::vector<std::string> orderArgs(const CallExpr& call,
                                     const std::vector<std::string>& paramNames,
                                     std::ostringstream& out,
                                     int variadicIdx = -1,
                                     const char* missingFill = "A_TAG") {
    std::vector<std::string> ordered(paramNames.size(), "");
    std::vector<bool> filled(paramNames.size(), false);
    std::vector<std::string> posVals;
    for (auto& a : call.args) {
      std::string v = emitExpr(a.value.get(), out);
      if (a.name.empty()) {
        posVals.push_back(v);
      } else {
        for (std::size_t i = 0; i < paramNames.size(); ++i) {
          if (paramNames[i] == a.name) {
            ordered[i] = v;
            filled[i] = true;
          }
        }
      }
    }
    std::size_t fixed = variadicIdx >= 0 ? (std::size_t)variadicIdx : paramNames.size();
    for (std::size_t i = 0; i < posVals.size() && i < fixed && i < paramNames.size(); ++i) {
      ordered[i] = posVals[i];
      filled[i] = true;
    }
    if (variadicIdx >= 0 && (std::size_t)variadicIdx < paramNames.size()) {
      std::string lst = temp("var");
      pre("aval " + lst + " = a_list_new(0, NULL);");
      for (std::size_t i = fixed; i < posVals.size(); ++i) {
        std::string tv = temp("va");
        pre("aval " + tv + " = " + posVals[i] + ";");
        pre("(void)a_list_push(" + lst + ", " + tv + ");");
      }
      ordered[variadicIdx] = lst;
      filled[variadicIdx] = true;
    }
    for (std::size_t i = 0; i < ordered.size(); ++i)
      if (!filled[i]) ordered[i] = missingFill;
    return ordered;
  }

  std::vector<std::string> positionalArgs(const CallExpr& call, std::ostringstream& out) {
    std::vector<std::string> args;
    for (auto& a : call.args) args.push_back(emitExpr(a.value.get(), out));
    return args;
  }

  // py_call(module, member, nkw, pos..., (kname, kval)*nkw): argv[0..1] are
  // the module/member strings, argv[2] the kwargs count, then positional
  // args, then name/value pairs. The runtime resolves libpython.
  std::string emitPyCall(const std::string& module, const std::string& member,
                         const CallExpr& call, std::ostringstream& out) {
    std::vector<std::string> pos;
    std::vector<std::string> kwFlat;
    int nkw = 0;
    for (auto& a : call.args) {
      if (a.name.empty()) {
        pos.push_back(emitExpr(a.value.get(), out));
      } else {
        ++nkw;
        kwFlat.push_back("a_str_new(\"" + cEscape(a.name) + "\", " +
                         std::to_string(a.name.size()) + ")");
        kwFlat.push_back(emitExpr(a.value.get(), out));
      }
    }
    std::vector<std::string> args;
    args.push_back("a_str_new(\"" + cEscape(module) + "\", " +
                   std::to_string(module.size()) + ")");
    args.push_back("a_str_new(\"" + cEscape(member) + "\", " +
                   std::to_string(member.size()) + ")");
    args.push_back("a_mkint(" + std::to_string(nkw) + ")");
    args.insert(args.end(), pos.begin(), pos.end());
    args.insert(args.end(), kwFlat.begin(), kwFlat.end());
    std::string arr = buildArgv(args, false);
    int bid = builtinRuntimeId("py_call");
    return "a_bi_call(" + std::to_string(bid) + ", " +
           std::to_string(args.size()) + ", " + arr + ")";
  }

  std::string emitCall(const CallExpr& call, std::ostringstream& out) {
    // Sema-resolved callee FnDecl (prelude fallback / ic desugar): dispatch
    // on the exact declaration, not the bare name.
    if (call.calleeFunction) {
      for (FnInfo* fi : fnList_) {
        if (!fi->isMethod && fi->decl == call.calleeFunction) {
          auto args = orderArgs(call, fi->paramNames, out,
                                fi->variadic ? (int)fi->paramNames.size() - 1 : -1);
          std::string arr = buildArgv(args, true);
          return fi->cname + "(" + arr + ")";
        }
      }
    }
    // Sema-resolved builtin (unshadowed).
    if (call.builtinId != BuiltinId::None) {
      int id = builtinIdForBuiltin(call.builtinId);
      if (id >= 0) {
        auto args = positionalArgs(call, out);
        std::string arr = buildArgv(args, false);
        return "a_bi_call(" + std::to_string(id) + ", " +
               std::to_string(args.size()) + ", " + arr + ")";
      }
    }
    const Expr* callee = call.callee.get();
    if (callee->kind == ExprKind::Ident) {
      const IdentExpr& id = static_cast<const IdentExpr&>(*callee);
      // Sema-resolved call to a runtime builtin (bare linalg names, native
    // from-imports): use the builtin id, never the ambiguous cross-module
    // bare-name binding table.
    if (call.calleeIsBuiltin) {
      int bid = builtinRuntimeId(call.calleeBuiltin.empty() ? id.name
                                                             : call.calleeBuiltin);
      if (bid >= 0) {
        auto args = positionalArgs(call, out);
        std::string arr = buildArgv(args, false);
        return "a_bi_call(" + std::to_string(bid) + ", " +
               std::to_string(args.size()) + ", " + arr + ")";
      }
    }
    const Binding* b = lookup(id.name);
      if (b) {
        if (b->kind == BKind::FnValue && b->fn) {
          auto args = orderArgs(call, b->fn->paramNames, out, b->fn->variadic ? (int)b->fn->paramNames.size()-1 : -1);
          std::string arr = buildArgv(args, true);
          return b->fn->cname + "(" + arr + ")";
        }
        if (b->kind == BKind::StructCtor && b->st) {
          auto args = orderArgs(call, b->st->fieldNames, out, -1, "A_NONE");
          std::string arr = buildArgv(args, true);
          return b->st->ctorCname + "(" + arr + ")";
        }
        if (b->kind == BKind::Builtin) {
          auto args = positionalArgs(call, out);
          std::string arr = buildArgv(args, false);
          return "a_bi_call(" + std::to_string(b->builtinId) + ", " +
                 std::to_string(args.size()) + ", " + arr + ")";
        }
        if (b->kind == BKind::PyFunc) {
          return emitPyCall(b->aux, b->pyMember, call, out);
        }
        if (b->kind == BKind::EnumValue || b->kind == BKind::NativeConst) {
          fail("value is not callable");
          return "A_NONE";
        }
        // Variable holding a function.
        auto args = positionalArgs(call, out);
        std::string arr = buildArgv(args, false);
        return "a_call(" + bindingExpr(*b) + ", " + std::to_string(args.size()) + ", " + arr + ")";
      }
      int bid = builtinRuntimeId(id.name);
      if (bid >= 0) {
        auto args = positionalArgs(call, out);
        std::string arr = buildArgv(args, false);
        return "a_bi_call(" + std::to_string(bid) + ", " +
               std::to_string(args.size()) + ", " + arr + ")";
      }
      fail("undefined function '" + id.name + "'");
      return "A_NONE";
    }
    if (callee->kind == ExprKind::Field) {
      const FieldExpr& f = static_cast<const FieldExpr&>(*callee);
      // Module-qualified: math.sqrt, mod.fn, mod.Struct
      if (f.object->kind == ExprKind::Ident) {
        const IdentExpr& mid = static_cast<const IdentExpr&>(*f.object);
        const Binding* mb = lookup(mid.name);
        if (mb && mb->kind == BKind::PyModule) {
          return emitPyCall(mb->aux, f.field, call, out);
        }
        if (mb && mb->kind == BKind::Module) {
          if (mb->moduleId < 0) {
            // Native module member: resolve module.member -> builtin name
            // via the nativeModules() spec first (member names may shadow
            // builtin names), then fall back to the plain builtin name.
            int bid = -1;
            if (const std::string* fn = nativeFunction(mb->aux, f.field))
              bid = builtinRuntimeId(*fn);
            if (bid < 0) bid = builtinRuntimeId(f.field);
            if (bid >= 0) {
              auto args = positionalArgs(call, out);
              std::string arr = buildArgv(args, false);
              return "a_bi_call(" + std::to_string(bid) + ", " +
                     std::to_string(args.size()) + ", " + arr + ")";
            }
            fail("native module function not supported: " + mb->aux + "." + f.field);
            return "A_NONE";
          }
          // File module (following __init__ re-exports). The member wins
          // over a builtin with the same name; builtins are the fallback.
          // Function in that module (following __init__ re-exports).
          auto rm = resolveModuleMember(mb->moduleId, f.field);
          for (FnInfo* fi : fnList_) {
            if (!fi->isMethod && fi->moduleId == rm.first && fi->name == rm.second) {
              auto args = orderArgs(call, fi->paramNames, out, fi->variadic ? (int)fi->paramNames.size()-1 : -1);
              std::string arr = buildArgv(args, true);
              return fi->cname + "(" + arr + ")";
            }
          }
          for (StructInfo* st : structList_) {
            if (st->moduleId == rm.first && st->name == rm.second) {
              auto args = orderArgs(call, st->fieldNames, out, -1, "A_NONE");
              std::string arr = buildArgv(args, true);
              return st->ctorCname + "(" + arr + ")";
            }
          }
          // Module-level let holding a callable: mod.handler(...).
          auto git = topLevelGlobals_.find(std::to_string(rm.first) + ":" + rm.second);
          if (git != topLevelGlobals_.end()) {
            auto args = positionalArgs(call, out);
            std::string arr = buildArgv(args, false);
            return "a_call(" + git->second + ", " + std::to_string(args.size()) +
                   ", " + arr + ")";
          }
          // Member not defined in the file module: fall back to the builtin
          // with the same name (std:math sqrt, etc.).
          {
            int bid = builtinRuntimeId(f.field);
            if (bid >= 0) {
              auto args = positionalArgs(call, out);
              std::string arr = buildArgv(args, false);
              return "a_bi_call(" + std::to_string(bid) + ", " +
                     std::to_string(args.size()) + ", " + arr + ")";
            }
          }
        }
      }
      // Struct method?
      TypePtr ot = resolveType(f.object->inferredType);
      if (ot && ot->kind == TypeKind::Struct) {
        auto sit = structs_.find(shortName(ot->structName));
        if (sit != structs_.end()) {
          auto mit = sit->second->methods.find(f.field);
          if (mit != sit->second->methods.end()) {
            std::string obj = emitExpr(f.object.get(), out);
            // Method params include `self` as param 0.
            std::vector<std::string> params = mit->second->paramNames;
            std::vector<std::string> restParams;
            if (!params.empty()) restParams.assign(params.begin() + 1, params.end());
            auto args = orderArgs(call, restParams, out, mit->second->variadic ? (int)restParams.size()-1 : -1);
            args.insert(args.begin(), obj);
            std::string arr = buildArgv(args, true);
            return mit->second->cname + "(" + arr + ")";
          }
        }
      }
      // Fallback: method name provided by exactly one struct (e.g. generic
      // or untyped receiver `value.size()`). Never hijack calls on values
      // whose static type is a known runtime collection/string (dict/list/
      // set/...): those go through a_method() dispatch.
      {
        bool isRuntimeValue = ot && ot->kind != TypeKind::Struct &&
                               ot->kind != TypeKind::Error &&
                               ot->kind != TypeKind::Var;
        FnInfo* candidate = nullptr;
        int candidates = 0;
        if (!isRuntimeValue && !isBuiltinValueMethod(f.field)) {
          for (StructInfo* st : structList_) {
            auto mit = st->methods.find(f.field);
            if (mit != st->methods.end()) { candidate = mit->second; ++candidates; }
          }
        }
        if (candidates == 1 && candidate) {
          std::string obj = emitExpr(f.object.get(), out);
          std::vector<std::string> params = candidate->paramNames;
          std::vector<std::string> restParams;
          if (!params.empty()) restParams.assign(params.begin() + 1, params.end());
          auto args = orderArgs(call, restParams, out, candidate->variadic ? (int)restParams.size()-1 : -1);
          args.insert(args.begin(), obj);
          std::string arr = buildArgv(args, true);
          return candidate->cname + "(" + arr + ")";
        }
      }
      // Generic method call.
      std::string obj = emitExpr(f.object.get(), out);
      auto args = positionalArgs(call, out);
      std::string arr = buildArgv(args, false);
      return "a_method(" + obj + ", \"" + cEscape(f.field) + "\", " +
             std::to_string(args.size()) + ", 0, " + arr + ")";
    }
    // Indirect call.
    std::string fn = emitExpr(callee, out);
    auto args = positionalArgs(call, out);
    std::string arr = buildArgv(args, false);
    return "a_call(" + fn + ", " + std::to_string(args.size()) + ", " + arr + ")";
  }

  // ---- lambdas ------------------------------------------------------------
  void collectRefs(const Expr* e, std::unordered_set<std::string>& bound,
                   std::vector<std::string>& refs, std::unordered_set<std::string>& seen) {
    if (!e) return;
    auto addRef = [&](const std::string& n) {
      if (!bound.count(n) && !seen.count(n)) {
        seen.insert(n);
        refs.push_back(n);
      }
    };
    switch (e->kind) {
      case ExprKind::Ident:
        addRef(static_cast<const IdentExpr&>(*e).name);
        return;
      case ExprKind::Lambda: {
        auto& l = static_cast<const LambdaExpr&>(*e);
        std::unordered_set<std::string> inner = bound;
        for (auto& p : l.params) inner.insert(p.name);
        collectRefs(l.body.get(), inner, refs, seen);
        return;
      }
      case ExprKind::Comprehension: {
        auto& c = static_cast<const ComprehensionExpr&>(*e);
        collectRefs(c.iterable.get(), bound, refs, seen);
        std::unordered_set<std::string> inner = bound;
        inner.insert(c.var);
        collectRefs(c.element.get(), inner, refs, seen);
        collectRefs(c.condition.get(), inner, refs, seen);
        return;
      }
      case ExprKind::Match: {
        auto& m = static_cast<const MatchExpr&>(*e);
        collectRefs(m.subject.get(), bound, refs, seen);
        for (auto& arm : m.arms) {
          std::unordered_set<std::string> inner = bound;
          collectPatternRefs(arm.pattern, inner);
          collectRefs(arm.guard.get(), inner, refs, seen);
          collectRefs(arm.result.get(), inner, refs, seen);
        }
        return;
      }
      default:
        break;
    }
    // Generic traversal of child expressions.
    for (const Expr* child : childExprs(e)) collectRefs(child, bound, refs, seen);
  }

  void collectPatternRefs(const Pattern& p, std::unordered_set<std::string>& bound) {
    if (p.kind == Pattern::Kind::Binding) bound.insert(p.name);
    if (p.value) {
      std::vector<std::string> r;
      std::unordered_set<std::string> s;
      collectRefs(p.value.get(), bound, r, s);
    }
    for (auto& sub : p.subpatterns) collectPatternRefs(sub, bound);
  }

  std::vector<const Expr*> childExprs(const Expr* e) {
    std::vector<const Expr*> out;
    auto add = [&](const ExprPtr& p) { if (p) out.push_back(p.get()); };
    switch (e->kind) {
      case ExprKind::Unary: add(static_cast<const UnaryExpr*>(e)->operand); break;
      case ExprKind::Binary: add(static_cast<const BinaryExpr*>(e)->lhs); add(static_cast<const BinaryExpr*>(e)->rhs); break;
      case ExprKind::Call: {
        auto& c = static_cast<const CallExpr&>(*e);
        add(c.callee);
        for (auto& a : c.args) add(a.value);
        break;
      }
      case ExprKind::Index: add(static_cast<const IndexExpr*>(e)->object); add(static_cast<const IndexExpr*>(e)->index); break;
      case ExprKind::Slice: add(static_cast<const SliceExpr*>(e)->object); add(static_cast<const SliceExpr*>(e)->start); add(static_cast<const SliceExpr*>(e)->end); add(static_cast<const SliceExpr*>(e)->step); break;
      case ExprKind::Field: add(static_cast<const FieldExpr*>(e)->object); break;
      case ExprKind::SafeNav: add(static_cast<const SafeNavExpr*>(e)->object); break;
      case ExprKind::ArrayLit: for (auto& el : static_cast<const ArrayLitExpr*>(e)->elements) add(el); break;
      case ExprKind::Tuple: for (auto& el : static_cast<const TupleExpr*>(e)->elements) add(el); break;
      case ExprKind::MapLit: for (auto& kv : static_cast<const MapLitExpr*>(e)->entries) { add(kv.first); add(kv.second); } break;
      case ExprKind::SetLit: for (auto& el : static_cast<const SetLitExpr*>(e)->elements) add(el); break;
      case ExprKind::Range: add(static_cast<const RangeExpr*>(e)->start); add(static_cast<const RangeExpr*>(e)->end); add(static_cast<const RangeExpr*>(e)->step); break;
      case ExprKind::Ternary: add(static_cast<const TernaryExpr*>(e)->cond); add(static_cast<const TernaryExpr*>(e)->thenValue); add(static_cast<const TernaryExpr*>(e)->elseValue); break;
      default: break;
    }
    return out;
  }

  std::string emitLambdaExpr(const LambdaExpr& lam, std::ostringstream& out) {
    (void)out;
    std::unordered_set<std::string> bound;
    for (auto& p : lam.params) bound.insert(p.name);
    std::vector<std::string> refs;
    std::unordered_set<std::string> seen;
    collectRefs(lam.body.get(), bound, refs, seen);
    LambdaJob job;
    job.decl = &lam;
    job.moduleId = curModuleId_;
    job.cname = "__a_lam" + std::to_string(counter_++);
    for (auto& name : refs) {
      const Binding* b = lookup(name);
      if (b && (b->kind == BKind::Local || b->kind == BKind::Param || b->kind == BKind::Capture)) {
        job.captures.push_back({name, *b});
      }
    }
    std::string env;
    if (job.captures.empty()) {
      env = "A_NONE";
    } else {
      std::string ev = temp("env");
      pre("aval " + ev + " = a_closure_env_new(" + std::to_string(job.captures.size()) + ");");
      for (std::size_t i = 0; i < job.captures.size(); ++i) {
        pre("a_env_set(" + ev + ", " + std::to_string(i) + ", " +
            bindingExpr(job.captures[i].second) + ");");
      }
      env = ev;
    }
    lambdaQueue_.push_back(std::move(job));
    return "a_fnon_new((uint64_t)(uintptr_t)&" + lambdaQueue_.back().cname + ", " + env +
           ", \"lambda\")";
  }

  // ---- comprehension / match ---------------------------------------------
  std::string emitComprehension(const ComprehensionExpr& c, std::ostringstream& out) {
    std::string res = temp("comp");
    std::string itv = temp("it");
    std::string var = fresh("v");
    std::string iter = emitExpr(c.iterable.get(), out);
    pre("aval " + res + " = " + (c.isSet ? std::string("a_set_new()") : std::string("a_list_new(0, NULL)")) + ";");
    pre("aval " + itv + " = a_iter_new(" + iter + ");");
    pre("aval " + var + ";");
    pre("while (a_iter_next(" + itv + ", &" + var + ")) {");
    pushScope();
    bindLocal(c.var, var);
    if (c.condition) {
      std::string cond = emitExpr(c.condition.get(), out);
      pre("if (a_truthy_raw(" + cond + ")) {");
    }
    std::string elem = emitExpr(c.element.get(), out);
    if (c.isSet)
      pre("a_set_add(" + res + ", " + elem + ");");
    else
      pre("a_list_push(" + res + ", " + elem + ");");
    if (c.condition) pre("}");
    popScope();
    pre("}");
    return res;
  }

  std::string bindPattern(const Pattern& p, const std::string& subj, std::ostringstream& out) {
    // Returns a C test expression; bindings are declared in prelude/scope as
    // a side effect via the caller (see emitMatch).
    (void)out;
    switch (p.kind) {
      case Pattern::Kind::Wildcard:
        return "1";
      case Pattern::Kind::Binding:
        return "1";
      case Pattern::Kind::Literal: {
        std::string v = emitPatternLiteral(p, out);
        return "a_truthy_raw(a_eq(" + subj + ", " + v + "))";
      }
      case Pattern::Kind::Range: {
        std::string lo = emitExpr(p.rangeStart.get(), out);
        std::string hi = emitExpr(p.rangeEnd.get(), out);
        return "a_truthy_raw(a_in_range(" + subj + ", " + lo + ", " + hi + "))";
      }
      case Pattern::Kind::Variant: {
        std::string v = emitExpr(p.ctor.get(), out);
        if (p.subpatterns.empty()) {
          return "a_truthy_raw(a_eq(" + subj + ", " + v + "))";
        }
        // Algebraic: compare __tag dict entry with variant tag value.
        return "a_truthy_raw(a_eq(a_dict_get(" + subj + ", a_str_new(\"__tag\", 5), A_NONE), " + v + "))";
      }
      case Pattern::Kind::Tuple: {
        std::string test = "a_truthy_raw(a_eq(a_len(" + subj + "), a_mkint(" +
                           std::to_string(p.subpatterns.size()) + ")))";
        for (std::size_t i = 0; i < p.subpatterns.size(); ++i) {
          std::string elem = temp("elem");
          pre("aval " + elem + " = a_getitem(" + subj + ", a_mkint(" + std::to_string(i) + "));");
          std::string sub = bindPattern(p.subpatterns[i], elem, out);
          test = "(" + test + " && " + sub + ")";
        }
        return test;
      }
      case Pattern::Kind::Or: {
        std::string test = "0";
        for (auto& sub : p.subpatterns) {
          std::string t = bindPattern(sub, subj, out);
          test = "(" + test + " || " + t + ")";
        }
        return test;
      }
    }
    return "0";
  }

  std::string emitPatternLiteral(const Pattern& p, std::ostringstream& out) {
    if (p.value) return emitExpr(p.value.get(), out);
    return "A_NONE";
  }

  void declarePatternBindings(const Pattern& p, const std::string& subj) {
    switch (p.kind) {
      case Pattern::Kind::Binding: {
        std::string cname = fresh("v");
        pre("aval " + cname + " = " + subj + ";");
        bindLocal(p.name, cname);
        return;
      }
      case Pattern::Kind::Tuple: {
        for (std::size_t i = 0; i < p.subpatterns.size(); ++i) {
          std::string elem = temp("elem");
          pre("aval " + elem + " = a_getitem(" + subj + ", a_mkint(" + std::to_string(i) + "));");
          declarePatternBindings(p.subpatterns[i], elem);
        }
        return;
      }
      case Pattern::Kind::Variant: {
        for (auto& sub : p.subpatterns) {
          if (sub.kind == Pattern::Kind::Binding) {
            std::string cname = fresh("v");
            pre("aval " + cname + " = a_dict_get(" + subj + ", a_str_new(\"" +
                cEscape(sub.name) + "\", " + std::to_string(sub.name.size()) + "), A_NONE);");
            bindLocal(sub.name, cname);
          }
        }
        return;
      }
      case Pattern::Kind::Or:
        for (auto& sub : p.subpatterns) declarePatternBindings(sub, subj);
        return;
      default:
        return;
    }
  }

  std::string emitMatch(const MatchExpr& m, std::ostringstream& out) {
    std::string subj = emitExpr(m.subject.get(), out);
    std::string subjT = temp("subj");
    std::string res = temp("match");
    pre("aval " + subjT + " = " + subj + ";");
    pre("aval " + res + ";");
    bool first = true;
    for (auto& arm : m.arms) {
      pushScope();
      declarePatternBindings(arm.pattern, subjT);
      std::string test = bindPattern(arm.pattern, subjT, out);
      std::string cond = test;
      if (arm.guard) {
        std::string g = emitExpr(arm.guard.get(), out);
        cond = "(" + test + " && a_truthy_raw(" + g + "))";
      }
      pre(std::string(first ? "if (" : "} else if (") + cond + ") {");
      std::string rv = emitExpr(arm.result.get(), out);
      pre(res + " = " + rv + ";");
      popScope();
      first = false;
    }
    if (!m.arms.empty()) pre("} else { " + res + " = a_match_fail(" + subjT + "); }");
    return res;
  }
};

}  // namespace

bool emitC(const ModuleGraph& graph, const SemaResult& sema, std::string& out,
           std::string& error, bool testMode) {
  Emitter emitter(graph, sema, testMode);
  return emitter.run(out, error);
}

}  // namespace a
