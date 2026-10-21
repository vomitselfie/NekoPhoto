# The portable editor core: document model, compositing, brush, history and the
# .comp project format. No Qt here; the only external dependency is libpng.

find_package(PkgConfig REQUIRED)
pkg_check_modules(PNG REQUIRED IMPORTED_TARGET libpng)
pkg_check_modules(ZLIB REQUIRED IMPORTED_TARGET zlib)

# OpenCV's DNN module runs the Remove Background model. Optional: without it the
# refinement still builds and the menu item explains what's missing.
option(NEKOPHOTO_WITH_OPENCV "Use OpenCV DNN for Remove Background" ON)
set(NEKOPHOTO_HAVE_OPENCV OFF)
if(NEKOPHOTO_WITH_OPENCV)
    # Only the three modules the model needs, so a packaged build doesn't drag in all of OpenCV.
    find_package(OpenCV QUIET COMPONENTS core imgproc dnn)
    if(OpenCV_FOUND)
        set(NEKOPHOTO_HAVE_OPENCV ON)
        set(NEKOPHOTO_OPENCV_LIBS opencv_core opencv_imgproc opencv_dnn)
        message(STATUS "Remove Background: OpenCV ${OpenCV_VERSION} DNN from ${OpenCV_DIR}")
    else()
        pkg_check_modules(OPENCV IMPORTED_TARGET opencv5)
        if(NOT OPENCV_FOUND)
            pkg_check_modules(OPENCV IMPORTED_TARGET opencv4)
        endif()
        if(OPENCV_FOUND)
            set(NEKOPHOTO_HAVE_OPENCV ON)
            set(NEKOPHOTO_OPENCV_LIBS PkgConfig::OPENCV)
            message(STATUS "Remove Background: OpenCV ${OPENCV_VERSION} DNN (via pkg-config, all modules)")
        else()
            message(STATUS "Remove Background: OpenCV not found, model inference disabled")
        endif()
    endif()
endif()
set(NEKOPHOTO_HAVE_OPENCV ${NEKOPHOTO_HAVE_OPENCV} PARENT_SCOPE)

# libmypaint paints the MyPaint brush presets. Optional: without it the round brush is the only engine.
option(NEKOPHOTO_WITH_MYPAINT "Use libmypaint for the MyPaint brush presets" ON)
set(NEKOPHOTO_HAVE_MYPAINT OFF)
if(NEKOPHOTO_WITH_MYPAINT)
    pkg_check_modules(MYPAINT IMPORTED_TARGET libmypaint>=1.5)
    if(MYPAINT_FOUND)
        # Ubuntu's libmypaint.pc puts -fopenmp in Libs. libmypaint.so already links its own OpenMP runtime;
        # the flag only makes our link ask for the compiler's (libomp under Clang, often not installed).
        foreach(property INTERFACE_LINK_OPTIONS INTERFACE_LINK_LIBRARIES INTERFACE_COMPILE_OPTIONS)
            get_target_property(values PkgConfig::MYPAINT ${property})
            if(values)
                # Homebrew's clang spells it "-Xpreprocessor -fopenmp": both go, or the lone -Xpreprocessor swallows
                # whatever follows it on the link line (the app's moc object, on macOS).
                list(FILTER values EXCLUDE REGEX "^-f?openmp$|^-fopenmp|^-Xpreprocessor$")
                set_target_properties(PkgConfig::MYPAINT PROPERTIES ${property} "${values}")
            endif()
        endforeach()
        set(NEKOPHOTO_HAVE_MYPAINT ON)
        message(STATUS "Brushes: libmypaint ${MYPAINT_VERSION}")
    else()
        message(STATUS "Brushes: libmypaint not found, MyPaint presets disabled")
    endif()
endif()
set(NEKOPHOTO_HAVE_MYPAINT ${NEKOPHOTO_HAVE_MYPAINT} PARENT_SCOPE)

# LibRaw (LGPL 2.1 / CDDL 1.0) develops camera RAW files. Optional: without it RAW files are refused.
option(NEKOPHOTO_WITH_LIBRAW "Open camera RAW files through LibRaw" ON)
set(NEKOPHOTO_HAVE_LIBRAW OFF)
if(NEKOPHOTO_WITH_LIBRAW)
    pkg_check_modules(LIBRAW IMPORTED_TARGET libraw)
    if(LIBRAW_FOUND)
        # Ubuntu's libraw.pc asks for -fopenmp as libmypaint's does; libraw.so links its own runtime (see above).
        foreach(property INTERFACE_LINK_OPTIONS INTERFACE_LINK_LIBRARIES INTERFACE_COMPILE_OPTIONS)
            get_target_property(values PkgConfig::LIBRAW ${property})
            if(values)
                # Homebrew's clang spells it "-Xpreprocessor -fopenmp": both go, or the lone -Xpreprocessor swallows
                # whatever follows it on the link line (the app's moc object, on macOS).
                list(FILTER values EXCLUDE REGEX "^-f?openmp$|^-fopenmp|^-Xpreprocessor$")
                set_target_properties(PkgConfig::LIBRAW PROPERTIES ${property} "${values}")
            endif()
        endforeach()
        set(NEKOPHOTO_HAVE_LIBRAW ON)
        message(STATUS "RAW: LibRaw ${LIBRAW_VERSION}")
    else()
        message(STATUS "RAW: LibRaw not found, camera RAW files disabled")
    endif()
endif()

# SQLite reads Clip Studio brushes (.sut) and projects (.clip). Optional: without it those imports are refused.
option(NEKOPHOTO_WITH_SQLITE "Import Clip Studio brushes through SQLite" ON)
set(NEKOPHOTO_HAVE_SQLITE OFF)
if(NEKOPHOTO_WITH_SQLITE)
    find_package(SQLite3 QUIET)
    if(SQLite3_FOUND)
        set(NEKOPHOTO_HAVE_SQLITE ON)
        # CMake 4 names the target SQLite3::SQLite3 and deprecates SQLite::SQLite3, the only name CMake 3.22 has.
        if(TARGET SQLite3::SQLite3)
            set(NEKOPHOTO_SQLITE_TARGET SQLite3::SQLite3)
        else()
            set(NEKOPHOTO_SQLITE_TARGET SQLite::SQLite3)
        endif()
        message(STATUS "Brush import: Clip Studio through SQLite ${SQLite3_VERSION}")
    else()
        message(STATUS "Brush import: SQLite not found, Clip Studio brushes disabled")
    endif()
endif()
set(NEKOPHOTO_HAVE_SQLITE ${NEKOPHOTO_HAVE_SQLITE} PARENT_SCOPE)

# zstd decompresses the streams of Affinity documents (most from Affinity 2 on). Optional: without it such
# documents are refused with a message; zlib-compressed ones still open.
option(NEKOPHOTO_WITH_ZSTD "Open zstd-compressed Affinity documents through libzstd" ON)
set(NEKOPHOTO_HAVE_ZSTD OFF)
if(NEKOPHOTO_WITH_ZSTD)
    pkg_check_modules(ZSTD IMPORTED_TARGET libzstd)
    if(ZSTD_FOUND)
        set(NEKOPHOTO_HAVE_ZSTD ON)
        message(STATUS "Affinity: zstd ${ZSTD_VERSION}")
    else()
        message(STATUS "Affinity: libzstd not found, zstd-compressed Affinity documents disabled")
    endif()
endif()

add_library(nekophoto_core STATIC
    src/abr.cpp
    src/artboard.cpp
    src/guides.cpp
    src/animation.cpp
    src/aseprite.cpp
    src/gif.cpp
    src/gif_write.cpp
    src/ico.cpp
    src/tga.cpp
    src/affinity.cpp
    src/affinity_tree.cpp
    src/adjustments.cpp
    src/adjustments_modes.cpp
    src/histogram.cpp
    src/adjustments_more.cpp
    src/adjustments_u16.cpp
    src/colorlookup.cpp
    src/clip.cpp
    src/blend_u8.cpp
    src/blur.cpp
    src/blur_u16.cpp
    src/brushimport.cpp
    src/brush.cpp
    src/brush_u16.cpp
    src/brush_f32.cpp
    src/brush_modes.cpp
    src/pixels_any.cpp
    src/transform_modes.cpp
    src/brushdynamics.cpp
    src/brushsample.cpp
    src/brushsmoothing.cpp
    src/cameraraw.cpp
    src/document.cpp
    src/filters.cpp
    src/filters_modes.cpp
    src/filters_grid.cpp
    src/filters_photocraft.cpp
    src/filters_u16.cpp
    src/history.cpp
    src/image.cpp
    src/kernels.cpp
    src/matte.cpp
    src/morphology.cpp
    src/mosh.cpp
    src/mosh_effects.cpp
    src/mypaint.cpp
    src/tipbrush.cpp
    src/parallel.cpp
    src/workcounters.cpp
    src/wand.cpp
    src/smartwand.cpp
    src/zip.cpp
    src/zipfile.cpp
    src/resample.cpp
    src/resample_u16.cpp
    src/heal.cpp
    src/inpaint.cpp
    src/contentmove.cpp
    src/seamcarve.cpp
    src/psd.cpp
    src/blendif.cpp
    src/layerstyle.cpp
    src/layerstyle_render.cpp
    src/layerstyle_write.cpp
    src/layerstyle_json.cpp
    src/toning.cpp
    src/bucket_modes.cpp
    src/raw.cpp
    src/rawtone.cpp
    src/rawmeasured.cpp
    src/camerarawxmp.cpp
    src/whitebalance.cpp
    src/vectorlayer.cpp
    src/supports.cpp
    src/svg.cpp
    src/svg_write.cpp
    src/svg_xml.cpp
    src/colour.cpp
    src/colormgmt.cpp
    src/colormgmt_display.cpp
    src/colormgmt_document.cpp
    src/psd_carry.cpp
    src/smartobject.cpp
    src/smartobject_edit.cpp
    src/smartfilter.cpp
    src/vectormask.cpp
    src/psd_text.cpp
    src/psd_writer.cpp
    src/subject.cpp
    src/sut.cpp
    src/scribble.cpp
    src/photoshop.cpp
    src/plist.cpp
    src/png.cpp
    src/presets.cpp
    src/procreate.cpp
    src/project.cpp
    src/render.cpp
    src/render_plan.cpp
    src/render_exec_u8.cpp
    src/selection.cpp
    src/channels.cpp
    src/psd_channels.cpp
    src/shape.cpp
    src/warp.cpp
    src/warpmesh.cpp
    src/trim.cpp
    src/smartfilter_render.cpp
    src/smartfilter_render16.cpp
    src/warpstroke.cpp
    src/transform.cpp
    src/uuid.cpp
    # 16 bits per channel (P2), last so the 8-bit code keeps its place in the library
    src/depth.cpp
    src/blend_u16.cpp
    src/render_exec_u16.cpp
    src/render_u16.cpp
    src/depth_f32.cpp
    src/blend_f32.cpp
    src/render_exec_f32.cpp
    src/render_f32.cpp
    src/blend_c8.cpp
    src/blend_modes16.cpp
    src/render_modes.cpp
    src/render_exec_c8.cpp
    src/render_exec_lab8.cpp
    src/render_exec_modes16.cpp
    src/colormode_convert.cpp
    # 32 bits per channel, editing (P5b)
    src/adjustments_f32.cpp
    src/blur_f32.cpp
    src/filters_f32.cpp
    src/resample_f32.cpp
)
add_library(nekophoto::core ALIAS nekophoto_core)
# The version a .nekophoto file names as its writer (nekophoto.json); on the one file, so a version bump rebuilds little.
set_source_files_properties(src/project.cpp PROPERTIES COMPILE_DEFINITIONS NEKOPHOTO_WRITER_VERSION="${NEKOPHOTO_VERSION_STRING}")

# The default Working CMYK profile, ISO Coated v2 300% (basICColor, zlib licence; src/third_party/icc/README.md),
# compiled in as bytes so the core has a CMYK profile without files beside the program.
set(NEKOPHOTO_CMYK_PROFILE ${CMAKE_SOURCE_DIR}/src/third_party/icc/ISOcoated_v2_300_bas.ICC)
set(NEKOPHOTO_CMYK_BLOB ${CMAKE_CURRENT_BINARY_DIR}/generated/cmyk_profile_blob.cpp)
add_custom_command(
    OUTPUT ${NEKOPHOTO_CMYK_BLOB}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${NEKOPHOTO_CMYK_PROFILE} -DOUTPUT=${NEKOPHOTO_CMYK_BLOB} -DSYMBOL=defaultCmykProfile
            -P ${CMAKE_CURRENT_SOURCE_DIR}/embed_blob.cmake
    DEPENDS ${NEKOPHOTO_CMYK_PROFILE} ${CMAKE_CURRENT_SOURCE_DIR}/embed_blob.cmake
    COMMENT "Embedding the default CMYK profile"
    VERBATIM)
target_sources(nekophoto_core PRIVATE ${NEKOPHOTO_CMYK_BLOB})

# RAWmakase's measured Camera Raw responses (MIT; src/third_party/rawmakase/README.md), compiled in the same way.
foreach(table IN ITEMS ColorMixerChart:color_mixer_chart VibranceChart:vibrance_chart
                       Parametric:parametric ColorGradeCurves:color_grade_curves)
    string(REPLACE ":" ";" parts ${table})
    list(GET parts 0 symbol)
    list(GET parts 1 file)
    set(input ${CMAKE_SOURCE_DIR}/src/third_party/rawmakase/${file}.bin)
    set(output ${CMAKE_CURRENT_BINARY_DIR}/generated/rawmakase_${file}_blob.cpp)
    add_custom_command(
        OUTPUT ${output}
        COMMAND ${CMAKE_COMMAND} -DINPUT=${input} -DOUTPUT=${output} -DSYMBOL=rawmakase${symbol} -P ${CMAKE_CURRENT_SOURCE_DIR}/embed_blob.cmake
        DEPENDS ${input} ${CMAKE_CURRENT_SOURCE_DIR}/embed_blob.cmake
        COMMENT "Embedding RAWmakase's ${file} table"
        VERBATIM)
    target_sources(nekophoto_core PRIVATE ${output})
endforeach()

target_include_directories(nekophoto_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PUBLIC ${CMAKE_SOURCE_DIR}/src/third_party
)
# The vendored Little CMS comes first on the link line: LibRaw's pkg-config file asks for the system's -llcms2, and
# a shared library named before the archive would answer every cms* call with whatever version the system has.
target_link_libraries(nekophoto_core
    PUBLIC nekophoto::pixels
    PRIVATE nekophoto_lcms2 PkgConfig::PNG PkgConfig::ZLIB nekophoto_warnings
)
target_compile_definitions(nekophoto_core PRIVATE _USE_MATH_DEFINES)
# The standard headers most of the core includes (NEKOPHOTO_PCH). Not nlohmann/json.hpp: nine files include it, and
# a PCH holding it costs the other hundred and twenty more to load than it saves those nine.
nekophoto_precompile_headers(nekophoto_core algorithm array cmath cstdint cstdio cstring functional map memory mutex optional set string vector)
# The Mosh effects hash float bit patterns: no fused multiply-adds, so every CPU computes the same bits. A PCH built
# without that option does not fit them, so they parse their headers themselves.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(src/mosh.cpp src/mosh_effects.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off" SKIP_PRECOMPILE_HEADERS ON)
endif()
if(NEKOPHOTO_HAVE_OPENCV)
    target_link_libraries(nekophoto_core PRIVATE ${NEKOPHOTO_OPENCV_LIBS})
    target_compile_definitions(nekophoto_core PRIVATE NEKOPHOTO_HAVE_OPENCV)
endif()
if(NEKOPHOTO_HAVE_SQLITE)
    # Clip Studio projects (.clip): a C reader of the container, its SQLite database and the layer tiles.
    target_sources(nekophoto_core PRIVATE src/csp_clip_import.c)
    target_link_libraries(nekophoto_core PRIVATE ${NEKOPHOTO_SQLITE_TARGET})
    target_compile_definitions(nekophoto_core PRIVATE NEKOPHOTO_HAVE_SQLITE)
endif()
if(NEKOPHOTO_HAVE_ZSTD)
    target_link_libraries(nekophoto_core PRIVATE PkgConfig::ZSTD)
    target_compile_definitions(nekophoto_core PRIVATE NEKOPHOTO_HAVE_ZSTD)
endif()
if(NEKOPHOTO_HAVE_LIBRAW)
    target_link_libraries(nekophoto_core PRIVATE PkgConfig::LIBRAW)
    target_compile_definitions(nekophoto_core PRIVATE NEKOPHOTO_HAVE_LIBRAW)
endif()
if(NEKOPHOTO_HAVE_MYPAINT)
    target_link_libraries(nekophoto_core PRIVATE PkgConfig::MYPAINT)
    target_compile_definitions(nekophoto_core PRIVATE NEKOPHOTO_HAVE_MYPAINT)
endif()
set_target_properties(nekophoto_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Patchy's PSD primitives (MIT, src/third_party/patchy_psd/README.md): descriptors and binary I/O.
add_library(patchy_psd STATIC
    ${CMAKE_SOURCE_DIR}/src/third_party/patchy_psd/psd/psd_binary.cpp
    ${CMAKE_SOURCE_DIR}/src/third_party/patchy_psd/psd/psd_descriptor.cpp
)
target_include_directories(patchy_psd PUBLIC ${CMAKE_SOURCE_DIR}/src/third_party/patchy_psd)
set_target_properties(patchy_psd PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(nekophoto_core PUBLIC patchy_psd)

# Little CMS (MIT, src/third_party/lcms2/README.md): CMYK PSDs through their own colour profiles.
file(GLOB NEKOPHOTO_LCMS2_SOURCES ${CMAKE_SOURCE_DIR}/src/third_party/lcms2/*.c)
add_library(nekophoto_lcms2 STATIC ${NEKOPHOTO_LCMS2_SOURCES})
target_include_directories(nekophoto_lcms2 PUBLIC ${CMAKE_SOURCE_DIR}/src/third_party/lcms2)
set_target_properties(nekophoto_lcms2 PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
