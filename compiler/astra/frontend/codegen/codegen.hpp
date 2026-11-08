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
// unchanged when there is no re-export. Shared by emitC and emitAsm.
std::pair<int, std::string> resolveModuleMember(const ModuleGraph& graph,
                                              int moduleId,
                                              const std::string& name);

// Emits x86-64 assembly (AT&T syntax) for astra to assemble/compile and link
// into machine code — the host fast path (A -> asm -> machine code).
// In test mode, `main` discovers and runs top-level `fn test_*()` functions
// through the runtime a_test_main harness. Returns false on constructs the
// asm backend does not cover.
bool emitAsm(const ModuleGraph& graph, const SemaResult& sema, std::string& out,
             std::string& error, bool testMode = false);

// Emits portable C (calling the public runtime API in a_runtime.h) for astra
// to compile for ANY target it supports (i386, x86-64, ARM, AArch64,
// RISC-V64, PE, Mach-O). This is the cross-compilation / non-x86-64 path:
// one emitter covers every backend target. In test mode, `main` discovers
// and runs top-level `fn test_*()` functions. Returns false and fills
// `error` on unsupported constructs.
bool emitC(const ModuleGraph& graph, const SemaResult& sema, std::string& out,
           std::string& error, bool testMode = false);

// Emits target-native assembly (A -> .s -> astra -> machine code) for the
// given arch ("arm64"/"aarch64", "riscv64", "i386", "arm", "i8086" today;
// more targets plug into the same NatTarget layer). No C intermediate:
// values are aval words in frame slots, all semantics via a_*_slow runtime
// calls. Returns false and fills `error` for unimplemented arches or
// uncovered constructs so the pipeline can fall back to emitC.
//
// Known gap: there is no x86-64 Windows (MS-ABI/PE) target. The astra
// backend itself supports it (ASTRA_TARGET_X86_64 + ASTRA_TARGET_PE,
// astrape.c/astracoff.c, win32/lib import libs), but emitNative has no
// Win64 NatTarget and emitAsm is SysV-only, so Windows x86-64 hosts always
// take the emitC path. A Windows native backend needs a Win64 calling
// convention (RCX/RDX/R8/R9 args, 32-byte shadow space, chkstk probing),
// SEH/.pdata-.xdata unwinding instead of SysV CFI, COFF/PE relocations,
// and the crt1.c/_runmain entry — all of which the astra side already
// provides for the C emitter.
bool emitNative(const ModuleGraph& graph, const SemaResult& sema,
                const std::string& arch, std::string& out,
                std::string& error, bool testMode = false);
}  // namespace a
