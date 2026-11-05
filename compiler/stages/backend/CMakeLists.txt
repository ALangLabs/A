add_library(a_backend STATIC
  interpreter.cpp
  stdlib/builtins.cpp
  native/native_codegen.cpp
  llvm/llvm_backend.cpp
)
target_link_libraries(a_backend PUBLIC a_type_checker a_runtime)
# TLS for net.tls_connect / https is optional: enable it when OpenSSL is present.
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
  target_compile_definitions(a_backend PRIVATE A_HAVE_OPENSSL=1)
  target_link_libraries(a_backend PUBLIC OpenSSL::SSL OpenSSL::Crypto)
else()
  message(STATUS "OpenSSL not found: net TLS support will report as unavailable")
endif()
add_library(a::backend ALIAS a_backend)
