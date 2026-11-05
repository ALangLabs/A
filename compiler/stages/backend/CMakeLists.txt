# A KGEN-only backend (Mojo ObjectCompiler/KGEN->LLVM architecture, adapted for A).
add_subdirectory(kgen/mlir)
add_library(a_backend STATIC
  kgen/kgen_backend.cpp
  kgen/execution_engine.cpp
  kgen/execution_engine_impl.cpp
  kgen/object_compiler.cpp
  kgen/codegen/llvm_emitter.cpp
  kgen/runtime/builtins.cpp
)
target_link_libraries(a_backend PUBLIC a_type_checker a_runtime)
target_link_libraries(a_backend PRIVATE a_kgen_mlir)
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
  target_compile_definitions(a_backend PRIVATE A_HAVE_OPENSSL=1)
  target_link_libraries(a_backend PUBLIC OpenSSL::SSL OpenSSL::Crypto)
else()
  message(STATUS "OpenSSL not found: net TLS support will report as unavailable")
endif()
add_library(a::backend ALIAS a_backend)
