# Astra toolchain support targets. The TinyCC fork itself is built by
# cmake/libtcc.cmake (libtcc + the `astra` driver + staged tcclib/); this file
# covers the C++ pieces that live alongside it.

# C++ frontend (lex/parse/sema/IR/pipeline), formerly compiler/stages/.
add_subdirectory(frontend)

# Python interop, relocated from the retired KGEN backend. It dlopens
# libpython at runtime (uv/venv/system discovery), so we only need headers at
# build time — never link libpython itself. Prefer the python on PATH
# (usually the uv/venv one) so header and runtime versions line up;
# -DPython3_EXECUTABLE still overrides.
add_library(a_python_interop STATIC
  python/python_interop.cpp
  python/python_loader.cpp
)
target_link_libraries(a_python_interop PUBLIC a_project_options a_runtime)
target_include_directories(a_python_interop PRIVATE ${CMAKE_SOURCE_DIR}/compiler ${CMAKE_SOURCE_DIR}/compiler/astra/frontend)

if(NOT DEFINED Python3_EXECUTABLE)
  find_program(A_PATH_PYTHON NAMES python3 python)
  if(A_PATH_PYTHON)
    set(Python3_EXECUTABLE "${A_PATH_PYTHON}")
  endif()
endif()
find_package(Python3 COMPONENTS Development QUIET)
if(Python3_Development_FOUND)
  target_compile_definitions(a_python_interop PRIVATE A_HAVE_PYTHON=1)
  # FindPython3 may resolve headers against a different Python than the
  # interpreter it picked (e.g. venv python + system headers). Ask the chosen
  # interpreter for its own include dir so headers match the likely runtime.
  execute_process(
    COMMAND "${Python3_EXECUTABLE}" -c "import sysconfig;print(sysconfig.get_paths()['include'])"
    OUTPUT_VARIABLE _A_PY_INCLUDE OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _A_PY_INCLUDE_RC)
  if(_A_PY_INCLUDE_RC EQUAL 0 AND IS_DIRECTORY "${_A_PY_INCLUDE}")
    target_include_directories(a_python_interop PRIVATE "${_A_PY_INCLUDE}")
  else()
    target_include_directories(a_python_interop PRIVATE ${Python3_INCLUDE_DIRS})
  endif()
  target_link_libraries(a_python_interop PRIVATE ${CMAKE_DL_LIBS})
  message(STATUS "Python interop enabled (runtime libpython discovery): ${Python3_VERSION}")
else()
  message(STATUS "Python3 development files not found: python:<pkg> imports will report unavailable")
endif()

add_library(a::python_interop ALIAS a_python_interop)
