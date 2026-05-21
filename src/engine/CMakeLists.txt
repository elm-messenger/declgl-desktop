add_library(declgl_engine OBJECT
    engine.cc
)

set_target_properties(declgl_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(declgl_engine
    PUBLIC
        ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(declgl_engine
    PUBLIC
        declgl_proto
        declgl_renderer
        declgl_gpu
        declgl_resources
        SDL3::SDL3
        glad
        glm::glm
)
