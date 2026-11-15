# CMake fragment for the A MLIR passes.
#
# Intended to be included from src/astra/mlir/CMakeLists.txt:
#
#   add_subdirectory(passes)
#
# Expects the parent scope to have found MLIR (find_package(MLIR REQUIRED
# CONFIG) or MLIR_DIR set) and to provide the `a_mlir_core` target
# (../CMakeLists.txt); falls back to `a_mlir` for standalone use.

# Pass sources. ALowerToLLVM.cpp intentionally matches `a` dialect ops by
# name so it compiles before the dialect headers exist; once they land the
# string dispatch should be replaced with typed accessors (see file header).
set(A_MLIR_PASS_SOURCES
  ALowerToLLVM.cpp
)

# The parent scope defines `a_mlir_core` (see ../CMakeLists.txt). Fall back
# to `a_mlir` for standalone/fragment use.
if(TARGET a_mlir_core)
  set(A_MLIR_TARGET a_mlir_core)
elseif(TARGET a_mlir)
  set(A_MLIR_TARGET a_mlir)
else()
  add_library(a_mlir STATIC ${A_MLIR_PASS_SOURCES})
  set(A_MLIR_TARGET a_mlir)
endif()

target_sources(${A_MLIR_TARGET} PRIVATE ${A_MLIR_PASS_SOURCES})

target_include_directories(${A_MLIR_TARGET}
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(TARGET a_mlir)
  # MLIR 18 dev package (also works with MLIRConfig.cmake from llvm-18).
  if(MLIR_FOUND)
    target_link_libraries(a_mlir PUBLIC MLIR)
  else()
    # Fallback: link the monolithic MLIR library when only the .a files exist.
    find_library(MLIR_LIB NAMES MLIR libMLIR.a
      PATHS /usr/lib/llvm-18/lib /usr/lib ${LLVM_LIBRARY_DIR}
      NO_DEFAULT_PATH)
    if(MLIR_LIB)
      target_link_libraries(a_mlir PUBLIC ${MLIR_LIB})
    else()
      message(WARNING "a_mlir: MLIR library not found; pass sources will not link")
    endif()
  endif()
endif()

# Also expose the passes to the driver/tooling targets in the parent scope.
set(A_MLIR_PASS_SOURCES ${A_MLIR_PASS_SOURCES} PARENT_SCOPE)
