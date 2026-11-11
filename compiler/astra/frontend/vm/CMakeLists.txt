# Tree-walking interpreter backend ("--vm"): executes A programs in-process
# on the C runtime's aval value model via the a_vm_runtime static lib.
add_library(a_vm STATIC vm.cpp)
target_link_libraries(a_vm PUBLIC a_vm_runtime a_type_checker a_name_resolution a_ast a_project_options)
add_library(a::vm ALIAS a_vm)
