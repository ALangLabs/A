add_library(a_optimization STATIC
    optimizer_constfold.cpp
    optimizer_dce.cpp
    optimizer_inline.cpp
    optimizer_licm.cpp
    optimizer_level.cpp
    optimizer_simplify.cpp
    optimizer_instcombine.cpp
    optimizer_sccp.cpp
    optimizer_pipeline.cpp
)
target_link_libraries(a_optimization PUBLIC a_ir)
add_library(a::optimization ALIAS a_optimization)
