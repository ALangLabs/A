# Direct MLIR backend (frontend -> .mlir llvm dialect -> mlir-translate -> .ll).
# mlir_emit_ad.cpp is the `a`-dialect emitter (A_MLIR_TYPED / a.mlir-opt
# round-trip path); it shares the same Sema/module-graph inputs.
# ../llvm/llvm_emit.cpp is the restored hand-written LLVM IR text emitter
# (pre-MLIR backend, kept as the A_BACKEND=llvm-legacy escape hatch).
add_library(a_mlir STATIC mlir_emit.cpp mlir_emit_ad.cpp ../llvm/llvm_emit.cpp)
target_link_libraries(a_mlir PUBLIC a_type_checker a_runtime a_project_options)
add_library(a::mlir ALIAS a_mlir)
