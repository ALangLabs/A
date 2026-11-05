add_library(a_object STATIC object_compiler.cpp)
target_link_libraries(a_object PUBLIC a_backend a_ir a_optimization)
add_library(a::object ALIAS a_object)
