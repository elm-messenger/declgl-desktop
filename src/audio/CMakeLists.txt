# audio — declarative audio runtime for declgl-desktop.
#
# Owns the SDL3 audio device, decoder dispatch (WAV via SDL3 builtin,
# OGG Vorbis via stb_vorbis), and the per-voice mixer.

add_library(declgl_audio OBJECT
    audio_decoder.cc
    audio_engine.cc
)

set_target_properties(declgl_audio PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(declgl_audio PUBLIC ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(declgl_audio
    PUBLIC
        declgl_proto
        SDL3::SDL3
    PRIVATE
        stb_vorbis
)
