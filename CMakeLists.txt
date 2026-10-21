cmake_minimum_required(VERSION 3.22)
project(nekophoto VERSION 1.8.15 LANGUAGES C CXX)

# The options were COMPOSITOR_* until 1.8.14: one given the old way (on the command line, or kept in an old build
# folder's cache) still sets its NEKOPHOTO_* namesake, unless that is set too.
get_cmake_property(nekophoto_cached CACHE_VARIABLES)
foreach(old IN LISTS nekophoto_cached)
    if(old MATCHES "^COMPOSITOR_(.+)$")
        set(now "NEKOPHOTO_${CMAKE_MATCH_1}")   # (CMAKE_MATCH_1 is only set once the MATCHES above has run)
        if(NOT DEFINED ${now})
            set(${now} "${${old}}" CACHE STRING "Set from ${old}")
        endif()
    endif()
endforeach()

# Releases pass the tag version in with -DNEKOPHOTO_VERSION; a local build shows the project version
# above. The override is a cache entry, so it defaults to empty rather than to the project version: a
# configure after a version bump then picks the new number up instead of keeping the one it first saw.
set(NEKOPHOTO_VERSION "" CACHE STRING "Version string shown by the app; empty for the project version")
if(NEKOPHOTO_VERSION STREQUAL "")
    set(NEKOPHOTO_VERSION_STRING "${PROJECT_VERSION}")
else()
    set(NEKOPHOTO_VERSION_STRING "${NEKOPHOTO_VERSION}")
endif()

# NekoPhoto, which began as the Linux port of Compositor for macOS (https://github.com/robbietilton/Compositor).

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(NEKOPHOTO_BUILD_APP "Build the Qt 6 application" ON)
option(NEKOPHOTO_BUILD_TESTS "Build the test executables" ON)
option(NEKOPHOTO_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
option(NEKOPHOTO_BUILD_FUZZERS "Build the libFuzzer targets (Clang; see docs/fuzzing.md)" OFF)
# LLVM's lld links about five times faster than GNU ld (the app: 0.07 s of CPU against 0.4 s), which adds up over the
# hundred test programs a change in the core relinks. Used on Linux when the compiler can find it; ld otherwise.
option(NEKOPHOTO_LLD "Link with lld when it is available (Linux, GCC or Clang)" ON)
if(NEKOPHOTO_LLD AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    include(CheckLinkerFlag)
    check_linker_flag(CXX "-fuse-ld=lld" NEKOPHOTO_HAVE_LLD)
    if(NEKOPHOTO_HAVE_LLD)
        add_link_options("-fuse-ld=lld")
    endif()
endif()
# x86-64-v2 (SSE4.2, every x86 CPU since 2009) makes roundf/floor/min/max single instructions in the kernels.
set(NEKOPHOTO_ARCH_FLAGS "" CACHE STRING "Extra architecture flags (empty picks x86-64-v2 on x86_64 with GCC/Clang)")
if(NEKOPHOTO_ARCH_FLAGS STREQUAL "" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64" AND (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
    set(NEKOPHOTO_ARCH_FLAGS "-march=x86-64-v2")
endif()
if(NOT NEKOPHOTO_ARCH_FLAGS STREQUAL "")
    separate_arguments(NEKOPHOTO_ARCH_FLAG_LIST NATIVE_COMMAND "${NEKOPHOTO_ARCH_FLAGS}")
    add_compile_options(${NEKOPHOTO_ARCH_FLAG_LIST})
endif()

# MinGW's <cmath> hides M_PI under strict -std=c++20 unless asked (glibc shows it anyway).
if(WIN32)
    add_compile_definitions(_USE_MATH_DEFINES)
endif()

add_library(nekophoto_warnings INTERFACE)
target_compile_options(nekophoto_warnings INTERFACE
    $<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-Wall -Wextra>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers>
    $<$<AND:$<BOOL:${NEKOPHOTO_WARNINGS_AS_ERRORS}>,$<COMPILE_LANG_AND_ID:C,GNU,Clang>>:-Werror>
    $<$<AND:$<BOOL:${NEKOPHOTO_WARNINGS_AS_ERRORS}>,$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>>:-Werror>
)

# Precompiled headers: the standard and Qt headers most files include are parsed once per target instead of once per
# file. Only system headers, never the project's own: a project header in a PCH would rebuild every file of the target
# whenever it changed. Off by default under MinGW, whose GCC can fail to map a large PCH where it was built ("had to
# relocate PCH"). Turn it off wherever a toolchain misbehaves; a build with it off also shows a file that has come to
# lean on the PCH for one of its includes.
if(MINGW)
    set(NEKOPHOTO_PCH_DEFAULT OFF)
else()
    set(NEKOPHOTO_PCH_DEFAULT ON)
endif()
option(NEKOPHOTO_PCH "Precompile the common standard and Qt headers" ${NEKOPHOTO_PCH_DEFAULT})

# nekophoto_precompile_headers(<target> <header>...): precompiles the system headers named (vector, QWidget) for the
# target's C++ files when NEKOPHOTO_PCH is on; its C files get none. A file compiled with other options than the
# target's opts out with the SKIP_PRECOMPILE_HEADERS source property.
function(nekophoto_precompile_headers target)
    if(NOT NEKOPHOTO_PCH)
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

if(NEKOPHOTO_BUILD_APP)
    add_subdirectory(src/app)
endif()

if(NEKOPHOTO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
if(NEKOPHOTO_BUILD_FUZZERS)
    add_subdirectory(tests/fuzz)
endif()
