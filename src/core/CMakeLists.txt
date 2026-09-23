# The portable editor core: document model, compositing, brush, history and the
# .comp project format. No Qt here; the only external dependency is libpng.

find_package(PkgConfig REQUIRED)
pkg_check_modules(PNG REQUIRED IMPORTED_TARGET libpng)
pkg_check_modules(ZLIB REQUIRED IMPORTED_TARGET zlib)

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
        message(STATUS "Remove Background: OpenCV ${OpenCV_VERSION} DNN from ${OpenCV_DIR}")
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

# libmypaint paints the MyPaint brush presets. Optional: without it the round brush is the only engine.
option(COMPOSITOR_WITH_MYPAINT "Use libmypaint for the MyPaint brush presets" ON)
set(COMPOSITOR_HAVE_MYPAINT OFF)
if(COMPOSITOR_WITH_MYPAINT)
    pkg_check_modules(MYPAINT IMPORTED_TARGET libmypaint>=1.5)
    if(MYPAINT_FOUND)
        # Ubuntu's libmypaint.pc puts -fopenmp in Libs. libmypaint.so already links its own OpenMP runtime;
        # the flag only makes our link ask for the compiler's (libomp under Clang, often not installed).
        foreach(property INTERFACE_LINK_OPTIONS INTERFACE_LINK_LIBRARIES INTERFACE_COMPILE_OPTIONS)
            get_target_property(values PkgConfig::MYPAINT ${property})
            if(values)
                list(FILTER values EXCLUDE REGEX "^-f?openmp$|^-fopenmp")
                set_target_properties(PkgConfig::MYPAINT PROPERTIES ${property} "${values}")
            endif()
        endforeach()
        set(COMPOSITOR_HAVE_MYPAINT ON)
        message(STATUS "Brushes: libmypaint ${MYPAINT_VERSION}")
    else()
        message(STATUS "Brushes: libmypaint not found, MyPaint presets disabled")
    endif()
endif()
set(COMPOSITOR_HAVE_MYPAINT ${COMPOSITOR_HAVE_MYPAINT} PARENT_SCOPE)

# SQLite reads Clip Studio brushes (.sut). Optional: without it that one import format is refused.
option(COMPOSITOR_WITH_SQLITE "Import Clip Studio brushes through SQLite" ON)
set(COMPOSITOR_HAVE_SQLITE OFF)
if(COMPOSITOR_WITH_SQLITE)
    find_package(SQLite3 QUIET)
    if(SQLite3_FOUND)
        set(COMPOSITOR_HAVE_SQLITE ON)
        # CMake 4 names the target SQLite3::SQLite3 and deprecates SQLite::SQLite3, the only name CMake 3.22 has.
        if(TARGET SQLite3::SQLite3)
            set(COMPOSITOR_SQLITE_TARGET SQLite3::SQLite3)
        else()
            set(COMPOSITOR_SQLITE_TARGET SQLite::SQLite3)
        endif()
        message(STATUS "Brush import: Clip Studio through SQLite ${SQLite3_VERSION}")
    else()
        message(STATUS "Brush import: SQLite not found, Clip Studio brushes disabled")
    endif()
endif()
set(COMPOSITOR_HAVE_SQLITE ${COMPOSITOR_HAVE_SQLITE} PARENT_SCOPE)

add_library(compositor_core STATIC
    src/abr.cpp
    src/adjustments.cpp
    src/blend.cpp
    src/blur.cpp
    src/brushimport.cpp
    src/brush.cpp
    src/document.cpp
    src/filters.cpp
    src/history.cpp
    src/image.cpp
    src/kernels.cpp
    src/matte.cpp
    src/morphology.cpp
    src/mypaint.cpp
    src/tipbrush.cpp
    src/parallel.cpp
    src/wand.cpp
    src/zip.cpp
    src/resample.cpp
    src/heal.cpp
    src/inpaint.cpp
    src/psd.cpp
    src/subject.cpp
    src/sut.cpp
    src/scribble.cpp
    src/photoshop.cpp
    src/plist.cpp
    src/png.cpp
    src/procreate.cpp
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
    PRIVATE PkgConfig::PNG PkgConfig::ZLIB compositor_warnings
)
target_compile_definitions(compositor_core PRIVATE _USE_MATH_DEFINES)
if(COMPOSITOR_HAVE_OPENCV)
    target_link_libraries(compositor_core PRIVATE ${COMPOSITOR_OPENCV_LIBS})
    target_compile_definitions(compositor_core PRIVATE COMPOSITOR_HAVE_OPENCV)
endif()
if(COMPOSITOR_HAVE_SQLITE)
    target_link_libraries(compositor_core PRIVATE ${COMPOSITOR_SQLITE_TARGET})
    target_compile_definitions(compositor_core PRIVATE COMPOSITOR_HAVE_SQLITE)
endif()
if(COMPOSITOR_HAVE_MYPAINT)
    target_link_libraries(compositor_core PRIVATE PkgConfig::MYPAINT)
    target_compile_definitions(compositor_core PRIVATE COMPOSITOR_HAVE_MYPAINT)
endif()
set_target_properties(compositor_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
