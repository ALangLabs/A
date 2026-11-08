# x86-64 assembly backend (frontend -> .s -> Astra). The AsmEmitter class is
# declared in codegen_internal.hpp and implemented across domain TUs.
add_library(a_codegen STATIC
  codegen.cpp
  codegen_analysis.cpp
  codegen_calls.cpp
  codegen_decls.cpp
  codegen_emit.cpp
  codegen_expr.cpp
  codegen_native.cpp
  codegen_stmt.cpp
)
target_link_libraries(a_codegen PUBLIC a_type_checker a_runtime a_project_options)
add_library(a::codegen ALIAS a_codegen)
