add_library(a_runtime STATIC
  strings/float_text.cpp
  strings/utf8.cpp
)
target_link_libraries(a_runtime PUBLIC a_project_options)
add_library(a::runtime ALIAS a_runtime)
