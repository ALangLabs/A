add_subdirectory(kgen/mlir)
add_library(a_backend STATIC
  kgen/kgen_backend.cpp
  kgen/execution_engine.cpp
  kgen/execution_engine_impl.cpp
  kgen/object_compiler.cpp
  kgen/codegen/llvm_emitter.cpp
  kgen/codegen/c_emitter.cpp
  kgen/jit.cpp
  kgen/runtime/builtins.cpp
  kgen/runtime/terminal.cpp
  kgen/python_interop.cpp
  kgen/python_loader.cpp
)
target_link_libraries(a_backend PUBLIC a_type_checker a_runtime)
target_link_libraries(a_backend PRIVATE a_kgen_mlir libtcc)
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
  target_compile_definitions(a_backend PRIVATE A_HAVE_OPENSSL=1)
  target_link_libraries(a_backend PUBLIC OpenSSL::SSL OpenSSL::Crypto)
else()
  message(STATUS "OpenSSL not found: net TLS support will report as unavailable")
endif()
add_library(a::backend ALIAS a_backend)

# Python interop dlopens libpython at runtime (uv/venv/system discovery), so
# we only need headers at build time — never link libpython itself. Prefer the
# python on PATH (usually the uv/venv one) so header and runtime versions
# line up; -DPython3_EXECUTABLE still overrides.
if(NOT DEFINED Python3_EXECUTABLE)
  find_program(A_PATH_PYTHON NAMES python3 python)
  if(A_PATH_PYTHON)
    set(Python3_EXECUTABLE "${A_PATH_PYTHON}")
  endif()
endif()
find_package(Python3 COMPONENTS Development QUIET)
if(Python3_Development_FOUND)
  target_compile_definitions(a_backend PRIVATE A_HAVE_PYTHON=1)
  # FindPython3 may resolve headers against a different Python than the
  # interpreter it picked (e.g. venv python + system headers). Ask the chosen
  # interpreter for its own include dir so headers match the likely runtime.
  execute_process(
    COMMAND "${Python3_EXECUTABLE}" -c "import sysconfig;print(sysconfig.get_paths()['include'])"
    OUTPUT_VARIABLE _A_PY_INCLUDE OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _A_PY_INCLUDE_RC)
  if(_A_PY_INCLUDE_RC EQUAL 0 AND IS_DIRECTORY "${_A_PY_INCLUDE}")
    target_include_directories(a_backend PRIVATE "${_A_PY_INCLUDE}")
  else()
    target_include_directories(a_backend PRIVATE ${Python3_INCLUDE_DIRS})
  endif()
  target_link_libraries(a_backend PRIVATE ${CMAKE_DL_LIBS})
  message(STATUS "Python interop enabled (runtime libpython discovery): ${Python3_VERSION}")
else()
  message(STATUS "Python3 development files not found: python:<pkg> imports will report unavailable")
endif()
