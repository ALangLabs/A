add_library(a_tooling STATIC
  source_text.cpp
  ast_text.cpp
  formatter.cpp
  docgen.cpp
)
target_link_libraries(a_tooling PUBLIC a_parser)
add_library(a::tooling ALIAS a_tooling)
