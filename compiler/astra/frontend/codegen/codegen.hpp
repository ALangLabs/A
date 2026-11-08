#pragma once
// x86-64 assembly code generation from the frontend AST. The C++ frontend
// (lex/parse/sema/ownership) lowers A to AT&T-syntax assembly; the runtime ABI
// lives in compiler/astra/runtime/a_runtime.h and generated code calls the
// a_* runtime helpers (compiled from compiler/astra/runtime/a_runtime.c by the
// Astra backend). The pipeline hands the staged .s and runtime source to the
// `astra` driver to produce a native executable.
#include <string>
#include <utility>

namespace a {
struct ModuleGraph;
struct SemaResult;

// Follows `from "x.a" import name` re-exports (package __init__ modules) to
// the module that actually declares `name`; returns {moduleId, name},
// unchanged when there is no re-export. Shared by the asm emitters.
std::pair<int, std::string> resolveModuleMember(const ModuleGraph& graph,
                                              int moduleId,
                                              const std::string& name);

// Emits x86-64 assembly (AT&T syntax) for astra to assemble/compile and link
// into machine code — the host fast path (A -> asm -> machine code).
// In test mode, `main` discovers and runs top-level `fn test_*()` functions
// through the runtime a_test_main harness. When `testFilter` is non-null and
// non-empty, only test functions whose name contains the filter substring are
// registered with the harness (unmatched tests are not called). Returns false
// on constructs the asm backend does not cover.
bool emitAsm(const ModuleGraph& graph, const SemaResult& sema, std::string& out,
             std::string& error, bool testMode = false,
             const std::string* testFilter = nullptr);

// Emits target-native assembly (A -> .s -> astra -> machine code) for the
// given arch ("arm64"/"aarch64", "riscv64", "i386", "arm", "i8086", and
// "x86_64"+os=="windows" today; more targets plug into the same NatTarget
// layer). No C intermediate: values are aval words in frame slots, all
// semantics via a_*_slow runtime calls. Returns false and fills `error`
// for unimplemented arches or uncovered constructs — the pipeline treats
// a decline as a hard error (there is no C fallback).
//
// x86_64+windows uses the Win64 MS ABI (rcx/rdx/r8/r9 args, 32-byte
// shadow space, __chkstk probing for >4KiB frames). Gap: no SEH
// .pdata/.xdata unwind info is emitted for asm functions.
bool emitNative(const ModuleGraph& graph, const SemaResult& sema,
                const std::string& arch, const std::string& os,
                std::string& out, std::string& error, bool testMode = false,
                const std::string* testFilter = nullptr);
}  // namespace a
