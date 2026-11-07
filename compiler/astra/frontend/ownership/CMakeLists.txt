add_library(a_ownership STATIC
  ownership.cpp
  ownership_borrow.cpp
  ownership_expr.cpp
  ownership_snapshot.cpp
  ownership_state.cpp
  ownership_stmt.cpp
)
target_link_libraries(a_ownership PUBLIC a_type_checker)
add_library(a::ownership ALIAS a_ownership)
