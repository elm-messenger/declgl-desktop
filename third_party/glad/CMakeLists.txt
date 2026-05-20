# GLAD2 GL 3.3 Core loader (vendored).
# Generated from https://gen.glad.sh/ — regenerate by hand if the GL version changes.

add_library(glad STATIC
    src/gl.c
)

target_include_directories(glad PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# We link to the system GL only on Linux/Windows. On macOS GLAD2's loader
# uses dlopen / dlsym to find OpenGL.framework, so no extra link is needed.
if(UNIX AND NOT APPLE)
    find_package(OpenGL REQUIRED)
    target_link_libraries(glad PUBLIC OpenGL::GL ${CMAKE_DL_LIBS})
elseif(WIN32)
    target_link_libraries(glad PUBLIC opengl32)
endif()

set_target_properties(glad PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
