# The portable editor core: document model, compositing, brush, history and the
# .comp project format. No Qt here; the only external dependency is libpng.

find_package(PkgConfig REQUIRED)
pkg_check_modules(PNG REQUIRED IMPORTED_TARGET libpng)

# OpenCV's DNN module runs the Remove Background model. Optional: without it the
# refinement still builds and the menu item explains what's missing.
option(COMPOSITOR_WITH_OPENCV "Use OpenCV DNN for Remove Background" ON)
set(COMPOSITOR_HAVE_OPENCV OFF)
if(COMPOSITOR_WITH_OPENCV)
    # Only the three modules the model needs, so a packaged build doesn't drag in all of OpenCV.
    find_package(OpenCV QUIET COMPONENTS core imgproc dnn)
    if(OpenCV_FOUND)
        set(COMPOSITOR_HAVE_OPENCV ON)
        set(COMPOSITOR_OPENCV_LIBS opencv_core opencv_imgproc opencv_dnn)
        message(STATUS "Remove Background: OpenCV ${OpenCV_VERSION} DNN")
    else()
        pkg_check_modules(OPENCV IMPORTED_TARGET opencv5)
        if(NOT OPENCV_FOUND)
            pkg_check_modules(OPENCV IMPORTED_TARGET opencv4)
        endif()
        if(OPENCV_FOUND)
            set(COMPOSITOR_HAVE_OPENCV ON)
            set(COMPOSITOR_OPENCV_LIBS PkgConfig::OPENCV)
            message(STATUS "Remove Background: OpenCV ${OPENCV_VERSION} DNN (via pkg-config, all modules)")
        else()
            message(STATUS "Remove Background: OpenCV not found, model inference disabled")
        endif()
    endif()
endif()
set(COMPOSITOR_HAVE_OPENCV ${COMPOSITOR_HAVE_OPENCV} PARENT_SCOPE)

add_library(compositor_core STATIC
    src/adjustments.cpp
    src/blend.cpp
    src/blur.cpp
    src/brush.cpp
    src/document.cpp
    src/filters.cpp
    src/history.cpp
    src/image.cpp
    src/kernels.cpp
    src/matte.cpp
    src/morphology.cpp
    src/parallel.cpp
    src/wand.cpp
    src/subject.cpp
    src/png.cpp
    src/project.cpp
    src/render.cpp
    src/selection.cpp
    src/shape.cpp
    src/warp.cpp
    src/warpstroke.cpp
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
if(COMPOSITOR_HAVE_OPENCV)
    target_link_libraries(compositor_core PRIVATE ${COMPOSITOR_OPENCV_LIBS})
    target_compile_definitions(compositor_core PRIVATE COMPOSITOR_HAVE_OPENCV)
endif()
set_target_properties(compositor_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
