# Astra toolchain support targets: the C++ frontend support library and
# the in-process runtime archive. The staged libaruntime.a that `a run`/
# `a build` link is produced by the top-level cmake/aruntime.cmake.

# C++ runtime support library (Value, utf8/format, file io) used by the
# frontend; lives next to the C runtime modules in runtime/.
add_subdirectory(runtime)

# C++ frontend (lex/parse/sema/IR/pipeline), formerly compiler/stages/.
add_subdirectory(frontend)

# In-process MLIR infrastructure (a dialect, passes, bytecode, native
# LLVM backend). Optional: builds a stub when MLIR dev files are absent.
add_subdirectory(mlir)
