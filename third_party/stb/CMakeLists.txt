# stb (image loader + ogg vorbis) — vendored single-header sources.
#
# We compile each implementation into one TU so it can be linked once
# into the final bundle. Consumers just `#include "stb_image.h"` /
# `#include "stb_vorbis.h"` (the latter is just the same `.c` file
# with `STB_VORBIS_HEADER_ONLY` defined; see audio_decoder.cc) without
# defining the implementation macros themselves.

add_library(stb_image OBJECT
    stb_image.cc
)

target_include_directories(stb_image PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

set_target_properties(stb_image PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

# stb_image.h has a few warnings under -Wall that are harmless and not
# under our control; suppress only on this target so they don't drown
# out our own warnings elsewhere.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(stb_image PRIVATE
        -Wno-implicit-fallthrough
        -Wno-unused-but-set-variable
        -Wno-unused-function
    )
endif()

# ----------------------------------------------------------------------------
# stb_vorbis — Ogg Vorbis decoder. Upstream ships as `stb_vorbis.c`
# which both declares and defines (no separate header). We compile it
# directly as a C source. The pushdata API is disabled (we only use
# the pulldata helpers `stb_vorbis_decode_filename` /
# `stb_vorbis_decode_memory`), which trims a couple of KB of code.
# ----------------------------------------------------------------------------
add_library(stb_vorbis OBJECT
    stb_vorbis.c
)

target_include_directories(stb_vorbis PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

set_target_properties(stb_vorbis PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_STANDARD 99
)

target_compile_definitions(stb_vorbis PRIVATE
    STB_VORBIS_NO_PUSHDATA_API
)

# stb_vorbis.c is generally `-Wall`-clean enough but does trip a few
# -Wextra-tier warnings we don't control. Mute them on this TU only.
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU|AppleClang")
    target_compile_options(stb_vorbis PRIVATE
        -Wno-unused-value
        -Wno-unused-but-set-variable
        -Wno-unused-function
        -Wno-implicit-fallthrough
        -Wno-sign-compare
        -Wno-misleading-indentation
        -Wno-unused-parameter
    )
endif()
