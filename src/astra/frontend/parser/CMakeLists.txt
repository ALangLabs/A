add_library(a_parser STATIC
    parser_core.cpp
    parser_decl.cpp
    parser_stmt.cpp
    parser_type.cpp
    parser_expr.cpp
    parser_expr_primary.cpp
    parser_pattern.cpp
    parser_string.cpp
)
target_link_libraries(a_parser PUBLIC a_lexer a_ast)
add_library(a::parser ALIAS a_parser)
