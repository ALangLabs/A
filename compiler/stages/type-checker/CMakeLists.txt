add_library(a_type_checker STATIC sema.cpp)
target_link_libraries(a_type_checker PUBLIC a_name_resolution)
add_library(a::type_checker ALIAS a_type_checker)
