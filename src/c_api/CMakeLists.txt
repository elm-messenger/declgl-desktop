# libdeclgl - the public engine library. The thin C ABI shim is in this
# directory; everything it talks to comes from declgl_engine.

add_library(declgl STATIC
    declgl.cc
)

set_target_properties(declgl PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(declgl
    PUBLIC
        ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(declgl
    PUBLIC
        declgl_engine
)

# DECLGL_API needs visibility("default") in extern "C" so the FFI can find
# symbols even though the global default is hidden.
target_compile_definitions(declgl PRIVATE DECLGL_BUILDING_DLL)
