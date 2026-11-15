# Direct MLIR backend (frontend -> .mlir llvm dialect -> mlir-translate -> .ll).
# mlir_emit_ad.cpp is the `a`-dialect emitter (A_MLIR_TYPED / a.mlir-opt
# round-trip path); it shares the same Sema/module-graph inputs.
add_library(a_mlir STATIC mlir_emit.cpp mlir_emit_ad.cpp)
target_link_libraries(a_mlir PUBLIC a_type_checker a_runtime a_project_options)
add_library(a::mlir ALIAS a_mlir)
