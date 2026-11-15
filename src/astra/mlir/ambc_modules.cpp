//===- ambc_modules.cpp - per-module .ambc package consumer ---------*- C++ -*-===//
//
// Part of the A toolchain (src/astra/mlir). See docs/mlir-architecture.md and
// the "Consumer spec" in src/astra/mlir/package.hpp.
//
// Consumes a per-module `.ambc` package directory (ambc.meta + one .ambc per
// ModuleGraph module, produced by precompilePackageModules): merges every
// module's ops into ONE mlir::ModuleOp and synthesizes the program-global
// tail that per-module emission deliberately omits (runtime decls,
// __a_main_body orchestration, __a_gc_root_mark, main). See
// ambc_modules.hpp for the contract and the entry-body limitation.
//
//===----------------------------------------------------------------------===//

#include "mlir/ambc_modules.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/OwningOpRef.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Pass/PassManager.h"

#include "bytecode.hpp"   // readModuleBytecode / bytecodeIsCompatible
#include "codegen.hpp"    // mlirModuleToObject
#include "native.hpp"     // parseMlirText, registerCoreDialects
#include "pipeline.hpp"   // registerAllDialects, registerAPasses, buildAOptPipeline

namespace a {
namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Runtime (`a_*`) function signatures.
//
// KEEP IN SYNC with kRuntimeSigs in
// src/astra/frontend/mlir/mlir_emit.cpp (~line 67): that table is file-local
// to the emitter, so it is duplicated here. Both feed runtimeDeclText-style
// "(args) -> ret" signature strings rendered as bare `llvm.func` declarations
// (no linkage, no body) exactly like whole-graph emitDecls.
// ---------------------------------------------------------------------------
const std::pair<const char*, const char*> kAmbcRuntimeSigs[] = {
    {"a_add_slow", "(i64, i64) -> i64"}, {"a_sub_slow", "(i64, i64) -> i64"},
    {"a_mul_slow", "(i64, i64) -> i64"}, {"a_div_slow", "(i64, i64) -> i64"},
    {"a_floordiv_slow", "(i64, i64) -> i64"},
    {"a_mod_slow", "(i64, i64) -> i64"}, {"a_pow", "(i64, i64) -> i64"},
    {"a_eq_slow", "(i64, i64) -> i64"}, {"a_ne_slow", "(i64, i64) -> i64"},
    {"a_lt_slow", "(i64, i64) -> i64"}, {"a_le_slow", "(i64, i64) -> i64"},
    {"a_gt_slow", "(i64, i64) -> i64"}, {"a_ge_slow", "(i64, i64) -> i64"},
    {"a_band_slow", "(i64, i64) -> i64"}, {"a_bor_slow", "(i64, i64) -> i64"},
    {"a_bxor_slow", "(i64, i64) -> i64"}, {"a_shl_slow", "(i64, i64) -> i64"},
    {"a_shr_slow", "(i64, i64) -> i64"}, {"a_neg_slow", "(i64) -> i64"},
    {"a_bnot_slow", "(i64) -> i64"}, {"a_not", "(i64) -> i64"},
    {"a_truthy_raw_slow", "(i64) -> i64"},
    {"a_unwrap", "(i64) -> i64"}, {"a_some_new", "(i64) -> i64"},
    {"a_try_result", "(i64) -> i64"}, {"a_is_option", "(i64) -> i32"},
    {"a_is_propagate", "(i64) -> i32"}, {"a_str", "(i64) -> i64"},
    {"a_repr", "(i64) -> i64"}, {"a_len", "(i64) -> i64"},
    {"a_int", "(i64) -> i64"}, {"a_float", "(i64) -> i64"},
    {"a_bool", "(i64) -> i64"}, {"a_type_name", "(i64) -> i64"},
    {"a_int_full", "(i64) -> i64"}, {"a_i64n", "(i64) -> i64"},
    {"a_str_new", "(!llvm.ptr, i64) -> i64"},
    {"a_char_new", "(!llvm.ptr, i64) -> i64"},
    {"a_str_concat", "(i64, i64) -> i64"},
    {"a_str_join", "(i64, !llvm.ptr) -> i64"},
    {"a_str_joinsep", "(i64, i64, !llvm.ptr) -> i64"},
    {"a_list_new", "(i64, !llvm.ptr) -> i64"},
    {"a_list_append", "(i64, i64) -> i64"},
    {"a_list_append_int", "(i64, i64) -> i64"},
    {"a_list_from_iter", "(i64) -> i64"},
    {"a_tuple_new", "(i64, !llvm.ptr) -> i64"},
    {"a_dict_new", "() -> i64"}, {"a_dict_set", "(i64, i64, i64) -> i64"},
    {"a_dict_get", "(i64, i64, i64) -> i64"},
    {"a_dict_has", "(i64, i64) -> i64"},
    {"a_set_new", "() -> i64"}, {"a_set_add", "(i64, i64) -> i64"},
    {"a_range_new", "(i64, i64, i64) -> i64"},
    {"a_range_bound", "(i64, i64) -> i64"},
    {"a_getitem", "(i64, i64) -> i64"},
    {"a_setitem", "(i64, i64, i64) -> i64"},
    {"a_slice", "(i64, i64, i64, i64) -> i64"},
    {"a_iter_new", "(i64) -> i64"},
    {"a_iter_next", "(i64, !llvm.ptr) -> i64"},
    {"a_contains", "(i64, i64) -> i64"},
    {"a_fnon_new", "(i64, i64, !llvm.ptr) -> i64"},
    {"a_closure_env_new", "(i64) -> i64"},
    {"a_env_new", "(i64) -> i64"}, {"a_env_get", "(i64, i64) -> i64"},
    {"a_env_set", "(i64, i64, i64) -> i64"},
    {"a_struct_new", "(i64, i64, i64) -> i64"},
    {"a_builtin", "(i64) -> i64"},
    {"a_bi_call", "(i64, i64, !llvm.ptr) -> i64"},
    {"a_call", "(i64, i64, !llvm.ptr) -> i64"},
    {"a_method", "(i64, !llvm.ptr, i64, i64, !llvm.ptr) -> i64"},
    {"a_getattr", "(i64, !llvm.ptr) -> i64"},
    {"a_setattr", "(i64, !llvm.ptr, i64) -> i64"},
    {"a_promote", "(i64) -> i64"}, {"a_panic_msg", "(i64) -> i64"},
    {"a_join_len", "(i64) -> i64"}, {"a_match_fail", "(i64) -> i64"},
    {"a_frame_push", "(!llvm.ptr, !llvm.ptr, i32) -> ()"},
    {"a_frame_pop", "() -> ()"},
    {"a_pin_slot", "(!llvm.ptr) -> ()"}, {"a_runtime_init", "() -> ()"},
    {"a_fib_main", "(i64, i64, i64, !llvm.ptr) -> i64"},
    {"a_fib_spawn_handle", "(i64, i64, !llvm.ptr) -> i64"},
    {"a_fib_join_handle", "(i64) -> i64"},
    {"a_test_main", "(i64, !llvm.ptr, !llvm.ptr) -> i32"},
    {"a_printv", "(i64, !llvm.ptr) -> ()"},
    {"a_eprintv", "(i64, !llvm.ptr) -> ()"},
    {"a_println", "(i64) -> ()"}, {"a_sleep", "(i64) -> ()"},
    {"a_now", "() -> i64"}, {"a_gc_collect", "() -> ()"},
    {"a_gc_mark_aval", "(i64) -> ()"}, {"a_gc_add_root_cb", "(!llvm.ptr) -> ()"},
};

// Renders "llvm.func @<name>(<typed args>) -> <ret>" for a compact
// "(args) -> ret" signature (mirrors runtimeDeclText in mlir_emit.cpp).
std::string runtimeDeclText(const char* name, const char* sig) {
  std::string s = sig;
  std::string args = s.substr(1, s.find(')') - 1);
  std::string ret = s.substr(s.find("->") + 3);
  while (!ret.empty() && ret.front() == ' ') ret.erase(ret.begin());
  std::vector<std::string> tys;
  {
    std::string cur;
    int depth = 0;
    for (char ch : args) {
      if (ch == '<') ++depth;
      if (ch == '>') --depth;
      if (ch == ',' && depth == 0) {
        tys.push_back(cur);
        cur.clear();
      } else {
        cur += ch;
      }
    }
    if (!cur.empty()) tys.push_back(cur);
  }
  std::string decl = "llvm.func @";
  decl += name;
  decl += "(";
  for (size_t i = 0; i < tys.size(); ++i) {
    if (i) decl += ", ";
    decl += "%a" + std::to_string(i) + ": " + tys[i];
  }
  decl += ")";
  if (ret != "()") decl += " -> " + ret;
  decl += "\n";
  return decl;
}

// ---------------------------------------------------------------------------
// ambc.meta parsing
// ---------------------------------------------------------------------------

struct AmbcMeta {
  int entry = -1;
  int prelude = -1;
  std::vector<int> initOrder;
  // module id -> .ambc file name (relative to pkgDir)
  std::vector<std::pair<int, std::string>> modules;
};

std::string trim(const std::string& s) {
  size_t b = 0;
  while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  size_t e = s.size();
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

std::vector<std::string> splitTabs(const std::string& s) {
  std::vector<std::string> out;
  size_t pos = 0;
  while (true) {
    size_t tab = s.find('\t', pos);
    if (tab == std::string::npos) {
      out.push_back(s.substr(pos));
      break;
    }
    out.push_back(s.substr(pos, tab - pos));
    pos = tab + 1;
  }
  return out;
}

bool parseAmbcMeta(const fs::path& pkgDir, AmbcMeta& meta,
                   std::ostream& err) {
  std::ifstream in(pkgDir / "ambc.meta", std::ios::binary);
  if (!in) {
    err << "error: ambc package: cannot read '"
        << (pkgDir / "ambc.meta") << "'\n";
    return false;
  }
  std::string line;
  bool sawFormat = false;
  while (std::getline(in, line)) {
    std::string t = trim(line);
    if (t.empty() || t[0] == '#') continue;
    if (t.rfind("format=", 0) == 0) {
      if (t != "format=ambc-modules-v1") {
        err << "error: ambc package: unsupported ambc.meta format '" << t
            << "'\n";
        return false;
      }
      sawFormat = true;
      continue;
    }
    if (t.rfind("entry=", 0) == 0) {
      try { meta.entry = std::stoi(t.substr(6)); } catch (...) {}
      continue;
    }
    if (t.rfind("prelude=", 0) == 0) {
      try { meta.prelude = std::stoi(t.substr(8)); } catch (...) {}
      continue;
    }
    if (t.rfind("init_order=", 0) == 0) {
      std::string rest = t.substr(11);
      size_t pos = 0;
      while (pos <= rest.size()) {
        size_t comma = rest.find(',', pos);
        std::string tok = rest.substr(
            pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (!tok.empty()) {
          try { meta.initOrder.push_back(std::stoi(tok)); } catch (...) {}
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
      }
      continue;
    }
    if (t.rfind("module_count=", 0) == 0 || t.rfind("compiler=", 0) == 0) {
      continue;
    }
    if (t.rfind("module ", 0) == 0 || t == "module") {
      // "module <id>\t<ambc_file>\t<source>\t<name>"
      std::string rest = trim(t.substr(6));
      size_t sp = rest.find_first_of(" \t");
      if (sp == std::string::npos) continue;
      int id = -1;
      try { id = std::stoi(rest.substr(0, sp)); } catch (...) {}
      std::vector<std::string> fields = splitTabs(rest.substr(sp + 1));
      if (id >= 0 && !fields.empty() && !fields[0].empty()) {
        meta.modules.emplace_back(id, fields[0]);
      }
      continue;
    }
    // Unknown key: ignore (forward compat).
  }
  if (!sawFormat) {
    err << "error: ambc package: '" << (pkgDir / "ambc.meta")
        << "' is not an ambc-modules-v1 descriptor\n";
    return false;
  }
  if (meta.modules.empty()) {
    err << "error: ambc package: '" << (pkgDir / "ambc.meta")
        << "' lists no modules\n";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Merge helpers
// ---------------------------------------------------------------------------

// True for the emitter's per-module private literal globals. Their counters
// (`strCounter_` in mlir_emit.cpp) restart at 0 for every emitModuleMLIR
// call, so `__a_str0`, `__a_chr0`, `__a_cstr0`, ... are defined once per
// module and MUST be renamed before merging into a single ModuleOp (MLIR
// symbol tables require unique names even for private linkage).
// `__a_g_<id>_*` globals are already module-mangled and never collide.
bool isCollidingPrivateGlobal(mlir::LLVM::GlobalOp g) {
  if (g.getLinkage() != mlir::LLVM::Linkage::Private) return false;
  llvm::StringRef name = g.getSymName();
  return name.starts_with("__a_str") || name.starts_with("__a_chr") ||
         name.starts_with("__a_cstr");
}

// Rename every colliding private global in `module` to
// `<name>_m<moduleId>` and rewrite all symbol uses inside the module.
void uniquifyModuleGlobals(mlir::ModuleOp module, int moduleId) {
  llvm::StringRef suffix = "_m" + llvm::Twine(moduleId).str();
  // Collect first: renaming mutates the symbol table.
  std::vector<mlir::LLVM::GlobalOp> globals;
  for (mlir::LLVM::GlobalOp g : module.getOps<mlir::LLVM::GlobalOp>()) {
    if (isCollidingPrivateGlobal(g)) globals.push_back(g);
  }
  for (mlir::LLVM::GlobalOp g : globals) {
    std::string fresh = (g.getSymName() + suffix).str();
    // Guard against a freak collision with a real symbol.
    int n = 0;
    std::string candidate = fresh;
    while (mlir::SymbolTable::lookupNearestSymbolFrom(
               module, mlir::StringAttr::get(module.getContext(), candidate))) {
      candidate = fresh + "_" + std::to_string(++n);
    }
    mlir::StringAttr newName =
        mlir::StringAttr::get(module.getContext(), candidate);
    mlir::StringAttr oldName = g.getSymNameAttr();
    mlir::SymbolTable symTable(module);
    if (mlir::failed(symTable.rename(g.getOperation(), newName))) {
      // Fallback: rename the symbol attr by hand and rewrite every use.
      g.setSymName(newName.getValue());
      (void)mlir::SymbolTable::replaceAllSymbolUses(oldName, newName, module);
    }
  }
}

// Move every op out of `src`'s body into `dst`'s body (append order).
void moveModuleOps(mlir::ModuleOp dst, mlir::ModuleOp src) {
  mlir::Block* dstBody = &dst.getBodyRegion().front();
  mlir::Block* srcBody = &src.getBodyRegion().front();
  while (!srcBody->empty()) {
    mlir::Operation* op = &srcBody->front();
    op->moveBefore(dstBody, dstBody->end());
  }
}

// Deduplicate `llvm.func` ops by symbol name across the merged module.
// Every per-module unit re-declares the `a_*` runtime functions (and may
// re-declare foreign `__a_f_m*`/`__a_m*` symbols) as body-less declarations;
// the module that owns a symbol also carries its definition. Keep the
// definition when present, else the first declaration; erase the rest.
void dedupLlvmFuncDecls(mlir::ModuleOp module) {
  struct Entry {
    mlir::LLVM::LLVMFuncOp op;
    bool hasBody = false;
  };
  std::unordered_map<std::string, Entry> best;
  std::vector<mlir::LLVM::LLVMFuncOp> all;
  for (mlir::LLVM::LLVMFuncOp fn : module.getOps<mlir::LLVM::LLVMFuncOp>()) {
    all.push_back(fn);
  }
  for (mlir::LLVM::LLVMFuncOp fn : all) {
    std::string name = fn.getSymName().str();
    bool hasBody = !fn.getBody().empty();
    auto it = best.find(name);
    if (it == best.end()) {
      best.emplace(name, Entry{fn, hasBody});
      continue;
    }
    Entry& cur = it->second;
    if (!cur.hasBody && hasBody) {
      cur.op->erase();
      cur.op = fn;
      cur.hasBody = true;
    } else {
      fn->erase();
    }
  }
}

// Deduplicate `llvm.mlir.global` ops by symbol name. The synthesized tail
// re-declares every `__a_g_*` global (external linkage, no initializer) so
// its `llvm.mlir.addressof` uses resolve at parse time; the per-module units
// already carry identical declarations. Keep the first occurrence (module
// decls precede the appended tail ops) and erase the rest. Globals with
// initializers (the private `__a_cstr`/`__a_str`/`__a_chr` constants) have
// unique names and are unaffected.
void dedupLlvmGlobals(mlir::ModuleOp module) {
  std::unordered_set<std::string> seen;
  std::vector<mlir::LLVM::GlobalOp> all;
  for (mlir::LLVM::GlobalOp g : module.getOps<mlir::LLVM::GlobalOp>()) {
    all.push_back(g);
  }
  for (mlir::LLVM::GlobalOp g : all) {
    if (!seen.insert(g.getSymName().str()).second) {
      g->erase();
    }
  }
}

// ---------------------------------------------------------------------------
// Tail synthesis (mirrors emitDecls/emitMainBody/emitGcRoots/emitMain in
// src/astra/frontend/mlir/mlir_emit.cpp for the merged whole-program view)
// ---------------------------------------------------------------------------

std::string synthesizeTailText(const AmbcMeta& meta,
                               const std::vector<int>& initOrder,
                               const std::string& entryBodyMlir,
                               mlir::ModuleOp module) {
  std::ostringstream t;
  t << "module {\n";

  // 1. Runtime declaration block (whole-graph emitDecls).
  for (const auto& kv : kAmbcRuntimeSigs) t << runtimeDeclText(kv.first, kv.second);

  // 2. __a_main_body: module inits in init_order, then the entry body.
  //    (whole-graph emitMainBody inlines global inits + top stmts + entry
  //    body; per-module .ambc moves the first two into __a_m<id>_init.)
  //    Declare each __a_m<id>_init first: the tail parses in isolation, so
  //    calls need an in-scope declaration even though the merged module
  //    already carries the definitions.
  for (int id : initOrder) {
    t << "llvm.func @__a_m" << id << "_init(%a0: !llvm.ptr) -> i64\n";
  }
  t << "llvm.func @__a_main_body(%argv: !llvm.ptr) -> i64 {\n";
  int v = 0;
  auto nv = [&]() { return "%mb" + std::to_string(v++); };
  for (int id : initOrder) {
    std::string sym = "__a_m" + std::to_string(id) + "_init";
    std::string r = nv();
    t << "  " << r << " = llvm.call @" << sym << "(%argv) : (!llvm.ptr) -> i64\n";
  }
  if (!entryBodyMlir.empty()) {
    t << entryBodyMlir;
    if (entryBodyMlir.back() != '\n') t << "\n";
  }
  // endFunction default: return the "none" aval (kAvalTag | 0x2).
  std::string noneName = nv();
  t << "  " << noneName << " = llvm.mlir.constant(-281474976710654 : i64) : i64\n";
  t << "  llvm.return " << noneName << " : i64\n";
  t << "}\n\n";

  // 3. __a_gc_root_mark over every __a_g_* global (emitGcRoots). Only
  //    __a_g_* globals hold avals; __a_str/__a_chr/__a_cstr are byte arrays.
  std::vector<std::string> rootSyms;
  for (mlir::LLVM::GlobalOp g : module.getOps<mlir::LLVM::GlobalOp>()) {
    llvm::StringRef name = g.getSymName();
    if (name.starts_with("__a_g_")) rootSyms.push_back(name.str());
  }
  std::sort(rootSyms.begin(), rootSyms.end());
  rootSyms.erase(std::unique(rootSyms.begin(), rootSyms.end()), rootSyms.end());
  if (!rootSyms.empty()) {
    // Declare each __a_g_* global in the tail so `llvm.mlir.addressof`
    // resolves at parse time (the tail parses in isolation). These are
    // external-linkage declarations identical to the ones the per-module
    // units already carry; dedupLlvmGlobals removes the duplicates after
    // the splice.
    for (const std::string& sym : rootSyms) {
      t << "llvm.mlir.global external @" << sym
        << "(0 : i64) {addr_space = 0 : i32} : i64\n";
    }
    t << "llvm.func @__a_gc_root_mark() -> () {\n";
    for (const std::string& sym : rootSyms) {
      t << "  %r_" << sym << " = llvm.mlir.addressof @" << sym << " : !llvm.ptr\n";
      t << "  %v_" << sym << " = llvm.load %r_" << sym << " : !llvm.ptr -> i64\n";
      t << "  llvm.call @a_gc_mark_aval(%v_" << sym << ") : (i64) -> ()\n";
    }
    t << "  llvm.return\n}\n\n";
  }

  // 4. main (whole-graph emitMain, non-test mode).
  t << "llvm.func @main(%argc: i32, %argv0: !llvm.ptr) -> i32 {\n";
  t << "  llvm.call @a_runtime_init() : () -> ()\n";
  if (!rootSyms.empty()) {
    t << "  %gr0 = llvm.mlir.addressof @__a_gc_root_mark : !llvm.ptr\n";
    t << "  llvm.call @a_gc_add_root_cb(%gr0) : (!llvm.ptr) -> ()\n";
  }
  t << "  %fnp0 = llvm.mlir.addressof @__a_main_body : !llvm.ptr\n";
  t << "  %fnp1 = llvm.ptrtoint %fnp0 : !llvm.ptr to i64\n";
  t << "  %cnone = llvm.mlir.constant(-281474976710654 : i64) : i64\n";
  t << "  %czero = llvm.mlir.constant(0 : i64) : i64\n";
  t << "  %nullp = llvm.mlir.zero : !llvm.ptr\n";
  t << "  %r = llvm.call @a_fib_main(%fnp1, %cnone, %czero, %nullp)"
        " : (i64, i64, i64, !llvm.ptr) -> i64\n";
  t << "  %c0 = llvm.mlir.constant(0 : i32) : i32\n";
  t << "  llvm.return %c0 : i32\n";
  t << "}\n\n";
  t << "}\n";

  (void)meta;
  return t.str();
}

} // namespace

int ambcModulesLoad(const std::string& pkgDir,
                    const std::string& entryBodyMlir, bool testMode,
                    AmbcModules& pkg, std::ostream& err) {
  if (testMode) {
    err << "error: per-module .ambc packages do not support `a test` "
           "(testFilter is not recorded in ambc.meta); build a whole-graph "
           ".ambc or compile from source instead\n";
    return 1;
  }

  fs::path dir(pkgDir);
  AmbcMeta meta;
  if (!parseAmbcMeta(dir, meta, err)) return 1;

  // Fresh context per package (same rationale as ambcCompileToObject):
  // bytecode modules are self-contained and a clean context avoids
  // cross-package symbol/attribute collisions.
  auto ctx = std::make_unique<mlir::MLIRContext>();
  registerAllDialects(*ctx);
  registerCoreDialects(*ctx);

  // Read every module .ambc into an OwningOpRef; keep them alive until all
  // ops are moved into the merged module.
  std::vector<mlir::OwningOpRef<mlir::ModuleOp>> modules;
  modules.reserve(meta.modules.size());
  const bool dbg = []{ const char* e = std::getenv("A_AMBC_DEBUG"); return e && *e; }();
  for (const auto& [id, file] : meta.modules) {
    fs::path ambcPath = dir / file;
    std::string reason;
    if (!bytecodeIsCompatible(ambcPath.string(), reason)) {
      err << "error: ambc package module '" << ambcPath.string()
          << "' is not usable: " << reason << "\n";
      return 1;
    }
    std::string readError;
    mlir::OwningOpRef<mlir::ModuleOp> m =
        readModuleBytecode(*ctx, ambcPath.string(), readError);
    if (!m) {
      err << "error: failed to read .ambc module '" << ambcPath.string()
          << "': " << readError << "\n";
      return 1;
    }
    modules.push_back(std::move(m));
    if (dbg) {
      llvm::errs() << "ambc module id=" << id << " file=" << file
                   << " ops=" << (*modules.back()).getBodyRegion().front().getOperations().size()
                   << "\n";
    }
  }
  if (modules.empty()) {
    err << "error: ambc package '" << pkgDir << "' contains no modules\n";
    return 1;
  }

  // Rename colliding private globals per module BEFORE merging (each module
  // keeps its own counter namespace, so __a_str0/__a_cstr0/... repeat).
  for (size_t i = 0; i < modules.size(); ++i) {
    int id = meta.modules[i].first;
    uniquifyModuleGlobals(*modules[i], id);
  }

  // Merge: first module becomes the anchor; move the rest's ops into it.
  // Ownership stays with modules.front()'s OwningOpRef until the very end
  // (anchor is a non-owning wrapper); transferring the OwningOpRef itself
  // into pkg.module avoids a double-own/double-free.
  mlir::ModuleOp anchor = *modules.front();
  for (size_t i = 1; i < modules.size(); ++i) {
    moveModuleOps(anchor, *modules[i]);
  }

  dedupLlvmFuncDecls(anchor);

  // init_order drives __a_main_body's __a_m<id>_init call sequence. Append
  // any module ids present in the package but missing from init_order so a
  // hand-edited meta cannot silently skip initializers.
  std::vector<int> initOrder = meta.initOrder;
  {
    std::unordered_set<int> seen(initOrder.begin(), initOrder.end());
    for (const auto& [id, file] : meta.modules) {
      if (seen.insert(id).second) initOrder.push_back(id);
    }
  }

  // Synthesize + parse the global tail into the same context, then splice
  // its ops into the merged module.
  std::string tailText =
      synthesizeTailText(meta, initOrder, entryBodyMlir, anchor);
  if (const char* dbg = std::getenv("A_AMBC_DEBUG_TAIL")) {
    if (*dbg) {
      llvm::errs() << "===TAIL===\n" << tailText << "===END===\n";
    }
  }
  std::string parseError;
  mlir::OwningOpRef<mlir::ModuleOp> tail =
      parseMlirText(*ctx, tailText, parseError);
  if (!tail) {
    err << "error: failed to parse synthesized ambc tail MLIR: "
        << (parseError.empty() ? "unknown parse error" : parseError) << "\n";
    return 1;
  }
  moveModuleOps(anchor, *tail);
  // The synthesized tail adds declaration-only llvm.func ops for the
  // __a_m<id>_init symbols and external llvm.mlir.global decls for the
  // __a_g_* roots; dedup again so the modules' own ops win.
  dedupLlvmFuncDecls(anchor);
  dedupLlvmGlobals(anchor);

  // Fill the result. `anchor` is still owned by modules.front(); moving
  // that OwningOpRef transfers ownership cleanly (the emptied modules[1..]
  // and tail OwningOpRefs destruct harmlessly). Member order in
  // AmbcModules guarantees `module` is destroyed before `context`.
  pkg.context = std::move(ctx);
  pkg.module = std::move(modules.front());
  pkg.initOrder = initOrder;
  pkg.entryModule = meta.entry;
  return 0;
}

int ambcModulesCompileToObject(const std::string& pkgDir,
                               const std::string& arch, int optLevel,
                               const std::string& objPath,
                               const std::string& entryBodyMlir,
                               bool testMode, std::ostream& err) {
  AmbcModules pkg;
  if (ambcModulesLoad(pkgDir, entryBodyMlir, testMode, pkg, err) != 0) {
    return 1;
  }

  // Same opt/lowering pipeline as nativeCompileMlirTextToObject: canonical-
  // ize/CSE (+ mem2reg/SROA at optLevel>0, `a`->`llvm` lowering when the
  // `a` dialect is present - a no-op for llvm-dialect-only packages).
  mlir::PassManager pm(pkg.context.get());
  registerAPasses();
  buildAOptPipeline(pm, optLevel, /*emitLlvmDialect=*/true);
  if (mlir::failed(pm.run(pkg.module.get()))) {
    err << "error: mlir backend: MLIR pass pipeline failed for ambc package '"
        << pkgDir << "'\n";
    return 1;
  }

  return mlirModuleToObject(pkg.module.get(), arch, optLevel, objPath, err);
}

} // namespace a
