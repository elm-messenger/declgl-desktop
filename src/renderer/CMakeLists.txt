add_library(declgl_renderer STATIC
    renderable_walker.cc
)

set_target_properties(declgl_renderer PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(declgl_renderer
    PUBLIC
        ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(declgl_renderer
    PUBLIC
        declgl_proto
        declgl_gpu
        declgl_resources
        glad
)
