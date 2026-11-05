add_library(a_driver STATIC driver.cpp)
target_link_libraries(a_driver PUBLIC a_backend a_ownership a_ir a_optimization a_tooling)
add_library(a::driver ALIAS a_driver)
