add_library(a_runtime STATIC
  strings/utf8.cpp
  strings/format.cpp
  io/file_io.cpp
)
target_link_libraries(a_runtime PUBLIC a_project_options)
add_library(a::runtime ALIAS a_runtime)
