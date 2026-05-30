# runtime — transport-agnostic SDL/GL run loop.
#
# Owns SDL window + GL ctx ownership, the per-frame loop, dispatch over
# BackendCommand, frame pacing, and profiling. Hosts (OCaml caml_bridge
# or future gRPC player) implement LoopHooks (loop_hooks.h) and feed it
# into a Runtime instance.

add_library(declgl_runtime OBJECT
    loop_hooks.h
    runtime.h
    runtime.cc
)

set_target_properties(declgl_runtime PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(declgl_runtime
    PUBLIC
        ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(declgl_runtime
    PUBLIC
        declgl_engine
        declgl_proto
        declgl_log
        SDL3::SDL3
)
