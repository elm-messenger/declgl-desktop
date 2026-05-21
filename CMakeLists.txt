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
add_subdirectory(src/gpu)
add_subdirectory(src/resources)
add_subdirectory(src/renderer)
add_subdirectory(src/engine)
add_subdirectory(src/caml_bridge)

# ----------------------------------------------------------------------------
# libdeclgl.a — a single merged static archive that the OCaml dune side
# picks up via (foreign_archives declgl). It bundles every first-party
# object file the OCaml-linked binary needs at link time:
#   - declgl_caml_bridge  (extern "C" CAMLprim entry points)
#   - declgl_engine       (SDL/GL + per-command dispatch)
#   - declgl_proto        (generated protobuf C++ stubs)
#   - glad                (GL loader)
#
# The third-party archives we depend on (SDL3, protobuf, abseil, ...) stay
# as separate -l flags fed to dune via c_library_flags — bundling those
# would balloon the archive and is brittle across vcpkg layouts.
#
# On macOS we use libtool -static (handles fat archives + dedupes object
# names); on other Unixes we use `ar -M` with a MRI script.
# ----------------------------------------------------------------------------
set(_DECLGL_BUNDLE_INPUTS
    $<TARGET_FILE:declgl_caml_bridge>
    $<TARGET_FILE:declgl_engine>
    $<TARGET_FILE:declgl_renderer>
    $<TARGET_FILE:declgl_gpu>
    $<TARGET_FILE:declgl_resources>
    $<TARGET_FILE:declgl_shaders>
    $<TARGET_FILE:declgl_proto>
    $<TARGET_FILE:glad>
    $<TARGET_FILE:stb_image>
)
set(_DECLGL_BUNDLE_OUT "${CMAKE_BINARY_DIR}/libdeclgl.a")

if(APPLE)
    add_custom_command(
        OUTPUT  ${_DECLGL_BUNDLE_OUT}
        COMMAND libtool -static -o ${_DECLGL_BUNDLE_OUT} ${_DECLGL_BUNDLE_INPUTS}
        DEPENDS declgl_caml_bridge declgl_engine declgl_renderer declgl_gpu declgl_resources declgl_shaders declgl_proto glad stb_image
        COMMENT "Bundling libdeclgl.a"
        VERBATIM
    )
else()
    # GNU ar MRI script approach. Generated at build time.
    set(_DECLGL_MRI_SCRIPT "${CMAKE_BINARY_DIR}/declgl_bundle.mri")
    add_custom_command(
        OUTPUT  ${_DECLGL_BUNDLE_OUT}
        COMMAND ${CMAKE_COMMAND} -E echo "create ${_DECLGL_BUNDLE_OUT}"   > ${_DECLGL_MRI_SCRIPT}
        COMMAND ${CMAKE_COMMAND} -E echo "addlib $<TARGET_FILE:declgl_caml_bridge>" >> ${_DECLGL_MRI_SCRIPT}
        COMMAND ${CMAKE_COMMAND} -E echo "addlib $<TARGET_FILE:declgl_engine>"      >> ${_DECLGL_MRI_SCRIPT}
        COMMAND ${CMAKE_COMMAND} -E echo "addlib $<TARGET_FILE:declgl_renderer>"    >> ${_DECLGL_MRI_SCRIPT}
        COMMAND ${CMAKE_COMMAND} -E echo "addlib $<TARGET_FILE:declgl_gpu>"         >> ${_DECLGL_MRI_SCRIPT}
        COMMAND ${CMAKE_COMMAND} -E echo "addlib $<TARGET_FILE:declgl_resources>"   >> ${_DECLGL_MRI_SCRIPT}
        COMMAND ${CMAKE_COMMAND} -E echo "addlib $<TARGET_FILE:declgl_shaders>"     >> ${_DECLGL_MRI_SCRIPT}
        COMMAND ${CMAKE_COMMAND} -E echo "addlib $<TARGET_FILE:declgl_proto>"       >> ${_DECLGL_MRI_SCRIPT}
        COMMAND ${CMAKE_COMMAND} -E echo "addlib $<TARGET_FILE:glad>"               >> ${_DECLGL_MRI_SCRIPT}
        COMMAND ${CMAKE_COMMAND} -E echo "addlib $<TARGET_FILE:stb_image>"          >> ${_DECLGL_MRI_SCRIPT}
        COMMAND ${CMAKE_COMMAND} -E echo "save"                                     >> ${_DECLGL_MRI_SCRIPT}
        COMMAND ${CMAKE_COMMAND} -E echo "end"                                      >> ${_DECLGL_MRI_SCRIPT}
        COMMAND ${CMAKE_AR} -M < ${_DECLGL_MRI_SCRIPT}
        DEPENDS declgl_caml_bridge declgl_engine declgl_renderer declgl_gpu declgl_resources declgl_shaders declgl_proto glad stb_image
        COMMENT "Bundling libdeclgl.a"
        VERBATIM
    )
endif()

add_custom_target(declgl ALL DEPENDS ${_DECLGL_BUNDLE_OUT})

# ----------------------------------------------------------------------------
# Dune integration: emit two artefacts that the OCaml side ingests via
# (foreign_archives ...) + (c_library_flags (:include ...)).
#
#   1. libdeclgl.a (already produced above) — bundled first-party code.
#   2. declgl_link_flags.sexp — third-party link flags as an S-expression
#      that dune can splice into c_library_flags. We list each absl
#      sub-archive vcpkg installs explicitly because (a) abseil's CMake
#      target graph is unstable across versions and (b) dune's foreign-
#      stub linker doesn't follow imported-target transitive deps on
#      its own.
#
# The .sexp lives next to libdeclgl.a so the OCaml side can point at one
# directory and find both. We always re-generate at configure time —
# the contents are static once vcpkg has resolved.
# ----------------------------------------------------------------------------
set(_DECLGL_LINK_SEXP "${CMAKE_BINARY_DIR}/declgl_link_flags.sexp")
set(_VCPKG_LIB        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib")

# Frameworks needed on macOS for SDL3 + GL (SDL3's static archive does
# not pull these in; the user's exe linker resolves them).
if(APPLE)
    set(_DECLGL_FRAMEWORKS
        "-framework Cocoa"
        "-framework IOKit"
        "-framework CoreVideo"
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework CoreHaptics"
        "-framework GameController"
        "-framework Metal"
        "-framework QuartzCore"
        "-framework ForceFeedback"
        "-framework Carbon"
        "-framework UniformTypeIdentifiers"
        "-framework OpenGL"
        # SDL3 also pulls in camera + media subsystems on macOS:
        "-framework AVFoundation"
        "-framework CoreMedia"
        "-framework CoreFoundation"
        "-framework CoreGraphics"
        "-framework CoreImage"
        "-framework CoreText"
        "-framework CoreServices"
        "-framework CoreBluetooth"
        "-framework Foundation"
    )
    set(_DECLGL_LINK_GROUP_START "")
    set(_DECLGL_LINK_GROUP_END "")
else()
    set(_DECLGL_FRAMEWORKS "")
    set(_DECLGL_LINK_GROUP_START "-Wl,--start-group")
    set(_DECLGL_LINK_GROUP_END   "-Wl,--end-group")
endif()

# Build the absl sub-archive list by globbing what vcpkg installed. We
# list them all because protobuf transitively depends on most of absl,
# and trimming by hand is fragile.
file(GLOB _ABSL_LIBS RELATIVE "${_VCPKG_LIB}" "${_VCPKG_LIB}/libabsl_*.a")
set(_ABSL_LFLAGS "")
foreach(_a IN LISTS _ABSL_LIBS)
    string(REGEX REPLACE "^lib(.*)\\.a$" "\\1" _name "${_a}")
    list(APPEND _ABSL_LFLAGS "-l${_name}")
endforeach()

# Compose the full sexp. -L paths come first so the -l names below
# resolve. We also link C++ stdlib explicitly because dune's foreign
# archive linker uses the C compiler driver by default.
set(_SEXP_LINES "(")
list(APPEND _SEXP_LINES "  -L${_VCPKG_LIB}")
list(APPEND _SEXP_LINES "  -L${CMAKE_BINARY_DIR}")
list(APPEND _SEXP_LINES "  -lprotobuf-lite")
list(APPEND _SEXP_LINES "  -lSDL3")
list(APPEND _SEXP_LINES "  -lutf8_range")
list(APPEND _SEXP_LINES "  -lutf8_validity")
if(_DECLGL_LINK_GROUP_START)
    list(APPEND _SEXP_LINES "  ${_DECLGL_LINK_GROUP_START}")
endif()
foreach(_f IN LISTS _ABSL_LFLAGS)
    list(APPEND _SEXP_LINES "  ${_f}")
endforeach()
if(_DECLGL_LINK_GROUP_END)
    list(APPEND _SEXP_LINES "  ${_DECLGL_LINK_GROUP_END}")
endif()
foreach(_f IN LISTS _DECLGL_FRAMEWORKS)
    list(APPEND _SEXP_LINES "  ${_f}")
endforeach()
list(APPEND _SEXP_LINES "  -lstdc++")
list(APPEND _SEXP_LINES ")")

string(REPLACE ";" "\n" _SEXP_TEXT "${_SEXP_LINES}")
file(WRITE "${_DECLGL_LINK_SEXP}" "${_SEXP_TEXT}\n")

message(STATUS "[declgl] wrote dune link flags -> ${_DECLGL_LINK_SEXP}")
