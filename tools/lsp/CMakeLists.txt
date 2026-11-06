add_executable(a-lsp
  server.cpp
  transport.cpp
  documents.cpp
  compiler_worker.cpp
  diagnostics.cpp
  handlers_navigation.cpp
  handlers_features.cpp
  handlers_hierarchy.cpp
  text_utils.cpp
  docs.cpp
  main.cpp
  analysis.cpp
  json.cpp
)
target_link_libraries(a-lsp PRIVATE a_type_checker a_tooling)
