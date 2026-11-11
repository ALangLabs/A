#pragma once

#include <cstdint>
#include <string>

extern "C" {
#include "a_runtime.h"
}

namespace a {
struct ModuleGraph;
struct SemaResult;
}

namespace a::vm {

// Tree-walking interpreter backend: executes a fully-loaded, sema-checked
// program in-process on top of the C runtime's `aval` value model
// (compiler/astra/runtime/a_runtime.h). Returns the process exit code
// (0 on success, 1 on VM-unsupported constructs / test failures).
//
// `testMode` mirrors `a test`: top-level `fn test_*()` declarations in the
// entry module are run instead of the `entry` function; `testFilter` is the
// `--filter` substring (empty = all tests).
int runProgram(const ModuleGraph& graph, const SemaResult& sema,
               bool testMode, const std::string& testFilter);

// Returns true when the program uses constructs the VM cannot execute
// (struct/enum/trait/impl/type-alias declarations). Callers should route
// such programs to the native backend so `--vm` has parity with `a run`.
bool programNeedsNative(const ModuleGraph& graph);

// Sentinel returned by runProgram when the program hit a construct the VM
// cannot execute mid-run; callers should re-run through the native backend.
inline constexpr int kVmFallbackToNative = -100;

// Exceptions used for non-local control flow inside the interpreters.
// VmReturn/VmPropagate/VmBreak/VmContinue/VmUnsupported are shared between the
// tree-walker (vm.cpp) and the bytecode VM (bc_run.cpp).
struct VmReturn {
  aval v;
};
struct VmPropagate {
  aval v;  // `expr?` early-returns this value from the enclosing function
};
struct VmBreak {};
struct VmContinue {};
struct VmUnsupported {
  std::string msg;
};

// Non-local control flow for statements (tree-walker only).
enum class Flow : uint8_t { Normal, Return, Break, Continue };
struct FlowRes {
  Flow f = Flow::Normal;
  aval v = A_NONE;
};

}  // namespace a::vm
