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

# ----------------------------------------------------------------------------
# Build options
# ----------------------------------------------------------------------------
# When ON (the default) we build the OCaml ↔ C++ bridge object library
# and bundle it into libdeclgl.a. Configure with -DBUILD_OCAML_BRIDGE=OFF
# to opt out — useful for non-OCaml hosts (e.g. the future declgl-player
# gRPC executable) and on machines without ocamlc in PATH.
option(BUILD_OCAML_BRIDGE "Build the OCaml ↔ C++ bridge target" ON)
option(BUILD_ELM_PLAYER "Build the standalone Elm/QuickJS player" ON)

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
if(BUILD_ELM_PLAYER)
    find_package(qjs CONFIG REQUIRED)
endif()

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
add_subdirectory(src/runtime)
if(BUILD_OCAML_BRIDGE)
    add_subdirectory(src/caml_bridge)
endif()
if(BUILD_ELM_PLAYER)
    add_subdirectory(src/elm_host)
    add_subdirectory(src/player)
endif()

# ----------------------------------------------------------------------------
# libdeclgl_base.a — first-party/object-library archive used as input for
# libdeclgl.a. The OCaml dune side picks up the final bundled archive via
# (foreign_archives declgl). We build the base archive directly from the
# object files emitted by the targets that the OCaml-linked binary needs at
# link time:
#   - declgl_caml_bridge  (extern "C" CAMLprim entry points)
#   - declgl_engine       (SDL/GL + per-command dispatch)
#   - declgl_proto        (generated protobuf C++ stubs)
#   - glad                (GL loader)
#
# A post-build step repacks this base archive together with the vcpkg static
# archives reported by pkg-config for protobuf-lite and SDL3. System libraries
# still stay as linker flags because normal static archives cannot contain
# libc/libstdc++/pthread/rt-style platform libraries.
# ----------------------------------------------------------------------------
add_library(declgl_base STATIC
    $<$<BOOL:${BUILD_OCAML_BRIDGE}>:$<TARGET_OBJECTS:declgl_caml_bridge>>
    $<TARGET_OBJECTS:declgl_runtime>
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

set_target_properties(declgl_base PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    OUTPUT_NAME "declgl_base"
    POSITION_INDEPENDENT_CODE ON
)

# ----------------------------------------------------------------------------
# Dune integration: emit two artefacts that the OCaml side ingests via
# (foreign_archives ...) + (c_library_flags (:include ...)).
#
#   1. libdeclgl.a — first-party code plus bundled vcpkg static archives.
#   2. declgl_link_flags.sexp — remaining link flags as an S-expression
#      that dune can splice into c_library_flags. A dedicated Python helper
#      writes the file from the resolved vcpkg/build paths.
#
# The .sexp lives next to libdeclgl.a so the OCaml side can point at one
# directory and find both. Both outputs are tracked build artefacts.
# ----------------------------------------------------------------------------
set(_DECLGL_LINK_SEXP "${CMAKE_BINARY_DIR}/declgl_link_flags.sexp")
set(_DECLGL_ARCHIVE   "${CMAKE_BINARY_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}declgl${CMAKE_STATIC_LIBRARY_SUFFIX}")
set(_VCPKG_LIB        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib")
set(_DECLGL_LINK_SCRIPT "${CMAKE_SOURCE_DIR}/scripts/write_declgl_link_flags.py")
set(_DECLGL_BUNDLE_SCRIPT "${CMAKE_SOURCE_DIR}/scripts/bundle_static_archive.py")
find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_custom_command(
    OUTPUT "${_DECLGL_ARCHIVE}"
    COMMAND "${Python3_EXECUTABLE}" "${_DECLGL_BUNDLE_SCRIPT}"
        --input "$<TARGET_FILE:declgl_base>"
        --output "${_DECLGL_ARCHIVE}"
        --vcpkg-lib "${_VCPKG_LIB}"
        --ar "${CMAKE_AR}"
        --ranlib "${CMAKE_RANLIB}"
        --packages protobuf-lite sdl3
    DEPENDS declgl_base "${_DECLGL_BUNDLE_SCRIPT}"
    VERBATIM
)

add_custom_target(declgl ALL DEPENDS "${_DECLGL_ARCHIVE}")

add_custom_command(
    OUTPUT "${_DECLGL_LINK_SEXP}"
    COMMAND "${Python3_EXECUTABLE}" "${_DECLGL_LINK_SCRIPT}"
        --vcpkg-lib "${_VCPKG_LIB}"
        --build-dir "${CMAKE_BINARY_DIR}"
        --output "${_DECLGL_LINK_SEXP}"
        --os-name "${CMAKE_SYSTEM_NAME}"
    DEPENDS "${_DECLGL_LINK_SCRIPT}"
    VERBATIM
)

add_custom_target(declgl_link_flags ALL DEPENDS "${_DECLGL_LINK_SEXP}")

message(STATUS "[declgl] bundled static archive -> ${_DECLGL_ARCHIVE}")
message(STATUS "[declgl] wrote dune link flags -> ${_DECLGL_LINK_SEXP}")
