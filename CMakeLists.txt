cmake_minimum_required(VERSION 3.22)
project(compositor-linux VERSION 0.2.0 LANGUAGES C CXX)

# Releases pass the tag version in; local builds use the project version above.
set(COMPOSITOR_VERSION "${PROJECT_VERSION}" CACHE STRING "Version string shown by the app")

# compositor-linux: the Linux port of Compositor. The macOS application (Compositor.xcodeproj)
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
# x86-64-v2 (SSE4.2, every x86 CPU since 2009) makes roundf/floor/min/max single instructions in the kernels.
set(COMPOSITOR_ARCH_FLAGS "" CACHE STRING "Extra architecture flags (empty picks x86-64-v2 on x86_64 with GCC/Clang)")
if(COMPOSITOR_ARCH_FLAGS STREQUAL "" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64" AND (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"))
    set(COMPOSITOR_ARCH_FLAGS "-march=x86-64-v2")
endif()
if(NOT COMPOSITOR_ARCH_FLAGS STREQUAL "")
    separate_arguments(COMPOSITOR_ARCH_FLAG_LIST NATIVE_COMMAND "${COMPOSITOR_ARCH_FLAGS}")
    add_compile_options(${COMPOSITOR_ARCH_FLAG_LIST})
endif()

add_library(compositor_warnings INTERFACE)
target_compile_options(compositor_warnings INTERFACE
    $<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-Wall -Wextra>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers>
    $<$<AND:$<BOOL:${COMPOSITOR_WARNINGS_AS_ERRORS}>,$<COMPILE_LANG_AND_ID:C,GNU,Clang>>:-Werror>
    $<$<AND:$<BOOL:${COMPOSITOR_WARNINGS_AS_ERRORS}>,$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>>:-Werror>
)

add_subdirectory(src/pixels)
add_subdirectory(src/core)

if(COMPOSITOR_BUILD_APP)
    add_subdirectory(src/app)
endif()

if(COMPOSITOR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
