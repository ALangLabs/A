# LLVM IR backend (frontend -> .ll -> clang/opt/llc -> machine code).
add_library(a_llvm STATIC llvm_emit.cpp)
target_link_libraries(a_llvm PUBLIC a_type_checker a_runtime a_project_options)
add_library(a::llvm ALIAS a_llvm)
