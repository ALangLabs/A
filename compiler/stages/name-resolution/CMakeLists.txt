add_library(a_name_resolution STATIC module_loader.cpp)
target_link_libraries(a_name_resolution PUBLIC a_parser)
add_library(a::name_resolution ALIAS a_name_resolution)
