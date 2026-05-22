add_library(declgl_renderer OBJECT
    renderable_walker.cc
    program_base.cc
    decl_program_registry.cc
    programs/triangle_program.cc
    programs/rect_program.cc
    programs/circle_program.cc
    programs/texture_program.cc
    programs/poly_program.cc
    programs/effect_programs.cc
    programs/textbox_program.cc
    programs/dynamic_program.cc
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
        declgl_log
        declgl_shaders
        glad
)
