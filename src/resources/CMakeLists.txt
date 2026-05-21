# resources — texture loading + GL upload pipeline.
#
# Owns the desktop-side equivalent of the JS backend's `Image()`-driven
# texture lifecycle: decode bytes → upload to GPU → register by name.
# Engine talks to this module; renderer/walker reads from it.

add_library(declgl_resources OBJECT
    texture.cc
    texture_registry.cc
    image_decoder.cc
    font.cc
    font_registry.cc
    asset_loader.cc
)

# AssetLoader uses std::thread / std::condition_variable.
find_package(Threads REQUIRED)

set_target_properties(declgl_resources PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(declgl_resources PUBLIC ${CMAKE_SOURCE_DIR}/src)

# stb_image is a vendored object lib living in third_party/. We pull
# its include path AND its TU through PUBLIC so consumers (engine.cc)
# can include "resources/*.h" without separately depending on stb.
target_link_libraries(declgl_resources
    PUBLIC
        glad
        Threads::Threads
    PRIVATE
        stb_image
        nlohmann_json::nlohmann_json
        SDL3::SDL3
)

# resources includes audio/audio_decoder.h. We don't need the audio TU
# linked here (declgl_audio is its own object lib already pulled into
# the top-level archive), but we need its include path. The convention
# in this tree is "everything under src/ is on the include path" —
# already covered by [target_include_directories] above pointing at
# ${CMAKE_SOURCE_DIR}/src.
