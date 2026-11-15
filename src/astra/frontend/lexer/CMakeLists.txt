add_library(a_lexer STATIC lexer.cpp lexer_scan.cpp)
target_link_libraries(a_lexer PUBLIC a_diagnostics)
add_library(a::lexer ALIAS a_lexer)
