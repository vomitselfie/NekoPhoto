# The portable C pixel routines from upstream Compositor's Compositor/Rendering (MIT, LICENSES/MIT-Compositor.txt),
# kept as they were. Nothing here depends on Apple frameworks.

add_library(compositor_pixels STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/AdjustPixels.c
    ${CMAKE_CURRENT_SOURCE_DIR}/BrushPixels.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ContentFill.c
    ${CMAKE_CURRENT_SOURCE_DIR}/HealPixels.c
    ${CMAKE_CURRENT_SOURCE_DIR}/LensPixels.c
    ${CMAKE_CURRENT_SOURCE_DIR}/LevelsPixels.c
    ${CMAKE_CURRENT_SOURCE_DIR}/NoisePixels.c
    ${CMAKE_CURRENT_SOURCE_DIR}/WandPixels.c
    # The Camera Raw kernels, extracted from upstream's newer AdjustPixels.c (the one above predates them).
    ${CMAKE_CURRENT_SOURCE_DIR}/CameraRawPixels.c
    ${CMAKE_CURRENT_SOURCE_DIR}/CameraRawPixelsFloat.c
)
add_library(compositor::pixels ALIAS compositor_pixels)

target_include_directories(compositor_pixels PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# M_PI is a POSIX extension, hidden under strict -std=c11.
target_compile_definitions(compositor_pixels PRIVATE _DEFAULT_SOURCE)
target_link_libraries(compositor_pixels PRIVATE compositor_warnings m)
set_target_properties(compositor_pixels PROPERTIES POSITION_INDEPENDENT_CODE ON)
