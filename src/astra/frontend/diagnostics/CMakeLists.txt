# SourceFile lives with diagnostics because DiagnosticSink stores source
# files to render context. Keeping it here avoids a dependency cycle
# between a_diagnostics and a_lexer (the lexer consumes diagnostics).
add_library(a_diagnostics STATIC
  diagnostics.cpp
  ../lexer/source.cpp
)
target_link_libraries(a_diagnostics PUBLIC a_project_options)
add_library(a::diagnostics ALIAS a_diagnostics)
