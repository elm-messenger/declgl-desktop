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

# Hide symbols by default; we explicitly export the C ABI later.
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

# ----------------------------------------------------------------------------
# Dependencies (vcpkg manifest mode -> these are auto-installed on configure)
# ----------------------------------------------------------------------------
find_package(SDL3      CONFIG REQUIRED)
find_package(glm       CONFIG REQUIRED)
# Protobuf is pulled in M2; we fetch it now anyway so the configure step
# verifies the manifest end-to-end.
find_package(Protobuf  CONFIG REQUIRED)

# ----------------------------------------------------------------------------
# Third-party (vendored single-file libs)
# ----------------------------------------------------------------------------
add_subdirectory(third_party/glad)

# ----------------------------------------------------------------------------
# Subdirectories
# ----------------------------------------------------------------------------
add_subdirectory(examples/demo)
