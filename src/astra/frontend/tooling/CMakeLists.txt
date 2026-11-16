add_library(a_tooling STATIC
  source_text.cpp
  ast_text.cpp
  formatter.cpp
  formatter_comments.cpp
  formatter_expr.cpp
  formatter_output.cpp
  formatter_stmt.cpp
  docgen_extract.cpp
  docgen_render.cpp
)
target_link_libraries(a_tooling PUBLIC a_parser)
add_library(a::tooling ALIAS a_tooling)
