add_library(a_type_checker STATIC
    sema_core.cpp
    sema_decls.cpp
    sema_module.cpp
    sema_stmt.cpp
    sema_expr.cpp
    sema_constructors.cpp
    sema_calls.cpp
    sema_builtins.cpp
    sema_builtins_collections.cpp
    sema_builtins_io.cpp
    sema_builtins_json.cpp
    sema_builtins_kernels.cpp
    sema_builtins_log.cpp
    sema_builtins_memory.cpp
    sema_builtins_metrics.cpp
    sema_builtins_net.cpp
    sema_builtins_numeric.cpp
    sema_builtins_system.cpp
    sema_builtins_testing.cpp
    sema_builtins_trace.cpp
    sema_builtins_values.cpp
    sema_literals.cpp
    sema_pattern.cpp
    sema_checks.cpp
)
target_link_libraries(a_type_checker PUBLIC a_name_resolution a_tooling)
add_library(a::type_checker ALIAS a_type_checker)
