# The portable editor core: document model, compositing, brush, history and the
# .comp project format. No Qt here; the only external dependency is libpng.

find_package(PkgConfig REQUIRED)
pkg_check_modules(PNG REQUIRED IMPORTED_TARGET libpng)

add_library(compositor_core STATIC
    src/adjustments.cpp
    src/blend.cpp
    src/brush.cpp
    src/document.cpp
    src/history.cpp
    src/image.cpp
    src/png.cpp
    src/project.cpp
    src/render.cpp
    src/selection.cpp
    src/transform.cpp
    src/uuid.cpp
)
add_library(compositor::core ALIAS compositor_core)

target_include_directories(compositor_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PUBLIC ${CMAKE_SOURCE_DIR}/src/third_party
)
target_link_libraries(compositor_core
    PUBLIC compositor::pixels
    PRIVATE PkgConfig::PNG compositor_warnings
)
target_compile_definitions(compositor_core PRIVATE _USE_MATH_DEFINES)
set_target_properties(compositor_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
