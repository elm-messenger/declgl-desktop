# caml_bridge — OCaml ↔ C++ glue for the desktop ml_regl backend.
#
# Builds an object library that exposes the two C symbols the OCaml side
# declares as `external`:
#   - declgl_ship_backend_cmd
#   - declgl_ship_audio_cmd
#
# There is intentionally no separate run-loop entry point: the loop is
# triggered by the protocol-level [StartRegl] command, just like the JS
# backend's `start()`. This archive plus declgl_engine + declgl_proto +
# GLAD get bundled into the final libdeclgl.a that the OCaml dune side
# picks up via (foreign_archives declgl).

# ----------------------------------------------------------------------------
# Locate the OCaml runtime headers (caml/callback.h, caml/mlvalues.h, ...).
# ----------------------------------------------------------------------------
find_program(OCAMLC_EXECUTABLE ocamlc REQUIRED)

execute_process(
    COMMAND ${OCAMLC_EXECUTABLE} -where
    OUTPUT_VARIABLE OCAML_LIB_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

if(NOT OCAML_LIB_DIR)
    message(FATAL_ERROR
        "Could not locate the OCaml stdlib path via 'ocamlc -where'.")
endif()

message(STATUS "[caml_bridge] OCaml stdlib at: ${OCAML_LIB_DIR}")

add_library(declgl_caml_bridge OBJECT
    declgl_caml.cc
)

set_target_properties(declgl_caml_bridge PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(declgl_caml_bridge
    PUBLIC
        ${OCAML_LIB_DIR}
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(declgl_caml_bridge
    PUBLIC
        declgl_runtime
        declgl_engine
        declgl_proto
        declgl_log
        SDL3::SDL3
)
