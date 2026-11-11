add_library(a_driver STATIC driver.cpp)
target_link_libraries(a_driver PUBLIC a_pipeline a_tooling a_diagnostics a_vm)
add_library(a::driver ALIAS a_driver)
