add_library(declgl_log OBJECT
    log.cc
)

set_target_properties(declgl_log PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(declgl_log
    PUBLIC
        ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(declgl_log
    PUBLIC
        quill::quill
)
