add_library(a_optimization STATIC optimizer.cpp)
target_link_libraries(a_optimization PUBLIC a_ir)
add_library(a::optimization ALIAS a_optimization)
