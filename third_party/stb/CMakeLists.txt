# stb (image loader) — vendored single-header.
#
# We compile the implementation into one TU (`stb_image.cc`) so it can be
# linked once into the final bundle. Consumers just `#include "stb_image.h"`
# without defining STB_IMAGE_IMPLEMENTATION themselves.

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
