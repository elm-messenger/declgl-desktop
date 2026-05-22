# shaders/CMakeLists.txt — vendor JS-built-in GLSL into an object library.
#
# Each .glsl file under this directory is embedded as a `const char*` in
# the generated `builtin_shaders.cc`. The lookup is exposed via
# `declgl::builtin_shader_source(name, kind)` from `builtin_shaders.h`.
#
# Naming convention: <program>.<vert|frag>.glsl
#
#   triangle.vert.glsl  → ("triangle", VERT)
#   triangle.frag.glsl  → ("triangle", FRAG)

set(_SHADER_FILES
    triangle.vert.glsl
    triangle.frag.glsl
    rect.vert.glsl
    rect.frag.glsl
    circle.vert.glsl
    circle.frag.glsl
    roundedRect.frag.glsl
    texture.vert.glsl
    texture.frag.glsl
    textureCentered.vert.glsl
    textureCentered.frag.glsl
    textureCroppedCentered.vert.glsl
    effect.vert.glsl
    palette.frag.glsl
    defaultCompositor.frag.glsl
    compFade.frag.glsl
    alphamult.frag.glsl
    colormult.frag.glsl
    blurh.frag.glsl
    blurv.frag.glsl
    gblur.frag.glsl
    crt.frag.glsl
    fxaa.vert.glsl
    fxaa.frag.glsl
    outline.frag.glsl
    pixilation.frag.glsl
    imgFade.frag.glsl
    text.vert.glsl
    text.frag.glsl
)

set(_GEN_HDR "${CMAKE_CURRENT_BINARY_DIR}/builtin_shaders.h")
set(_GEN_SRC "${CMAKE_CURRENT_BINARY_DIR}/builtin_shaders.cc")
set(_GEN_SCRIPT "${CMAKE_SOURCE_DIR}/scripts/generate_builtin_shaders.py")
find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_custom_command(
    OUTPUT ${_GEN_SRC} ${_GEN_HDR}
    COMMAND
        "${Python3_EXECUTABLE}" "${_GEN_SCRIPT}"
        --shader-dir "${CMAKE_CURRENT_SOURCE_DIR}"
        --header-out "${_GEN_HDR}"
        --source-out "${_GEN_SRC}"
        ${_SHADER_FILES}
    DEPENDS ${_SHADER_FILES} "${_GEN_SCRIPT}"
    COMMENT "Embedding builtin GLSL shaders -> builtin_shaders.{h,cc}"
    VERBATIM
)

add_library(declgl_shaders OBJECT ${_GEN_SRC} ${_GEN_HDR})
set_target_properties(declgl_shaders PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(declgl_shaders PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
