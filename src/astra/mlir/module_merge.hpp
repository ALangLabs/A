//===- module_merge.hpp - ModuleOp merge/dedup helpers --------------------===//
//
// Part of the A toolchain (src/astra/mlir). Internal header shared by the
// .ambc producer (package.cpp) and the .ambc consumer (ambc_modules.cpp).
//
// Both sides merge several per-module `llvm`-dialect ModuleOps into one
// (or splice a support/tail document into one) and then deduplicate the
// re-declared `llvm.func` / `llvm.mlir.global` symbol ops the merge
// produces. These helpers implement that exact contract; keep them in
// sync with the comments at each call site.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/IR/BuiltinOps.h"

namespace a {

// Move every op out of `src`'s body into `dst`'s body (append order).
// `src`'s body is left empty; ownership of the moved ops transfers to
// `dst`'s region. Callers keep `src` alive until after the move.
inline void moveModuleOps(mlir::ModuleOp dst, mlir::ModuleOp src) {
  mlir::Block* dstBody = &dst.getBodyRegion().front();
  mlir::Block* srcBody = &src.getBodyRegion().front();
  while (!srcBody->empty()) {
    mlir::Operation* op = &srcBody->front();
    op->moveBefore(dstBody, dstBody->end());
  }
}

// Deduplicate `llvm.func` ops by symbol name in `module`. Every merged
// unit re-declares the `a_*` runtime functions (and may re-declare foreign
// `__a_f_m*`/`__a_m*` symbols) as body-less declarations while the module
// that owns a symbol also carries its definition. Keep a definition when
// present, else the first declaration; erase the rest.
inline void dedupLlvmFuncOps(mlir::ModuleOp module) {
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
    const std::string name = fn.getSymName().str();
    const bool hasBody = !fn.getBody().empty();
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

// Deduplicate `llvm.mlir.global` ops by symbol name in `module`, keeping
// the first occurrence. Merged units may each carry identical external
// declarations of `__a_g_*` globals; globals with initializers (the
// private `__a_str`/`__a_chr`/`__a_cstr` constants) have unique names and
// are unaffected.
inline void dedupLlvmGlobalOps(mlir::ModuleOp module) {
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

} // namespace a
