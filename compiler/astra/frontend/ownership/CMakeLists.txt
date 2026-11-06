add_library(a_ownership STATIC ownership.cpp)
target_link_libraries(a_ownership PUBLIC a_type_checker)
add_library(a::ownership ALIAS a_ownership)
