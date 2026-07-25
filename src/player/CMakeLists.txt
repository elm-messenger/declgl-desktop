add_executable(declgl-player main.cc)

target_include_directories(declgl-player PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(declgl-player PRIVATE declgl_elm_host)

set_target_properties(declgl-player PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
