if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/integration/run_tests.sh")
  add_test(NAME language-suite
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/integration/run_tests.sh $<TARGET_FILE:a>)
endif()
# Run-cache self-test (content-addressed `a run` binary reuse + invalidation).
add_test(NAME run-cache
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/integration/cache_test.sh $<TARGET_FILE:a>)
add_executable(a-tooling-tests tooling/tooling_tests.cpp)
target_link_libraries(a-tooling-tests PRIVATE a_tooling)
add_test(NAME tooling-tests COMMAND a-tooling-tests)
add_executable(a-ir-tests ir/ir_tests.cpp)
target_link_libraries(a-ir-tests PRIVATE a_ir a_optimization)
add_test(NAME ir-tests COMMAND a-ir-tests)
add_test(NAME tooling-cli
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tooling/run_cli_tests.sh $<TARGET_FILE:a>)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME language-server-smoke
    COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_SOURCE_DIR}/tools/lsp/smoke_test.py
      $<TARGET_FILE:a-lsp>)
endif()
