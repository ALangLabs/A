# C code generation (frontend -> C for Astra). Links the type checker for
# SemaResult / inferred types.
add_library(a_codegen STATIC codegen.cpp)
target_link_libraries(a_codegen PUBLIC a_type_checker a_project_options)
add_library(a::codegen ALIAS a_codegen)
