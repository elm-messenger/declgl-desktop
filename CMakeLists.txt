cmake_minimum_required(VERSION 3.20)

# Allow older sub-projects (vcpkg ports) that still set
# cmake_minimum_required(VERSION 3.0).
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

project(declgl_desktop
    VERSION 0.0.1
    LANGUAGES C CXX
)

# ----------------------------------------------------------------------------
# Global settings
# ----------------------------------------------------------------------------
set(CMAKE_C_STANDARD              11)
set(CMAKE_C_STANDARD_REQUIRED     ON)
set(CMAKE_CXX_STANDARD            17)
set(CMAKE_CXX_STANDARD_REQUIRED   ON)
set(CMAKE_CXX_EXTENSIONS          OFF)

# Hide symbols by default. The OCaml caml_bridge's `extern "C" CAMLprim`
# entry points are resolved at static link time, not dlopen — so we don't
# need them visible in the final shared lib (there isn't one).
set(CMAKE_C_VISIBILITY_PRESET     hidden)
set(CMAKE_CXX_VISIBILITY_PRESET   hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Common warning flags. Kept simple, no fancy lint options yet.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU|AppleClang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

# Silence the "object file was built for newer 'macOS' version" link
# warnings: vcpkg compiles deps against the host SDK, but we deploy to a
# fixed older target. Functionally fine; the warnings are pure noise.
if(APPLE)
    add_link_options(-Wl,-w)
endif()

# ----------------------------------------------------------------------------
# Dependencies (vcpkg manifest mode -> these are auto-installed on configure)
# ----------------------------------------------------------------------------
find_package(glm       CONFIG REQUIRED)
find_package(Protobuf  CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(quill     CONFIG REQUIRED)

find_package(SDL3      CONFIG REQUIRED)

# Bring in the modern target-based protobuf_generate() helper.
include("${Protobuf_DIR}/protobuf-generate.cmake")

# ----------------------------------------------------------------------------
# Third-party (vendored single-file libs)
# ----------------------------------------------------------------------------
add_subdirectory(third_party/glad)
add_subdirectory(third_party/stb)

# ----------------------------------------------------------------------------
# First-party
# ----------------------------------------------------------------------------
add_subdirectory(src/proto_gen)
add_subdirectory(shaders)
add_subdirectory(src/log)
add_subdirectory(src/gpu)
add_subdirectory(src/resources)
add_subdirectory(src/audio)
add_subdirectory(src/renderer)
add_subdirectory(src/engine)
add_subdirectory(src/caml_bridge)

# ----------------------------------------------------------------------------
# libdeclgl.a — a single static archive that the OCaml dune side picks up
# via (foreign_archives declgl). We build it directly from the object files
# emitted by the first-party/object-library targets that the OCaml-linked
# binary needs at link time:
#   - declgl_caml_bridge  (extern "C" CAMLprim entry points)
#   - declgl_engine       (SDL/GL + per-command dispatch)
#   - declgl_proto        (generated protobuf C++ stubs)
#   - glad                (GL loader)
#
# The third-party archives we depend on (SDL3, protobuf, abseil, ...) stay
# as separate -l flags fed to dune via c_library_flags — folding those into
# libdeclgl.a would balloon the archive and is brittle across vcpkg layouts.
# ----------------------------------------------------------------------------
add_library(declgl STATIC
    $<TARGET_OBJECTS:declgl_caml_bridge>
    $<TARGET_OBJECTS:declgl_engine>
    $<TARGET_OBJECTS:declgl_renderer>
    $<TARGET_OBJECTS:declgl_gpu>
    $<TARGET_OBJECTS:declgl_resources>
    $<TARGET_OBJECTS:declgl_audio>
    $<TARGET_OBJECTS:declgl_shaders>
    $<TARGET_OBJECTS:declgl_proto>
    $<TARGET_OBJECTS:declgl_log>
    $<TARGET_OBJECTS:glad>
    $<TARGET_OBJECTS:stb_image>
    $<TARGET_OBJECTS:stb_vorbis>
)

set_target_properties(declgl PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    POSITION_INDEPENDENT_CODE ON
)

# ----------------------------------------------------------------------------
# Dune integration: emit two artefacts that the OCaml side ingests via
# (foreign_archives ...) + (c_library_flags (:include ...)).
#
#   1. libdeclgl.a (already produced above) — bundled first-party code.
#   2. declgl_link_flags.sexp — third-party link flags as an S-expression
#      that dune can splice into c_library_flags. A dedicated Python helper
#      writes the file from the resolved vcpkg/build paths.
#
# The .sexp lives next to libdeclgl.a so the OCaml side can point at one
# directory and find both. We always re-generate it at configure time once
# vcpkg has resolved.
# ----------------------------------------------------------------------------
set(_DECLGL_LINK_SEXP "${CMAKE_BINARY_DIR}/declgl_link_flags.sexp")
set(_VCPKG_LIB        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib")
set(_DECLGL_LINK_SCRIPT "${CMAKE_SOURCE_DIR}/scripts/write_declgl_link_flags.py")
find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(_DECLGL_LINK_SCRIPT_ARGS
    "${_DECLGL_LINK_SCRIPT}"
    --vcpkg-lib "${_VCPKG_LIB}"
    --build-dir "${CMAKE_BINARY_DIR}"
    --output "${_DECLGL_LINK_SEXP}"
    --os-name "${CMAKE_SYSTEM_NAME}"
)

execute_process(
    COMMAND "${Python3_EXECUTABLE}" ${_DECLGL_LINK_SCRIPT_ARGS}
    COMMAND_ERROR_IS_FATAL ANY
)

message(STATUS "[declgl] wrote dune link flags -> ${_DECLGL_LINK_SEXP}")
