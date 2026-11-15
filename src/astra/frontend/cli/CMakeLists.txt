add_executable(a main.cpp)
# a::mlir_core provides `a precompile`/`a mlir-opt` and the optional
# in-process MLIR backend used by the pipeline (A_MLIR_NATIVE=1). Its public
# include dir exposes src/astra/mlir/ so "mlir/driver.hpp" resolves; without
# MLIR dev files it still provides the stub CLI entry points.
target_include_directories(a PRIVATE ${CMAKE_SOURCE_DIR}/src/astra/mlir)
# a::mlir_core is whole-archived: pipeline.cpp references its entry
# points (nativeCompileMlirTextToObject, ambcCompileToObject, ...) via
# weak fallbacks, and static-lib link order would otherwise let the weak
# stubs win before the strong objects are pulled.
target_link_libraries(a PRIVATE a_driver a_vm_runtime
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,a::mlir_core>")
