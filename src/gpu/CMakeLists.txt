add_library(declgl_gpu STATIC
    program.cc
    program_registry.cc
    fbo_pool.cc
)

set_target_properties(declgl_gpu PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(declgl_gpu
    PUBLIC
        ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(declgl_gpu
    PUBLIC
        glad
        declgl_shaders
)
