add_library(a_pipeline STATIC pipeline.cpp)
target_link_libraries(a_pipeline PUBLIC a_codegen a_ownership a_ir a_optimization a_tooling a_type_checker a_name_resolution a_parser a_lexer a_diagnostics)
add_library(a::pipeline ALIAS a_pipeline)
