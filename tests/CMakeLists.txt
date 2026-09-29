add_executable(pixels_tests pixels_tests.cpp)
target_link_libraries(pixels_tests PRIVATE compositor::pixels compositor_warnings)
add_test(NAME pixels_tests COMMAND pixels_tests)

add_executable(core_tests core_tests.cpp)
target_link_libraries(core_tests PRIVATE compositor::core compositor_warnings)
add_test(NAME core_tests COMMAND core_tests)

add_executable(history_tests history_tests.cpp)
target_link_libraries(history_tests PRIVATE compositor::core compositor_warnings)
add_test(NAME history_tests COMMAND history_tests)

add_executable(imaget_tests imaget_tests.cpp)
target_link_libraries(imaget_tests PRIVATE compositor::core compositor_warnings)
add_test(NAME imaget_tests COMMAND imaget_tests)

add_executable(colormodes_tests colormodes_tests.cpp)
target_link_libraries(colormodes_tests PRIVATE compositor::core compositor_warnings)
add_test(NAME colormodes_tests COMMAND colormodes_tests)
add_executable(colormodes_render_tests colormodes_render_tests.cpp)
target_link_libraries(colormodes_render_tests PRIVATE compositor::core compositor_warnings)
add_test(NAME colormodes_render_tests COMMAND colormodes_render_tests)

add_executable(blur_tests blur_tests.cpp)
target_link_libraries(blur_tests PRIVATE compositor::core compositor_warnings)
add_test(NAME blur_tests COMMAND blur_tests)

add_executable(selection_tests selection_tests.cpp)
target_link_libraries(selection_tests PRIVATE compositor::core compositor_warnings)
add_test(NAME selection_tests COMMAND selection_tests)
add_executable(channels_tests channels_tests.cpp)
target_link_libraries(channels_tests PRIVATE compositor::core compositor_warnings)
add_test(NAME channels_tests COMMAND channels_tests)

add_executable(blend_tests blend_tests.cpp)
target_link_libraries(blend_tests PRIVATE compositor::core compositor_warnings)
add_test(NAME blend_tests COMMAND blend_tests)

add_executable(kernel_tests kernel_tests.cpp)
target_link_libraries(kernel_tests PRIVATE compositor::core compositor::pixels compositor_warnings)
add_test(NAME kernel_tests COMMAND kernel_tests)

add_executable(compositor_bench bench.cpp)
target_link_libraries(compositor_bench PRIVATE compositor::core compositor_warnings)

add_executable(golden_tests golden_tests.cpp)
target_link_libraries(golden_tests PRIVATE compositor::core compositor_warnings)
target_compile_definitions(golden_tests PRIVATE GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")
add_test(NAME golden_tests COMMAND golden_tests)

# Render hashes over golden scenes, blend modes, brushes, filters and adjustments (docs/high-bit-depth-plan.md, P0).
# COMPOSITOR_UPDATE_RENDER_HASHES=1 rewrites tests/render_hashes.txt.
add_executable(render_hash_tests render_hash_tests.cpp)
target_link_libraries(render_hash_tests PRIVATE compositor::core compositor_warnings)
target_compile_definitions(render_hash_tests PRIVATE RENDER_HASHES_FILE="${CMAKE_CURRENT_SOURCE_DIR}/render_hashes.txt"
                           MYPAINT_BRUSHES_DIR="${CMAKE_SOURCE_DIR}/src/app/brushes/mypaint")
add_test(NAME render_hash_tests COMMAND render_hash_tests)

# The Mosh effects: fingerprints (COMPOSITOR_UPDATE_MOSH_HASHES=1 rewrites tests/mosh_hashes.txt), threads, depths.
add_executable(mosh_tests mosh_tests.cpp)
target_link_libraries(mosh_tests PRIVATE compositor::core compositor_warnings)
target_include_directories(mosh_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/core/src)
target_compile_definitions(mosh_tests PRIVATE MOSH_HASHES_FILE="${CMAKE_CURRENT_SOURCE_DIR}/mosh_hashes.txt")
add_test(NAME mosh_tests COMMAND mosh_tests)

# Timings for render, brush, blur and adjustments; not a test. Numbers in docs/benchmarks.md.
add_executable(bench_core bench_core.cpp)
target_link_libraries(bench_core PRIVATE compositor::core compositor_warnings)
target_include_directories(bench_core PRIVATE ${CMAKE_SOURCE_DIR}/src/core/src)   # the render plan, for the layer-count lines

add_executable(resample_tests resample_tests.cpp)
target_link_libraries(resample_tests PRIVATE compositor::core compositor_warnings)
add_test(NAME resample_tests COMMAND resample_tests)

add_executable(heal_tests heal_tests.cpp)
target_link_libraries(heal_tests PRIVATE compositor::core compositor::pixels compositor_warnings)
add_test(NAME heal_tests COMMAND heal_tests)

# The font picker's grouping is plain QtCore logic; test it when Qt is around.
find_package(Qt6 6.4 QUIET COMPONENTS Core)
if(TARGET Qt6::Core)
    # The G'MIC catalogue parser (app code; Qt Core only, with moc for the runner in the same file).
    add_executable(gmic_catalogue_tests gmic_catalogue_tests.cpp ${CMAKE_SOURCE_DIR}/src/app/Gmic.cpp ${CMAKE_SOURCE_DIR}/src/app/Gmic.h)
    set_target_properties(gmic_catalogue_tests PROPERTIES AUTOMOC ON)
    target_include_directories(gmic_catalogue_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/app)
    target_link_libraries(gmic_catalogue_tests PRIVATE Qt6::Core compositor::core compositor_warnings)
    add_test(NAME gmic_catalogue_tests COMMAND gmic_catalogue_tests)
    add_executable(font_group_tests font_group_tests.cpp ${CMAKE_SOURCE_DIR}/src/app/FontGroups.cpp)
    target_include_directories(font_group_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/app)
    target_link_libraries(font_group_tests PRIVATE Qt6::Core compositor_warnings)
    add_test(NAME font_group_tests COMMAND font_group_tests)
endif()
find_package(Qt6 6.4 QUIET COMPONENTS Gui)
if(TARGET Qt6::Gui)
    # The text editor's document and a text layer's style runs (app code; Qt Gui, an offscreen platform).
    add_executable(rich_text_tests rich_text_tests.cpp ${CMAKE_SOURCE_DIR}/src/app/RichText.cpp)
    target_include_directories(rich_text_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/app)
    target_link_libraries(rich_text_tests PRIVATE Qt6::Gui compositor::core compositor_warnings)
    add_test(NAME rich_text_tests COMMAND rich_text_tests)
    # Text painted at 16 bits against 8, and a 16-bit type layer through PSD (app code; Qt Gui, offscreen).
    add_executable(depth_text_tests depth_text_tests.cpp ${CMAKE_SOURCE_DIR}/src/app/TextLayer.cpp)
    target_include_directories(depth_text_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/app)
    target_link_libraries(depth_text_tests PRIVATE Qt6::Gui compositor::core compositor_warnings)
    add_test(NAME depth_text_tests COMMAND depth_text_tests)
endif()

add_executable(psd_tests psd_tests.cpp)
target_link_libraries(psd_tests PRIVATE compositor::core compositor_warnings)
add_test(NAME psd_tests COMMAND psd_tests)
add_executable(artboard_tests artboard_tests.cpp)
target_link_libraries(artboard_tests PRIVATE compositor::core compositor_warnings)
add_test(NAME artboard_tests COMMAND artboard_tests)

add_executable(depth_format_tests depth_format_tests.cpp)
target_link_libraries(depth_format_tests PRIVATE compositor::core compositor_warnings z)
add_test(NAME depth_format_tests COMMAND depth_format_tests)

add_executable(depth_edit_tests depth_edit_tests.cpp)
target_link_libraries(depth_edit_tests PRIVATE compositor::core compositor_warnings)
add_test(NAME depth_edit_tests COMMAND depth_edit_tests)

# Shapes, vector masks and layer styles at 16 bits against their 8-bit paths.
add_executable(depth_vector_tests depth_vector_tests.cpp)
target_link_libraries(depth_vector_tests PRIVATE compositor::core compositor_warnings)
add_test(NAME depth_vector_tests COMMAND depth_vector_tests)

add_executable(depth_paint_tests depth_paint_tests.cpp)
target_link_libraries(depth_paint_tests PRIVATE compositor::core compositor_warnings)
add_test(NAME depth_paint_tests COMMAND depth_paint_tests)

add_executable(psd_writer_tests psd_writer_tests.cpp)
target_link_libraries(psd_writer_tests PRIVATE compositor::core compositor_warnings)
add_test(NAME psd_writer_tests COMMAND psd_writer_tests)
add_executable(psd_modes_tests psd_modes_tests.cpp)
target_link_libraries(psd_modes_tests PRIVATE compositor::core compositor_warnings ZLIB::ZLIB)
# Patchy beside this checkout (CONTRIBUTING.md); PATCHY_FIXTURES in the environment overrides it.
target_compile_definitions(psd_modes_tests PRIVATE PATCHY_FIXTURES="${CMAKE_SOURCE_DIR}/../Patchy/test-fixtures/psd")
add_test(NAME psd_modes_tests COMMAND psd_modes_tests)

# Offline background-removal harness: runs the pipeline on a PNG and dumps every stage, or scores a
# directory of image/alpha pairs (SAD, MSE, MAD, Grad, Conn). Not a test.
add_executable(matte_tool matte_tool.cpp)
target_link_libraries(matte_tool PRIVATE compositor::core compositor_warnings)

add_executable(mypaint_tests mypaint_tests.cpp)
target_link_libraries(mypaint_tests PRIVATE compositor::core compositor_warnings)
target_compile_definitions(mypaint_tests PRIVATE BRUSHES_DIR="${CMAKE_SOURCE_DIR}/src/app/brushes/mypaint")
add_test(NAME mypaint_tests COMMAND mypaint_tests)

add_executable(tipbrush_tests tipbrush_tests.cpp)
target_link_libraries(tipbrush_tests PRIVATE compositor::core compositor_warnings)
add_test(NAME tipbrush_tests COMMAND tipbrush_tests)

add_executable(abr_tests abr_tests.cpp)
target_link_libraries(abr_tests PRIVATE compositor::core compositor_warnings)
add_test(NAME abr_tests COMMAND abr_tests)

add_executable(procreate_tests procreate_tests.cpp)
target_link_libraries(procreate_tests PRIVATE compositor::core compositor_warnings)
add_test(NAME procreate_tests COMMAND procreate_tests)

add_executable(sut_tests sut_tests.cpp)
# zlib compresses the test's layer tiles, as Clip Studio does.
find_package(ZLIB REQUIRED)
target_link_libraries(sut_tests PRIVATE compositor::core compositor_warnings z)
if(COMPOSITOR_HAVE_SQLITE)
    find_package(SQLite3 REQUIRED)
    if(TARGET SQLite3::SQLite3)
        target_link_libraries(sut_tests PRIVATE SQLite3::SQLite3)
    else()
        target_link_libraries(sut_tests PRIVATE SQLite::SQLite3)
    endif()
    target_compile_definitions(sut_tests PRIVATE COMPOSITOR_HAVE_SQLITE)
endif()
add_test(NAME sut_tests COMMAND sut_tests)

# The brush parity harness (docs/brush-engine.md): standard motions and recorded strokes painted with every kind of
# brush, hashed and measured against tests/brush_parity_baseline.txt (COMPOSITOR_UPDATE_BRUSH_PARITY=1 rewrites it).
# brush_parity_tool writes the images and measurements out; it is not a test.
add_library(brush_harness STATIC brush_harness.cpp)
target_link_libraries(brush_harness PUBLIC compositor::core compositor_warnings ZLIB::ZLIB)
target_compile_definitions(brush_harness PUBLIC BRUSH_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/brush_fixtures"
                           MYPAINT_BRUSHES_DIR="${CMAKE_SOURCE_DIR}/src/app/brushes/mypaint"
                           PROCREATE_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/brushes/procreate"
                           LOCAL_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/local-fixtures")
if(COMPOSITOR_HAVE_SQLITE)
    if(TARGET SQLite3::SQLite3)
        target_link_libraries(brush_harness PUBLIC SQLite3::SQLite3)
    else()
        target_link_libraries(brush_harness PUBLIC SQLite::SQLite3)
    endif()
    target_compile_definitions(brush_harness PUBLIC COMPOSITOR_HAVE_SQLITE)
endif()
add_executable(brush_parity brush_parity.cpp)
target_link_libraries(brush_parity PRIVATE brush_harness)
target_compile_definitions(brush_parity PRIVATE BRUSH_PARITY_BASELINE="${CMAKE_CURRENT_SOURCE_DIR}/brush_parity_baseline.txt")
add_test(NAME brush_parity COMMAND brush_parity)
add_executable(brush_dynamics_tests brush_dynamics_tests.cpp)
target_link_libraries(brush_dynamics_tests PRIVATE compositor::core compositor_warnings)
add_test(NAME brush_dynamics_tests COMMAND brush_dynamics_tests)
# Moving grain under torture: phase, spin, wrap, sampling, rotation, zoom, depth and threads, from each dab's trace.
add_executable(brush_grain_tests brush_grain_tests.cpp)
target_link_libraries(brush_grain_tests PRIVATE brush_harness)
add_test(NAME brush_grain_tests COMMAND brush_grain_tests)
add_executable(brush_smoothing_tests brush_smoothing_tests.cpp)
target_link_libraries(brush_smoothing_tests PRIVATE brush_harness)
add_test(NAME brush_smoothing_tests COMMAND brush_smoothing_tests)
add_executable(brush_parity_tool brush_parity_tool.cpp)
target_link_libraries(brush_parity_tool PRIVATE brush_harness)

add_executable(clip_tests clip_tests.cpp)
target_link_libraries(clip_tests PRIVATE compositor::core compositor_warnings z)
if(COMPOSITOR_HAVE_SQLITE)
    if(TARGET SQLite3::SQLite3)
        target_link_libraries(clip_tests PRIVATE SQLite3::SQLite3)
    else()
        target_link_libraries(clip_tests PRIVATE SQLite::SQLite3)
    endif()
    target_compile_definitions(clip_tests PRIVATE COMPOSITOR_HAVE_SQLITE)
endif()
add_test(NAME clip_tests COMMAND clip_tests)
add_executable(wand_bench wand_bench.cpp)
target_link_libraries(wand_bench PRIVATE compositor::core compositor_warnings)
add_test(NAME wand_bench_check COMMAND wand_bench check)

# Round trip of what a PSD carries (psd_carry.h) over a folder of real files, e.g. Patchy's fixtures.
add_executable(psd_roundtrip psd_roundtrip.cpp)
target_link_libraries(psd_roundtrip PRIVATE compositor::core compositor_warnings)

add_executable(layerstyle_tests layerstyle_tests.cpp)
target_link_libraries(layerstyle_tests PRIVATE compositor::core compositor_warnings)
add_test(NAME layerstyle_tests COMMAND layerstyle_tests)

add_executable(smartobject_tests smartobject_tests.cpp)
target_link_libraries(smartobject_tests PRIVATE compositor::core compositor_warnings)
add_test(NAME smartobject_tests COMMAND smartobject_tests)

add_executable(smartfilter_tests smartfilter_tests.cpp)
target_link_libraries(smartfilter_tests PRIVATE compositor::core compositor_warnings)
add_test(NAME smartfilter_tests COMMAND smartfilter_tests)

add_executable(vectormask_tests vectormask_tests.cpp)
target_link_libraries(vectormask_tests PRIVATE compositor::core compositor_warnings)
add_test(NAME vectormask_tests COMMAND vectormask_tests)

add_executable(cameraraw_tests cameraraw_tests.cpp)
target_link_libraries(cameraraw_tests PRIVATE compositor::core compositor_warnings)
add_test(NAME cameraraw_tests COMMAND cameraraw_tests)

add_executable(restyle_check restyle_check.cpp)
target_link_libraries(restyle_check PRIVATE compositor::core compositor_warnings)

add_executable(toning_tests toning_tests.cpp)
target_link_libraries(toning_tests PRIVATE compositor::core compositor_warnings)
add_test(NAME toning_tests COMMAND toning_tests)

add_executable(raw_tests raw_tests.cpp)
target_link_libraries(raw_tests PRIVATE compositor::core compositor_warnings)
add_test(NAME raw_tests COMMAND raw_tests)

add_executable(vectorlayer_tests vectorlayer_tests.cpp)
target_link_libraries(vectorlayer_tests PRIVATE compositor::core compositor_warnings)
add_test(NAME vectorlayer_tests COMMAND vectorlayer_tests)

add_executable(formats_tests formats_tests.cpp)
target_link_libraries(formats_tests PRIVATE compositor::core compositor_warnings z)
add_test(NAME formats_tests COMMAND formats_tests)

add_executable(affinity_tests affinity_tests.cpp)
target_link_libraries(affinity_tests PRIVATE compositor::core compositor_warnings z)
add_test(NAME affinity_tests COMMAND affinity_tests)

# Not a test: dumps what the Affinity import makes of files (see the file's header).
add_executable(affinity_check affinity_check.cpp)
target_link_libraries(affinity_check PRIVATE compositor::core compositor_warnings)

add_executable(presets_tests presets_tests.cpp)
target_link_libraries(presets_tests PRIVATE compositor::core compositor_warnings)
add_test(NAME presets_tests COMMAND presets_tests)

add_executable(svg_tests svg_tests.cpp)
target_link_libraries(svg_tests PRIVATE compositor::core compositor_warnings)
add_test(NAME svg_tests COMMAND svg_tests)

add_executable(hostile_input_tests hostile_input_tests.cpp)
target_link_libraries(hostile_input_tests PRIVATE compositor::core compositor_warnings z)
add_test(NAME hostile_input_tests COMMAND hostile_input_tests)

add_executable(seamcarve_tests seamcarve_tests.cpp)
target_link_libraries(seamcarve_tests PRIVATE compositor::core compositor_warnings)
add_test(NAME seamcarve_tests COMMAND seamcarve_tests)

# Every Japanese string translated and none left over from removed code (tools/check_translations.py runs lupdate
# over src/app into a copy of the .ts, so a new tr() string without a translation fails here).
# It checks the sources against the .ts, which is the same on every platform, so it runs where the Linux CI runs it;
# on the Windows runner lupdate or Python exited without a word, failing a check that has nothing Windows-specific.
find_package(Qt6 QUIET COMPONENTS LinguistTools)
find_package(Python3 QUIET COMPONENTS Interpreter)
if(COMPOSITOR_BUILD_APP AND NOT WIN32 AND TARGET Qt6::lupdate AND Python3_Interpreter_FOUND)
    add_test(NAME translations_check COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_translations.py
        --lupdate $<TARGET_FILE:Qt6::lupdate>)
endif()

# Colour management: transforms against Little CMS (linked directly for the reference values), proofing, Convert.
add_executable(colormgmt_tests colormgmt_tests.cpp)
target_link_libraries(colormgmt_tests PRIVATE compositor::core compositor_warnings nekophoto_lcms2)
add_test(NAME colormgmt_tests COMMAND colormgmt_tests)

add_executable(colormgmt_cmyk_tests colormgmt_cmyk_tests.cpp)
target_link_libraries(colormgmt_cmyk_tests PRIVATE compositor::core compositor_warnings nekophoto_lcms2)
add_test(NAME colormgmt_cmyk_tests COMMAND colormgmt_cmyk_tests)
