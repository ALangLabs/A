# Astra toolchain support targets. The Astra fork itself is built by
# cmake/libastra.cmake (libastra + the `astra` driver + staged astralib/); this file
# covers the C++ pieces that live alongside it.

# C++ runtime support library (Value, utf8/format, file io) used by the
# frontend; lives next to the C runtime modules in runtime/.
add_subdirectory(runtime)

# C++ frontend (lex/parse/sema/IR/pipeline), formerly compiler/stages/.
add_subdirectory(frontend)
