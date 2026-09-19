# The existing portable C pixel routines, compiled unchanged from where the
# Xcode project keeps them. Nothing here depends on Apple frameworks.

set(COMPOSITOR_PIXELS_DIR ${CMAKE_SOURCE_DIR}/Compositor/Rendering)

add_library(compositor_pixels STATIC
    ${COMPOSITOR_PIXELS_DIR}/AdjustPixels.c
    ${COMPOSITOR_PIXELS_DIR}/BrushPixels.c
    ${COMPOSITOR_PIXELS_DIR}/ContentFill.c
    ${COMPOSITOR_PIXELS_DIR}/HealPixels.c
    ${COMPOSITOR_PIXELS_DIR}/LensPixels.c
    ${COMPOSITOR_PIXELS_DIR}/LevelsPixels.c
    ${COMPOSITOR_PIXELS_DIR}/NoisePixels.c
    ${COMPOSITOR_PIXELS_DIR}/WandPixels.c
)
add_library(compositor::pixels ALIAS compositor_pixels)

target_include_directories(compositor_pixels PUBLIC ${COMPOSITOR_PIXELS_DIR})
# M_PI is a POSIX extension, hidden under strict -std=c11.
target_compile_definitions(compositor_pixels PRIVATE _DEFAULT_SOURCE)
target_link_libraries(compositor_pixels PRIVATE compositor_warnings m)
set_target_properties(compositor_pixels PROPERTIES POSITION_INDEPENDENT_CODE ON)
