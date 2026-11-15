# Builds the staged A runtime that `a run`/`a build` link against.
#
# compiler/astra/runtime/*.c (plus the vendored mimalloc TU and the Python
# interop bridge, python.c, which now lives in runtime/) is compiled once by
# the system C compiler into libaruntime.a, and the runtime sources are
# staged into ${CMAKE_BINARY_DIR}/astralib next to it. The `a` driver locates
# both relative to its own executable (build/astralib for in-tree builds,
# <prefix>/astralib for installed releases), so the stage dir name is part of
# the runtime contract — do not rename it without updating findRuntime() in
# compiler/astra/frontend/pipeline/pipeline.cpp.

set(ASTRA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/compiler/astra")
set(ASTRA_RUNTIME_DIR "${ASTRA_ROOT}/runtime")
set(ASTRA_STAGE "${CMAKE_BINARY_DIR}/astralib")
set(ASTRA_OBJ_DIR "${CMAKE_BINARY_DIR}/aruntime-obj")

# Stage the runtime sources next to libaruntime.a. Copied by build-time
# custom commands (not file(COPY)) so runtime edits are picked up on
# rebuild. CONFIGURE_DEPENDS re-globs when modules are added/removed, and
# staged files whose source disappeared are removed at configure time.
file(GLOB ASTRA_RUNTIME_SOURCES CONFIGURE_DEPENDS
     "${ASTRA_RUNTIME_DIR}/*.c" "${ASTRA_RUNTIME_DIR}/*.h"
     "${ASTRA_RUNTIME_DIR}/*.s")
file(GLOB ASTRA_RUNTIME_STAGED "${ASTRA_STAGE}/*.c" "${ASTRA_STAGE}/*.h" "${ASTRA_STAGE}/*.s")
foreach(staged IN LISTS ASTRA_RUNTIME_STAGED)
    get_filename_component(staged_name "${staged}" NAME)
    if(NOT EXISTS "${ASTRA_RUNTIME_DIR}/${staged_name}")
        file(REMOVE "${staged}")
    endif()
endforeach()
set(ASTRA_RUNTIME_STAGED_OUTPUTS "")
set(ASTRA_RUNTIME_C_OUTPUTS "")
foreach(rt_file IN LISTS ASTRA_RUNTIME_SOURCES)
    get_filename_component(rt_name "${rt_file}" NAME)
    set(staged_file "${ASTRA_STAGE}/${rt_name}")
    add_custom_command(
        OUTPUT "${staged_file}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${rt_file}" "${staged_file}"
        DEPENDS "${rt_file}"
        COMMENT "stage ${rt_name} -> astralib"
        VERBATIM)
    list(APPEND ASTRA_RUNTIME_STAGED_OUTPUTS "${staged_file}")
    if(rt_file MATCHES "\\.c$")
        list(APPEND ASTRA_RUNTIME_C_OUTPUTS "${staged_file}")
    endif()
endforeach()

# Prebuild the A runtime modules once into libaruntime.a so `a run`/`a build`
# link an archive instead of recompiling every runtime .c per program. Built
# by the system C compiler on every host OS.
set(ASTRA_RT_OBJS "")
set(ASTRA_RT_CC_FLAGS -O3 -std=gnu11 -w -fno-semantic-interposition
    -I${ASTRA_STAGE})
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64|i386|i686")
    list(APPEND ASTRA_RT_CC_FLAGS -march=native -mtune=native)
endif()
file(MAKE_DIRECTORY "${ASTRA_OBJ_DIR}")
foreach(rt_c IN LISTS ASTRA_RUNTIME_C_OUTPUTS)
    get_filename_component(rt_c_name "${rt_c}" NAME)
    set(rt_obj "${ASTRA_OBJ_DIR}/cc_rt_${rt_c_name}.o")
    # math_ew.c ports A-side float formulas literally (mul-then-add, not
    # fma); disable FP contraction so -O3 cannot fuse them into fma and
    # change last-ulp results vs the A fallback paths.
    set(rt_extra_flags "")
    if(rt_c_name STREQUAL "math_ew.c" AND NOT MSVC)
        list(APPEND rt_extra_flags -ffp-contract=off)
    endif()
    add_custom_command(OUTPUT "${rt_obj}"
        COMMAND ${CMAKE_C_COMPILER} ${ASTRA_RT_CC_FLAGS} ${rt_extra_flags}
                -c "${rt_c}" -o "${rt_obj}"
        DEPENDS "${rt_c}"
        COMMENT "system runtime ${rt_c_name}"
        VERBATIM)
    list(APPEND ASTRA_RT_OBJS "${rt_obj}")
endforeach()
# Vendored mimalloc (compiler/astra/runtime/vendor/mimalloc): compiled as a
# single TU exposing mi_* symbols. Not yet wired into the GC allocator —
# kept as a ready fast allocator for runtime allocation paths.
set(ASTRA_MIMALLOC_SRC "${ASTRA_RUNTIME_DIR}/vendor/mimalloc/src/static.c")
if(EXISTS "${ASTRA_MIMALLOC_SRC}")
    set(mimalloc_obj "${ASTRA_OBJ_DIR}/cc_rt_mimalloc.o")
    # MI_MALLOC_OVERRIDE interposes malloc/calloc/realloc/free/posix_memalign
    # so every runtime allocation (xmalloc/xrealloc/GC blocks) runs on
    # mimalloc's size-classed allocator. POSIX hosts only: the Win32 CRT
    # pairing (_aligned_malloc/_aligned_free) is not override-safe.
    set(ASTRA_MIMALLOC_DEFS "")
    if(NOT WIN32)
        set(ASTRA_MIMALLOC_DEFS -DMI_MALLOC_OVERRIDE=1)
    endif()
    add_custom_command(OUTPUT "${mimalloc_obj}"
        COMMAND ${CMAKE_C_COMPILER} ${ASTRA_RT_CC_FLAGS} ${ASTRA_MIMALLOC_DEFS}
                -I${ASTRA_RUNTIME_DIR}/vendor/mimalloc/include
                -c "${ASTRA_MIMALLOC_SRC}" -o "${mimalloc_obj}"
        DEPENDS "${ASTRA_MIMALLOC_SRC}"
        COMMENT "system runtime mimalloc"
        VERBATIM)
    list(APPEND ASTRA_RT_OBJS "${mimalloc_obj}")
endif()
set(ASTRA_RUNTIME_ARCHIVE "${ASTRA_STAGE}/libaruntime.a")
add_custom_command(OUTPUT "${ASTRA_RUNTIME_ARCHIVE}"
    COMMAND ${CMAKE_AR} rcs "${ASTRA_RUNTIME_ARCHIVE}" ${ASTRA_RT_OBJS}
    DEPENDS ${ASTRA_RT_OBJS}
    COMMENT "system C runtime libaruntime.a"
    VERBATIM)

add_custom_target(aruntime ALL
    DEPENDS ${ASTRA_RUNTIME_STAGED_OUTPUTS} "${ASTRA_RUNTIME_ARCHIVE}")

message(STATUS "aruntime: staged A runtime -> ${ASTRA_STAGE}")
