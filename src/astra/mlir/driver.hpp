//===- driver.hpp - `a mlir-opt` pass-debugging driver -------------*- C++ -*-===//
//
// Part of the A toolchain (src/astra/mlir). See docs/mlir-architecture.md.
//
// `runMlirOptCli` is the backend for the hidden `a mlir-opt` subcommand --
// a kgen-opt/mlir-opt style driver for inspecting and debugging the MLIR
// pass pipeline on `.mlir` text or `.ambc` bytecode inputs.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <iosfwd>

namespace a {

/// Entry point for the hidden `a mlir-opt` subcommand. `argv`/`argc` are the
/// arguments AFTER the subcommand name (i.e. `a mlir-opt <args...>` passes
/// `<args...>` here). `out` receives the resulting IR / LLVM IR / bytecode
/// diagnostics on stdout; `err` receives errors and usage text.
///
/// Supported surface:
///   a mlir-opt <file.mlir|file.ambc> [options] [passes...]
///     --mlir-print-ir-before-all | --mlir-print-ir-after-all
///     --mlir-print-ir-before=<pass> | --mlir-print-ir-after=<pass>
///     --verify-each
///     --emit-llvmir            print LLVM IR text (mlir -> llvm -> text)
///     --emit-bytecode          write MLIR bytecode to the output
///     --o <file> | -o <file>   output path (default: stdout)
///     --version                print the compiler version
///     --help                   print usage
///   Bare positional pass names (e.g. `cse canonicalize`, or a pipeline
///   string like `builtin.module(func.func(cse))`) are parsed with
///   `mlir::parsePassPipeline`; an empty list runs the default `a` pipeline
///   via `a::buildAOptPipeline` (see src/astra/mlir/pipeline.hpp).
///
/// Returns 0 on success, 1 on failure, 2 on usage error.
int runMlirOptCli(int argc, char **argv, std::ostream &out, std::ostream &err);

} // namespace a
