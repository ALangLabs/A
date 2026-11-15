# Direct MLIR backend (frontend -> .mlir llvm dialect -> mlir-translate -> .ll).
add_library(a_mlir STATIC mlir_emit.cpp)
target_link_libraries(a_mlir PUBLIC a_type_checker a_runtime a_project_options)
add_library(a::mlir ALIAS a_mlir)
