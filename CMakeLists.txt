cmake_minimum_required(VERSION 3.22)
project(Compositor VERSION 1.0.4 LANGUAGES C CXX)

# The Linux port. The macOS application (Compositor.xcodeproj) does not use this file;
# it keeps building from Xcode. See docs/linux-port-architecture.md.

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

add_library(compositor_warnings INTERFACE)
target_compile_options(compositor_warnings INTERFACE
    $<$<COMPILE_LANG_AND_ID:C,GNU,Clang>:-Wall -Wextra>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wall -Wextra -Wno-unused-parameter>
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
