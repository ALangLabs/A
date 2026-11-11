add_library(a_runtime STATIC
  strings/float_text.cpp
  strings/utf8.cpp
)
target_link_libraries(a_runtime PUBLIC a_project_options)
add_library(a::runtime ALIAS a_runtime)

# In-process VM backend support: compile the A runtime C modules (the same
# sources the astra driver stages/compiles into standalone programs) into a
# static library the C++ `a` compiler binary can link directly. Top-level
# runtime/*.c only: i8086_runtime.s is assembly and strings//collections/
# subdirs are C++ frontend helpers built elsewhere. backend/python/python.c is
# included because runtime modules reference its python-interop symbols; it
# dlopens libpython at run time so no Python headers/libs are needed.
# i8086_runtime.s and the subdirectories are deliberately excluded.
file(GLOB A_VM_RUNTIME_C_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
     "${CMAKE_CURRENT_SOURCE_DIR}/../backend/python/python.c")
add_library(a_vm_runtime STATIC ${A_VM_RUNTIME_C_SOURCES})
set_target_properties(a_vm_runtime PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_include_directories(a_vm_runtime PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../backend/python")
# The runtime is compiled elsewhere with warnings suppressed (-w / /W0) and
# without -Werror; keep it that way here so -DA_WARNINGS_AS_ERRORS=ON does not
# break the build on legacy C code.
target_compile_options(a_vm_runtime PRIVATE
  $<$<NOT:$<C_COMPILER_ID:MSVC>>:-w>
  $<$<C_COMPILER_ID:MSVC>:/W0>)
if(NOT WIN32)
  target_link_libraries(a_vm_runtime PUBLIC m dl pthread)
endif()
add_library(a::vm_runtime ALIAS a_vm_runtime)
