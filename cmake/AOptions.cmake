include_guard(GLOBAL)
option(A_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
# LTO roughly doubles link time on the `a`/`a-lsp` binaries, so it is off by
# default for fast dev iteration. Enable it for release/perf builds:
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DA_ENABLE_LTO=ON
option(A_ENABLE_LTO "Enable link-time optimization in Release builds" OFF)
if(A_ENABLE_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
  include(CheckIPOSupported)
  check_ipo_supported(RESULT a_ipo_supported OUTPUT a_ipo_error)
  if(a_ipo_supported)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
  else()
    message(STATUS "LTO unavailable: ${a_ipo_error}")
  endif()
endif()
add_library(a_project_options INTERFACE)
target_include_directories(a_project_options INTERFACE
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/astra/frontend>
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/astra/runtime>
  $<INSTALL_INTERFACE:include/a/compiler>
  $<INSTALL_INTERFACE:include/a/runtime>)
if(MSVC)
  target_compile_options(a_project_options INTERFACE /W4)
  # This codebase deliberately uses the classic, portable CRT surface
  # (getenv, strncpy, fopen, ...). MSVC's C4996 "unsafe function" notes point
  # at its _s replacements, not at bugs here, so silence them project-wide
  # instead of rewriting each call site.
  target_compile_definitions(a_project_options INTERFACE
    _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
  if(A_WARNINGS_AS_ERRORS)
    target_compile_options(a_project_options INTERFACE /WX)
  endif()
else()
  target_compile_options(a_project_options INTERFACE -Wall -Wextra -Wpedantic)
  # fastpath_mlir.hpp embeds a ~94 KB generated MLIR literal; AppleClang
  # rejects literals over 65536 bytes under -Werror,-Woverlength-strings.
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(a_project_options INTERFACE -Wno-overlength-strings)
  endif()
  if(A_WARNINGS_AS_ERRORS)
    target_compile_options(a_project_options INTERFACE -Werror)
  endif()
endif()
add_library(a::project_options ALIAS a_project_options)
