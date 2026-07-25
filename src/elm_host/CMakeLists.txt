file(READ "${CMAKE_CURRENT_SOURCE_DIR}/headless_dom.js" HEADLESS_DOM_SOURCE)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/headless_dom.inc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/headless_dom.inc"
    @ONLY
)

add_library(declgl_elm_host STATIC
    elm_adapter.cc
    elm_adapter.h
    elm_host.cc
    elm_host.h
)

target_include_directories(declgl_elm_host
    PUBLIC ${CMAKE_SOURCE_DIR}/src
    PRIVATE ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(declgl_elm_host
    PUBLIC
        declgl_runtime
        declgl_engine
        declgl_renderer
        declgl_gpu
        declgl_resources
        declgl_audio
        declgl_shaders
        declgl_proto
        declgl_log
        glad
        stb_image
        stb_vorbis
        qjs
        SDL3::SDL3
)

set_target_properties(declgl_elm_host PROPERTIES POSITION_INDEPENDENT_CODE ON)
