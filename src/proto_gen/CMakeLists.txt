# Generated protobuf C++ sources for declgl-desktop.
#
# We collect the four .proto files from the sibling `proto/` directory and
# turn them into a small static library (`declgl_proto`) that other parts of
# the engine can link against. Files are generated into the build tree, not
# into the source tree.

set(_PROTO_SRC_DIR "${CMAKE_SOURCE_DIR}/proto")
set(_PROTO_FILES
    "${_PROTO_SRC_DIR}/transport_common.proto"
    "${_PROTO_SRC_DIR}/transport_backend.proto"
    "${_PROTO_SRC_DIR}/transport_render.proto"
    "${_PROTO_SRC_DIR}/transport_audio.proto"
)

# Output goes into ${CMAKE_CURRENT_BINARY_DIR}/proto_gen so the include
# path is always `#include "proto_gen/transport_*.pb.h"`.
set(_PROTO_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")

add_library(declgl_proto STATIC)
set_target_properties(declgl_proto PROPERTIES POSITION_INDEPENDENT_CODE ON)

# protobuf_generate() is the modern per-target API. It attaches generated
# .pb.{h,cc} sources to the target and adds the required include dir.
protobuf_generate(
    TARGET declgl_proto
    LANGUAGE cpp
    PROTOS ${_PROTO_FILES}
    IMPORT_DIRS "${_PROTO_SRC_DIR}"
    PROTOC_OUT_DIR "${_PROTO_OUT_DIR}"
)

target_link_libraries(declgl_proto PUBLIC
    protobuf::libprotobuf-lite
)

# Consumers do `#include "proto_gen/transport_backend.pb.h"`.
# protobuf_generate() puts files at ${_PROTO_OUT_DIR}/transport_backend.pb.h
# so we expose the parent dir of proto_gen by exposing _PROTO_OUT_DIR's
# parent. Simpler: just expose _PROTO_OUT_DIR directly and write
# `#include "transport_backend.pb.h"` (no proto_gen/ prefix).
target_include_directories(declgl_proto PUBLIC
    "${_PROTO_OUT_DIR}"
)
