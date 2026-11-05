add_executable(a-lsp
  server.cpp
  analysis.cpp
  json.cpp
)
target_link_libraries(a-lsp PRIVATE a_type_checker a_tooling)
