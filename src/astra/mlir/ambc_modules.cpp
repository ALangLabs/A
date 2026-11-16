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

#include "frontend/mlir/runtime_sigs.hpp"
#include "mlir/ambc_modules.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
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
#include "module_merge.hpp" // moveModuleOps / dedupLlvmFuncOps / dedupLlvmGlobalOps
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

// The ModuleOp merge/dedup helpers (moveModuleOps / dedupLlvmFuncOps /
// dedupLlvmGlobalOps) live in module_merge.hpp, shared with the .ambc
// producer in package.cpp. Semantics: dedupLlvmFuncOps keeps a definition
// over a declaration for each symbol name; dedupLlvmGlobalOps keeps the
// first `llvm.mlir.global` per symbol name (module decls precede the
// appended tail ops, so the tail's re-declarations lose).

// ---------------------------------------------------------------------------
// Tail synthesis (mirrors emitDecls/emitMainBody/emitGcRoots/emitMain in
// src/astra/frontend/mlir/mlir_emit.cpp for the merged whole-program view)
// ---------------------------------------------------------------------------

std::string synthesizeTailText(const AmbcMeta& meta,
                               const std::vector<int>& initOrder,
                               const std::string& entryBodyMlir,
                               mlir::ModuleOp module, bool testMode,
                               const std::string& testFilter,
                               std::vector<std::string>* testNameSyms,
                               std::vector<std::string>* testFnSyms) {
  std::ostringstream t;
  t << "module {\n";

  // 1. Runtime declaration block (whole-graph emitDecls).
  for (const auto& kv : runtime_sigs::kRuntimeSigs) t << runtimeDeclText(kv.first, kv.second);

  // 2. __a_main_body: module inits in init_order, then the entry body.
  //    (whole-graph emitMainBody inlines global inits + top stmts + entry
  //    body; per-module .ambc moves the first two into __a_m<id>_init.)
  //    Declare each __a_m<id>_init first: the tail parses in isolation, so
  //    calls need an in-scope declaration even though the merged module
  //    already carries the definitions.
  //    Under testMode the entry body is skipped — mirroring whole-graph
  //    emitMainBody, which drops the entry portion when testMode_ is set.
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
  if (!testMode && !entryBodyMlir.empty()) {
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
    // units already carry; dedupLlvmGlobalOps removes the duplicates after
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

  // 4. main: non-test mode emits the a_fib_main trampoline (whole-graph
  //    emitMain); test mode emits the a_test_main harness (whole-graph
  //    emitTestMain): run module inits on a fiber, then register the entry
  //    module's `test_*` fns with the runtime harness.
  if (!testMode) {
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
  } else {
    // Discover test fns: per-module units name them
    // `__a_f_m<moduleId>_<test_name>_<seq>` (MlirEmitter::collectTopLevel,
    // mlir_emit.cpp ~line 715); only definitions owned by the ENTRY module
    // are registered (whole-graph emitTestMain filters fn->moduleId ==
    // graph_.entry). The trailing `_<seq>` is digits; the recovered
    // `test_name` may itself contain underscores, so strip only the LAST
    // `_<digits>` segment.
    struct TestFn { std::string name; std::string sym; std::string nameGlobal; };
    std::vector<TestFn> tests;
    static const std::regex testFnRe(
        "^__a_f_m([0-9]+)_(.+)_([0-9]+)$");
    for (mlir::LLVM::LLVMFuncOp fn : module.getOps<mlir::LLVM::LLVMFuncOp>()) {
      if (fn.getBody().empty()) continue;  // declarations only
      std::string sym = fn.getSymName().str();
      std::smatch m;
      if (!std::regex_match(sym, m, testFnRe)) continue;
      int modId = -1;
      try { modId = std::stoi(m[1].str()); } catch (...) {}
      if (modId != meta.entry) continue;
      std::string name = m[2].str();
      if (name.rfind("test_", 0) != 0) continue;
      // Same predicate as the emitter: substring match on the source name.
      if (!testFilter.empty() && name.find(testFilter) == std::string::npos)
        continue;
      tests.push_back({name, sym, {}});
    }
    std::sort(tests.begin(), tests.end(),
              [](const TestFn& a, const TestFn& b) { return a.name < b.name; });

    // One NUL-terminated name global per test, emitted at module scope
    // before `main` (mirrors whole-graph emitTestMain's __a_cstr<N>
    // globals). `__a_tcstr` cannot collide with the per-module
    // `__a_cstr<N>` counters — but uniquify against the merged symbol
    // table anyway for safety. Private linkage keeps them out of
    // dedupLlvmGlobalOps (which only sees `__a_g_*`-style externals).
    if (!tests.empty()) {
      int g = 0;
      for (TestFn& tf : tests) {
        std::string cand;
        do {
          cand = "__a_tcstr" + std::to_string(g++);
        } while (mlir::SymbolTable::lookupNearestSymbolFrom(
                     module, mlir::StringAttr::get(module.getContext(),
                                                  cand)));
        tf.nameGlobal = cand;
        if (testNameSyms) testNameSyms->push_back(cand);
        if (testFnSyms) testFnSyms->push_back(tf.sym);
        // The tail parses standalone: `main`'s llvm.mlir.addressof on the
        // test fn requires an in-scope llvm.func declaration. Emit one
        // (uniform A-fn signature); dedupLlvmFuncOps keeps the module's
        // real definition after the splice — same pattern as the
        // __a_m*_init decls.
        t << "llvm.func @" << tf.sym << "(%argv: !llvm.ptr) -> i64\n";
        // MLIR string attribute: escape '\' and '"'; append \00 for the
        // C NUL terminator. Array length = bytes incl. terminator.
        std::string lit;
        for (char ch : tf.name) {
          if (ch == '\\' || ch == '"') lit += '\\';
          lit += ch;
        }
        lit += "\\00";
        t << "llvm.mlir.global private constant @" << cand << "(\"" << lit
          << "\") : !llvm.array<" << (tf.name.size() + 1)
          << " x i8>\n\n";
      }
    }

    t << "llvm.func @main(%argc: i32, %argv0: !llvm.ptr) -> i32 {\n";
    t << "  llvm.call @a_runtime_init() : () -> ()\n";
    // Module inits on a fiber — same as whole-graph emitTestMain. The
    // entry body is NOT run (emitMainBody skips it under test mode).
    t << "  %fnp0 = llvm.mlir.addressof @__a_main_body : !llvm.ptr\n";
    t << "  %fnp1 = llvm.ptrtoint %fnp0 : !llvm.ptr to i64\n";
    t << "  %cnone = llvm.mlir.constant(-281474976710654 : i64) : i64\n";
    t << "  %czero = llvm.mlir.constant(0 : i64) : i64\n";
    t << "  %nullp = llvm.mlir.zero : !llvm.ptr\n";
    t << "  %rinit = llvm.call @a_fib_main(%fnp1, %cnone, %czero, %nullp)"
          " : (i64, i64, i64, !llvm.ptr) -> i64\n";
    if (tests.empty()) {
      // Mirrors emitTestMain: no tests -> return 0 (the pipeline's
      // no-matching-filter diagnostic happens before compile, on the
      // source path; a stale/empty package just runs nothing).
      t << "  %c0 = llvm.mlir.constant(0 : i32) : i32\n";
      t << "  llvm.return %c0 : i32\n}\n\n";
    } else {
      const int n = (int)tests.size();
      // names/fns as stack arrays of !llvm.ptr (char* / fn-ptr elements).
      t << "  %tone = llvm.mlir.constant(1 : i64) : i64\n";
      t << "  %names = llvm.alloca %tone x !llvm.array<" << n
        << " x !llvm.ptr> : (i64) -> !llvm.ptr\n";
      t << "  %fns = llvm.alloca %tone x !llvm.array<" << n
        << " x !llvm.ptr> : (i64) -> !llvm.ptr\n";
      for (int i = 0; i < n; ++i) {
        std::string idx = "%ti" + std::to_string(i);
        t << "  " << idx << " = llvm.mlir.constant(" << i << " : i64) : i64\n";
        t << "  %tn" << i << " = llvm.mlir.addressof @" << tests[i].nameGlobal
          << " : !llvm.ptr\n";
        t << "  %tnp" << i << " = llvm.getelementptr %names[" << idx
          << "] : (!llvm.ptr, i64) -> !llvm.ptr, i64\n";
        t << "  llvm.store %tn" << i << ", %tnp" << i
          << " : !llvm.ptr, !llvm.ptr\n";
        t << "  %tf" << i << " = llvm.mlir.addressof @" << tests[i].sym
          << " : !llvm.ptr\n";
        t << "  %tfp" << i << " = llvm.getelementptr %fns[" << idx
          << "] : (!llvm.ptr, i64) -> !llvm.ptr, i64\n";
        t << "  llvm.store %tf" << i << ", %tfp" << i
          << " : !llvm.ptr, !llvm.ptr\n";
      }
      t << "  %tn = llvm.mlir.constant(" << n << " : i64) : i64\n";
      t << "  %rc = llvm.call @a_test_main(%tn, %names, %fns)"
           " : (i64, !llvm.ptr, !llvm.ptr) -> i32\n";
      t << "  llvm.return %rc : i32\n}\n\n";
    }
  }

  // Declare every external symbol `entryBodyMlir` references. The tail is
  // parsed as a standalone module and llvm.call / llvm.mlir.addressof
  // verify that callees/globals exist in scope at parse time; symbols
  // already declared above (runtime sigs, __a_m<id>_init, __a_g_* roots,
  // __a_tcstr* test-name globals) are skipped. Everything else gets a
  // declaration that dedupLlvmFuncOps/dedupLlvmGlobalOps replaces with the
  // real definition after the splice — the same pattern the __a_m*_init
  // and __a_g_* declarations already rely on. Func decls use the uniform
  // A-function signature (!llvm.ptr) -> i64 (every user fn/method/ctor/
  // lambda/parfor helper has it); literal/global symbols are recognised by
  // their emitter prefixes. Emitted last inside `module { }` — op order
  // is irrelevant to symbol resolution.
  {
    std::set<std::string> declared;
    for (const auto& kv : runtime_sigs::kRuntimeSigs) declared.insert(kv.first);
    for (int id : initOrder)
      declared.insert("__a_m" + std::to_string(id) + "_init");
    for (const std::string& sym : rootSyms) declared.insert(sym);
    declared.insert("__a_main_body");
    declared.insert("__a_gc_root_mark");
    declared.insert("main");
    // Test-name globals (__a_tcstr*) and test fn decls are defined in
    // the tail itself; mark them declared so a same-named ref can't
    // spawn a duplicate decl.
    if (testNameSyms) {
      for (const std::string& sym : *testNameSyms) declared.insert(sym);
    }
    if (testFnSyms) {
      for (const std::string& sym : *testFnSyms) declared.insert(sym);
    }
    static const std::regex refRe("@([A-Za-z_][A-Za-z0-9_]*)");
    std::set<std::string> refs;
    for (std::sregex_iterator it(entryBodyMlir.begin(), entryBodyMlir.end(),
                                 refRe),
         end;
         it != end; ++it) {
      refs.insert((*it)[1].str());
    }
    for (const std::string& sym : refs) {
      if (declared.count(sym)) continue;
      if (sym.rfind("__a_g_", 0) == 0 || sym.rfind("__a_str", 0) == 0 ||
          sym.rfind("__a_chr", 0) == 0 || sym.rfind("__a_cstr", 0) == 0 ||
          sym.rfind("__a_estr", 0) == 0 || sym.rfind("__a_echr", 0) == 0 ||
          sym.rfind("__a_ecstr", 0) == 0) {
        t << "llvm.mlir.global external @" << sym
          << "(0 : i64) {addr_space = 0 : i32} : i64\n";
      } else {
        t << "llvm.func @" << sym << "(%argv: !llvm.ptr) -> i64\n";
      }
    }
  }

  t << "}\n";

  return t.str();
}

} // namespace

int ambcModulesLoad(const std::string& pkgDir,
                    const std::string& entryBodyMlir, bool testMode,
                    AmbcModules& pkg, std::ostream& err,
                    const std::string& testFilter) {
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

  dedupLlvmFuncOps(anchor);

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
  // its ops into the merged module. testNameSyms collects the __a_tcstr*
  // globals the test tail defines so the tail's own decl pass can mark
  // them (they are not yet in `anchor` when synthesizeTailText runs).
  std::vector<std::string> testNameSyms;
  std::vector<std::string> testFnSyms;
  std::string tailText = synthesizeTailText(meta, initOrder, entryBodyMlir,
                                          anchor, testMode, testFilter,
                                          &testNameSyms, &testFnSyms);
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
  dedupLlvmFuncOps(anchor);
  dedupLlvmGlobalOps(anchor);

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
                               bool testMode, std::ostream& err,
                               const std::string& testFilter) {
  AmbcModules pkg;
  if (ambcModulesLoad(pkgDir, entryBodyMlir, testMode, pkg, err,
                      testFilter) != 0) {
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
