add_executable(declgl_replay main.cc)

target_link_libraries(declgl_replay
    PRIVATE
        declgl
        declgl_proto)

target_include_directories(declgl_replay
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
