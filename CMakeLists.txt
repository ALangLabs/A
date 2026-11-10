cmake_minimum_required(VERSION 3.20)
project(Alang VERSION 0.0.1 LANGUAGES CXX C)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/AOptions.cmake)
enable_testing()
# Keep `a` / `a-lsp` at the build root so tooling/build/build.sh, ./ac and
# `tests/integration/run_tests.sh build/a` work unchanged.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
# Modules in dependency order (each owns its CMakeLists.txt)
add_subdirectory(compiler/astra)
add_subdirectory(tools/lsp)
# In-process JIT backend (Goose-style): the Astra (compiler/astra/)
# compiles the C
# emitter's output into memory for `a run`.
include(cmake/libastra.cmake)
# Packaging
include(GNUInstallDirs)
install(TARGETS a RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS a-lsp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY compiler/astra/frontend/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/a/compiler
  FILES_MATCHING PATTERN "*.hpp")
install(DIRECTORY compiler/astra/runtime/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/a/runtime
  FILES_MATCHING PATTERN "*.hpp")

# Backend + runtime payload. An installed `a` locates both relative to its own
# executable (findAstra/findRuntime in pipeline.cpp), so the staged drivers go
# under <bindir>/astra/ and the staged runtime dirs (headers, runtime .c files,
# libastra1.a, libaruntime.a) under <bindir>/astralib*/. Without these the
# installed compiler cannot build or run a single program.
install(PROGRAMS "${ASTRA_BUILD}/astra${CMAKE_EXECUTABLE_SUFFIX}"
        DESTINATION ${CMAKE_INSTALL_BINDIR}/astra)
foreach(arch IN LISTS ASTRA_ALL_ARCHES)
  if(EXISTS "${ASTRA_BUILD}/astra-${arch}${CMAKE_EXECUTABLE_SUFFIX}")
    install(PROGRAMS "${ASTRA_BUILD}/astra-${arch}${CMAKE_EXECUTABLE_SUFFIX}"
            DESTINATION ${CMAKE_INSTALL_BINDIR}/astra)
  endif()
endforeach()
install(DIRECTORY "${ASTRA_STAGE}/" DESTINATION ${CMAKE_INSTALL_BINDIR}/astralib)
foreach(arch IN LISTS ASTRA_ALL_ARCHES)
  if(EXISTS "${CMAKE_BINARY_DIR}/astralib-${arch}")
    install(DIRECTORY "${CMAKE_BINARY_DIR}/astralib-${arch}/"
            DESTINATION ${CMAKE_INSTALL_BINDIR}/astralib-${arch})
  endif()
endforeach()
# The stdlib is resolved from <bindir>/../share/a/stdlib (defaultStdlibRoot).
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/stdlib/"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/a/stdlib
        PATTERN "_acache_" EXCLUDE)

add_subdirectory(tests)
