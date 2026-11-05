# Header-only AST module: no sources, but a first-class CMake target so
# every `compiler/stages/*/` directory is an independently addressable
# module (a::ast). Links a_diagnostics for Span it builds on conceptually;
# keep it INTERFACE-only so it never becomes a hard dependency cycle.
add_library(a_ast INTERFACE)
target_link_libraries(a_ast INTERFACE a_diagnostics a_project_options)
add_library(a::ast ALIAS a_ast)
