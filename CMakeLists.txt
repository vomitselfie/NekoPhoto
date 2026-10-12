cmake_minimum_required(VERSION 3.22)
project(nekophoto VERSION 1.8.14 LANGUAGES C CXX)

# Releases pass the tag version in with -DCOMPOSITOR_VERSION; a local build shows the project version
# above. The override is a cache entry, so it defaults to empty rather than to the project version: a
# configure after a version bump then picks the new number up instead of keeping the one it first saw.
set(COMPOSITOR_VERSION "" CACHE STRING "Version string shown by the app; empty for the project version")
if(COMPOSITOR_VERSION STREQUAL "")
    set(COMPOSITOR_VERSION_STRING "${PROJECT_VERSION}")
else()
    set(COMPOSITOR_VERSION_STRING "${COMPOSITOR_VERSION}")
endif()

# NekoPhoto, which began as the Linux port of Compositor. The macOS application (Compositor.xcodeproj)
# does not use this file; it keeps building from Xcode. See docs/linux-port-architecture.md.

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(COMPOSITOR_BUILD_APP "Build the Qt 6 application" ON)
option(COMPOSITOR_BUILD_TESTS "Build the test executables" ON)
option(COMPOSITOR_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
option(COMPOSITOR_BUILD_FUZZERS "Build the libFuzzer targets (Clang; see docs/fuzzing.md)" OFF)
# x86-64-v2 (SSE4.2, every x86 CPU since 2009) makes roundf/floor/min/max single instructions in the kernels.
set(COMPOSITOR_ARCH_FLAGS "" CACHE STRING "Extra architecture flags (empty picks x86-64-v2 on x86_64 with GCC/Clang)")
if(COMPOSITOR_ARCH_FLAGS STREQUAL "" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64" AND (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
    set(COMPOSITOR_ARCH_FLAGS "-march=x86-64-v2")
endif()
if(NOT COMPOSITOR_ARCH_FLAGS STREQUAL "")
    separate_arguments(COMPOSITOR_ARCH_FLAG_LIST NATIVE_COMMAND "${COMPOSITOR_ARCH_FLAGS}")
    add_compile_options(${COMPOSITOR_ARCH_FLAG_LIST})
endif()

# MinGW's <cmath> hides M_PI under strict -std=c++20 unless asked (glibc shows it anyway).
if(WIN32)
    add_compile_definitions(_USE_MATH_DEFINES)
endif()

add_library(compositor_warnings INTERFACE)
target_compile_options(compositor_warnings INTERFACE
    $<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-Wall -Wextra>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers>
    $<$<AND:$<BOOL:${COMPOSITOR_WARNINGS_AS_ERRORS}>,$<COMPILE_LANG_AND_ID:C,GNU,Clang>>:-Werror>
    $<$<AND:$<BOOL:${COMPOSITOR_WARNINGS_AS_ERRORS}>,$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>>:-Werror>
)

# Precompiled headers: the standard and Qt headers most files include are parsed once per target instead of once per
# file. Only system headers, never the project's own: a project header in a PCH would rebuild every file of the target
# whenever it changed. Off by default under MinGW, whose GCC can fail to map a large PCH where it was built ("had to
# relocate PCH"). Turn it off wherever a toolchain misbehaves; a build with it off also shows a file that has come to
# lean on the PCH for one of its includes.
if(MINGW)
    set(COMPOSITOR_PCH_DEFAULT OFF)
else()
    set(COMPOSITOR_PCH_DEFAULT ON)
endif()
option(COMPOSITOR_PCH "Precompile the common standard and Qt headers" ${COMPOSITOR_PCH_DEFAULT})

# compositor_precompile_headers(<target> <header>...): precompiles the system headers named (vector, QWidget) for the
# target's C++ files when COMPOSITOR_PCH is on; its C files get none. A file compiled with other options than the
# target's opts out with the SKIP_PRECOMPILE_HEADERS source property.
function(compositor_precompile_headers target)
    if(NOT COMPOSITOR_PCH)
        return()
    endif()
    set(headers "")
    foreach(header IN LISTS ARGN)
        list(APPEND headers "$<$<COMPILE_LANGUAGE:CXX>:<${header}$<ANGLE-R>>")
    endforeach()
    target_precompile_headers(${target} PRIVATE ${headers})
endfunction()

add_subdirectory(src/pixels)
add_subdirectory(src/core)

if(COMPOSITOR_BUILD_APP)
    add_subdirectory(src/app)
endif()

if(COMPOSITOR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
if(COMPOSITOR_BUILD_FUZZERS)
    add_subdirectory(tests/fuzz)
endif()
