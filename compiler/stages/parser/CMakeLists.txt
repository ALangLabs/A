add_library(a_parser STATIC parser.cpp)
target_link_libraries(a_parser PUBLIC a_lexer)
add_library(a::parser ALIAS a_parser)
