add_library(a_driver STATIC driver.cpp)
target_link_libraries(a_driver PUBLIC a_pipeline a_object a_tooling a_diagnostics)
add_library(a::driver ALIAS a_driver)
