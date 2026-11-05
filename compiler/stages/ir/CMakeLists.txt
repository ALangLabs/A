add_library(a_ir STATIC ir.cpp)
target_link_libraries(a_ir PUBLIC a_type_checker a_runtime)
add_library(a::ir ALIAS a_ir)
