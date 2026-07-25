add_library(declgl_renderer OBJECT
    renderable_walker.cc
    program_base.cc
    decl_program_registry.cc
    programs/triangle_program.cc
    programs/rect_program.cc
    programs/circle_program.cc
    programs/texture_program.cc
    programs/poly_program.cc
    programs/effect_common.cc
    programs/palette_program.cc
    programs/default_compositor_program.cc
    programs/comp_fade_program.cc
    programs/alpha_mult_program.cc
    programs/color_mult_program.cc
    programs/blurh_program.cc
    programs/blurv_program.cc
    programs/gblurh_program.cc
    programs/gblurv_program.cc
    programs/crt_program.cc
    programs/fxaa_program.cc
    programs/outline_program.cc
    programs/pixilation_program.cc
    programs/img_fade_program.cc
    programs/textbox_program.cc
    programs/glsl_es_translator.cc
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
