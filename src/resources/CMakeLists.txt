# resources — texture loading + GL upload pipeline.
#
# Owns the desktop-side equivalent of the JS backend's `Image()`-driven
# texture lifecycle: decode bytes → upload to GPU → register by name.
# Engine talks to this module; renderer/walker reads from it.

add_library(declgl_resources STATIC
    texture.cc
    texture_registry.cc
    image_decoder.cc
)

set_target_properties(declgl_resources PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(declgl_resources PUBLIC ${CMAKE_SOURCE_DIR}/src)

# stb_image is a vendored static lib living in third_party/. We pull
# its include path AND its TU through PUBLIC so consumers (engine.cc)
# can include "resources/*.h" without separately depending on stb.
target_link_libraries(declgl_resources
    PUBLIC
        glad
    PRIVATE
        stb_image
)
