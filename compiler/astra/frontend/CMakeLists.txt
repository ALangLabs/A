# C++ frontend of the A toolchain (formerly compiler/stages/).
# Everything compiler-related lives under compiler/astra/; this
# directory holds the lex/parse/sema/IR/codegen/pipeline modules while
# ../runtime/, ../python/ and the Astra fork are the C backend.
# Modules in dependency order (each owns its CMakeLists.txt).
add_subdirectory(diagnostics)
add_subdirectory(lexer)
add_subdirectory(ast)
add_subdirectory(parser)
add_subdirectory(name-resolution)
add_subdirectory(type-checker)
add_subdirectory(ownership)
add_subdirectory(codegen)
add_subdirectory(ir)
add_subdirectory(optimization)
add_subdirectory(tooling)
add_subdirectory(vm)
add_subdirectory(pipeline)
add_subdirectory(driver)
add_subdirectory(cli)
