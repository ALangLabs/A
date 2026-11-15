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
# Stage the A runtime (compiler/astra/runtime/*.c) into build/astralib as
# libaruntime.a; `a run`/`a build` link it and the run-cache fingerprints it —
# keep the staged runtime fresh whenever `a` is rebuilt so `--target a` builds
# cannot serve binaries linked against a stale runtime.
include(cmake/aruntime.cmake)
add_dependencies(a aruntime)
# Packaging
include(GNUInstallDirs)
install(TARGETS a RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS a-lsp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY compiler/astra/frontend/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/a/compiler
  FILES_MATCHING PATTERN "*.hpp")
install(DIRECTORY compiler/astra/runtime/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/a/runtime
  FILES_MATCHING PATTERN "*.hpp")
add_subdirectory(tests)
