# In-process MLIR infrastructure for the A toolchain (KGEN-style):
# `a` dialect, pass pipeline, bytecode (.ambc) packaging, and the
# in-process LLVM backend that replaces the mlir-translate/opt/llc/clang
# subprocess chain. See docs/mlir-architecture.md.
#
# Sources are added by the workstream that owns them; this file lists the
# expected set and is kept in sync at integration time.

# MLIR/LLVM 18 dev files are installed system-wide but not on CMake's
# default search path (only LLVMConfig is, via LLVM_DIR). Probe the standard
# llvm-<ver> prefixes before find_package so distro installs Just Work.
if(NOT MLIR_DIR OR MLIR_DIR STREQUAL "MLIR_DIR-NOTFOUND")
    foreach(_prefix
            /usr/lib/llvm-18 /usr/lib/llvm-17 /usr/lib/llvm-16 /usr/lib/llvm
            /usr/local/lib/cmake/mlir /usr/lib/cmake/mlir)
        if(EXISTS "${_prefix}/lib/cmake/mlir/MLIRConfig.cmake")
            set(MLIR_DIR "${_prefix}/lib/cmake/mlir" CACHE PATH FORCE
                "Directory containing MLIRConfig.cmake")
            break()
        elseif(EXISTS "${_prefix}/MLIRConfig.cmake")
            set(MLIR_DIR "${_prefix}" CACHE PATH FORCE
                "Directory containing MLIRConfig.cmake")
            break()
        endif()
    endforeach()
endif()
find_package(MLIR CONFIG QUIET)
if(NOT LLVM_DIR OR LLVM_DIR STREQUAL "LLVM_DIR-NOTFOUND")
    foreach(_prefix /usr/lib/llvm-18 /usr/lib/llvm-17 /usr/lib/llvm-16
                   /usr/lib/llvm /usr/local)
        if(EXISTS "${_prefix}/lib/cmake/llvm/LLVMConfig.cmake")
            set(LLVM_DIR "${_prefix}/lib/cmake/llvm" CACHE PATH FORCE
                "Directory containing LLVMConfig.cmake")
            break()
        endif()
    endforeach()
endif()
find_package(LLVM CONFIG QUIET)

if(MLIR_FOUND)
    # MLIRConfig.cmake sets MLIR_TABLEGEN_EXE to the bare "mlir-tblgen" name;
    # resolve it against the LLVM tools dir / PATH so the custom commands work
    # when llvm-18/bin is not on PATH.
    find_program(MLIR_TABLEGEN_EXE_ABS NAMES ${MLIR_TABLEGEN_EXE} mlir-tblgen
        PATHS ${LLVM_TOOLS_BINARY_DIR} /usr/lib/llvm-18/bin /usr/bin
        NO_DEFAULT_PATH)
    if(MLIR_TABLEGEN_EXE_ABS)
        set(MLIR_TABLEGEN_EXE ${MLIR_TABLEGEN_EXE_ABS})
    endif()
endif()

# Core sources: everything that does not depend on the frontend.
# package.cpp/package_cli.cpp are the exception - they call back into the
# checked-AST pipeline (runFrontendPipeline/emitMLIR), so a_mlir_core links
# a::pipeline + a::mlir (frontend emitter) PUBLICly; the frontend never links
# back (pipeline.cpp forward-declares the native entry points and the `a`
# executable resolves them from a_mlir_core).
set(A_MLIR_CORE_SOURCES
    ambc.cpp
    ambc_modules.cpp
    bytecode.cpp
    codegen.cpp
    native.cpp
    pipeline.cpp
    driver.cpp
    dialect/ADialect.cpp
    dialect/AOps.cpp
    dialect/ATypes.cpp
    passes/ALowerToLLVM.cpp
    passes/ALowerTyped.cpp
)

# Frontend-coupled sources: only meaningful when the frontend exists.
# These TUs include MLIR headers, so they are excluded entirely from the
# no-MLIR stub build (see stubs/ below).
set(A_MLIR_PACKAGE_SOURCES
    package.cpp
    package_cli.cpp
)
set(A_MLIR_PACKAGE_STUBS
    stubs/package_cli_stub.cpp
    stubs/driver_stub.cpp
)

if(MLIR_FOUND AND LLVM_FOUND)
    # ---- TableGen: `a` dialect .inc files ---------------------------------
    # The dialect .td files live under include/A/; generated headers land in
    # ${CMAKE_CURRENT_BINARY_DIR}/include/A/ so `#include "A/AOps.h.inc"`
    # resolves. mlir-tblgen needs -I <dir containing mlir/... .td> plus the
    # dialect include dir for the relative "A/*.td" includes.
    set(A_DIALECT_TD_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)
    set(A_TD_INCLUDES -I ${A_DIALECT_TD_DIR} -I ${MLIR_INCLUDE_DIRS})
    set(A_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/include/A)
    file(MAKE_DIRECTORY ${A_GEN_DIR})

    set(A_TD_DEFS
        ADialect.td   # dialect decls/defs
        ATypes.td     # type decls/defs
        AOps.td       # op decls/defs + enum attr decls/defs
    )
    # (output-file, mlir-tblgen args...) — '|' separated: ';' cannot appear
    # inside a CMake list element, so foreach would otherwise split the
    # rules into their parts.
    set(A_TG_RULES
        "ADialect.h.inc|-gen-dialect-decls|-dialect=a|ADialect.td"
        "ADialect.cpp.inc|-gen-dialect-defs|-dialect=a|ADialect.td"
        "ATypes.h.inc|-gen-typedef-decls|-typedefs-dialect=a|ATypes.td"
        "ATypes.cpp.inc|-gen-typedef-defs|-typedefs-dialect=a|ATypes.td"
        "AOps.h.inc|-gen-op-decls|AOps.td"
        "AOps.cpp.inc|-gen-op-defs|AOps.td"
        "AEnums.h.inc|-gen-enum-decls|AOps.td"
        "AEnums.cpp.inc|-gen-enum-defs|AOps.td"
    )
    set(A_TG_OUTPUTS "")
    foreach(rule ${A_TG_RULES})
        string(REPLACE "|" ";" rule_parts "${rule}")
        list(GET rule_parts 0 ofn)
        list(LENGTH rule_parts nparts)
        math(EXPR argend "${nparts} - 2")
        list(SUBLIST rule_parts 1 ${argend} args)  # drop trailing .td input
        set(out ${A_GEN_DIR}/${ofn})
        list(APPEND A_TG_OUTPUTS ${out})
        # The .td input is the last arg.
        list(GET rule_parts -1 tdfile)
        add_custom_command(
            OUTPUT ${out}
            COMMAND ${MLIR_TABLEGEN_EXE} ${args} ${A_TD_INCLUDES}
                    ${A_DIALECT_TD_DIR}/A/${tdfile}
                    -o ${out}
            DEPENDS ${A_DIALECT_TD_DIR}/A/${tdfile}
            COMMENT "Building ${ofn}"
            VERBATIM)
    endforeach()
    add_custom_target(a_dialect_tablegen DEPENDS ${A_TG_OUTPUTS})

    add_library(a_mlir_core STATIC ${A_MLIR_CORE_SOURCES} ${A_MLIR_PACKAGE_SOURCES})
    add_dependencies(a_mlir_core a_dialect_tablegen)
    # MLIR/LLVM headers are not -Wpedantic/-Wall clean (e.g. unused-parameter
    # in template instantiation); mark them SYSTEM so the project's
    # -Werror flags don't fire inside upstream headers.
    target_include_directories(a_mlir_core SYSTEM PUBLIC
        ${MLIR_INCLUDE_DIRS} ${LLVM_INCLUDE_DIRS})
    target_include_directories(a_mlir_core PUBLIC
        ${CMAKE_SOURCE_DIR}/src/astra
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}/include)
    target_link_libraries(a_mlir_core PUBLIC
        MLIRIR MLIRParser MLIRBytecodeReader MLIRBytecodeWriter
        MLIRPass MLIRTransforms
        MLIRBuiltinToLLVMIRTranslation MLIRLLVMToLLVMIRTranslation
        MLIRTargetLLVMIRExport MLIRToLLVMIRTranslationRegistration
        MLIRFromLLVMIRTranslationRegistration MLIRTargetCpp
        MLIRSPIRVTranslateRegistration MLIRIndexDialect
        LLVMSupport)
    # package.cpp/package_cli.cpp call the frontend pipeline + emitter;
    # a::mlir_core is self-contained for consumers (the `a` binary links it
    # after a_driver, which pulls in a_pipeline/a_mlir anyway).
    target_link_libraries(a_mlir_core PUBLIC
        a::pipeline a::mlir)
    target_compile_features(a_mlir_core PUBLIC cxx_std_20)
    # TableGen-generated op/attr/type .inc files are known to trip
    # -Wunused-parameter under -Wextra (they emit unused `loc`/`odsBuilder`
    # parameters in verifiers). LLVM's own build disables this warning for
    # generated code; do the same for the whole mlir infra target.
    if(NOT MSVC)
        target_compile_options(a_mlir_core PRIVATE -Wno-unused-parameter)
    endif()
    # Tell pipeline.cpp (frontend) that the native entry points exist so it
    # compiles the A_MLIR_NATIVE-gated call sites; a_pipeline picks up the
    # definition here.
    get_target_property(_a_mlir_type a_mlir_core TYPE)
    if(_a_mlir_type STREQUAL "STATIC_LIBRARY")
        target_compile_definitions(a_mlir_core PUBLIC A_WITH_MLIR_CORE=1)
    endif()
    add_library(a::mlir_core ALIAS a_mlir_core)
else()
    # No MLIR dev: the `a` dialect, passes, bytecode and native codegen are
    # unavailable. Provide stubs for the two CLI entry points so `a` still
    # links; the native compile path is #ifdef'd out in the frontend and the
    # subprocess backend remains the default.
    add_library(a_mlir_core STATIC ${A_MLIR_PACKAGE_STUBS})
    target_include_directories(a_mlir_core PUBLIC
        ${CMAKE_SOURCE_DIR}/src/astra
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_features(a_mlir_core PUBLIC cxx_std_20)
    add_library(a::mlir_core ALIAS a_mlir_core)
endif()
