//===- ambc.hpp - .ambc bytecode package -> object -------------------*- C++ -*-===//
//
// Part of the A project. See docs/mlir-architecture.md.
//
// Entry point used by the frontend pipeline (pipeline.cpp) for the
// A_AMBC=1 fast path: compile a precompiled `.ambc` package directly to a
// machine object, skipping parse/comptime/sema/emit entirely.
//
//===----------------------------------------------------------------------===//

#ifndef A_MLIR_AMBC_HPP
#define A_MLIR_AMBC_HPP

#include <iosfwd>
#include <string>

namespace a {

// Compile the `.ambc` package at `ambcPath` to a machine object at
// `objPath`.
//   arch     - target arch/triple; "" / "host" selects the host default
//              (cross compile unsupported, same as the rest of the backend).
//   optLevel - 0..3, mirroring `opt -O<n>`.
// Returns 0 on success, 1 on failure (diagnostic on `err`).
int ambcCompileToObject(const std::string& ambcPath, const std::string& arch,
                        int optLevel, const std::string& objPath,
                        std::ostream& err);

} // namespace a

#endif // A_MLIR_AMBC_HPP
