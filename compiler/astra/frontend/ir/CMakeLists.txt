add_library(a_ir STATIC
  ir.cpp
  ir_analysis.cpp
  ir_builder_core.cpp
  ir_builder_flow.cpp
  ir_builder_expr.cpp
  ir_builder_stmt.cpp
  ir_print.cpp
)
target_link_libraries(a_ir PUBLIC a_type_checker a_runtime)
add_library(a::ir ALIAS a_ir)
