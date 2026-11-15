add_executable(a main.cpp)
target_link_libraries(a PRIVATE a_driver a_vm_runtime)
