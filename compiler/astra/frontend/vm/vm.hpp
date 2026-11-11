#pragma once

#include <string>

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

}  // namespace a::vm
