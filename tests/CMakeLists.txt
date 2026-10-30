# Patchy's PSD fixtures (CONTRIBUTING.md): beside the main checkout, found through git's common directory so a linked
# worktree (.claude/worktrees/..., git worktree add) finds the same checkout as the main one; PATCHY_FIXTURES in the
# environment overrides it. The tests that need them say SKIPPED when they are missing.
set(NEKOPHOTO_MAIN_CHECKOUT "${CMAKE_SOURCE_DIR}")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --path-format=absolute --git-common-dir
                    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" OUTPUT_VARIABLE _common_dir RESULT_VARIABLE _common_result
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_common_result EQUAL 0 AND IS_DIRECTORY "${_common_dir}" AND _common_dir MATCHES "/\\.git$")
        get_filename_component(NEKOPHOTO_MAIN_CHECKOUT "${_common_dir}" DIRECTORY)
    endif()
endif()
get_filename_component(NEKOPHOTO_PATCHY_FIXTURES "${NEKOPHOTO_MAIN_CHECKOUT}/../Patchy/test-fixtures/psd" ABSOLUTE)
if(NOT IS_DIRECTORY "${NEKOPHOTO_PATCHY_FIXTURES}")
    message(STATUS "Patchy's fixtures are not at ${NEKOPHOTO_PATCHY_FIXTURES}: the Photoshop oracle tests will say SKIPPED "
                   "unless PATCHY_FIXTURES names them")
endif()

add_executable(pixels_tests pixels_tests.cpp)
target_link_libraries(pixels_tests PRIVATE nekophoto::pixels nekophoto_warnings)
add_test(NAME pixels_tests COMMAND pixels_tests)

add_executable(core_tests core_tests.cpp)
target_link_libraries(core_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME core_tests COMMAND core_tests)

add_executable(history_tests history_tests.cpp)
target_link_libraries(history_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME history_tests COMMAND history_tests)

# Work done, not time taken: fixed workloads with upper bounds on the work counters (docs/work-counters.md).
add_executable(work_counter_tests work_counter_tests.cpp)
target_link_libraries(work_counter_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME work_counter_tests COMMAND work_counter_tests)
# An adjustment layer's drag through a RenderCache: every frame equals an uncached render (render_resume.h).
add_executable(adjustment_drag_tests adjustment_drag_tests.cpp)
target_link_libraries(adjustment_drag_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME adjustment_drag_tests COMMAND adjustment_drag_tests)

add_executable(imaget_tests imaget_tests.cpp)
target_link_libraries(imaget_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME imaget_tests COMMAND imaget_tests)

add_executable(colormodes_tests colormodes_tests.cpp)
target_link_libraries(colormodes_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME colormodes_tests COMMAND colormodes_tests)
# docs/mode-matrix.md is generated from supports(); the check fails when it is stale.
add_executable(mode_matrix mode_matrix.cpp)
target_link_libraries(mode_matrix PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME mode_matrix_check COMMAND mode_matrix --check ${PROJECT_SOURCE_DIR}/docs/mode-matrix.md)
add_executable(colormodes_render_tests colormodes_render_tests.cpp)
target_link_libraries(colormodes_render_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME colormodes_render_tests COMMAND colormodes_render_tests)

add_executable(blur_tests blur_tests.cpp)
target_link_libraries(blur_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME blur_tests COMMAND blur_tests)

add_executable(selection_tests selection_tests.cpp)
target_link_libraries(selection_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME selection_tests COMMAND selection_tests)
add_executable(channels_tests channels_tests.cpp)
target_link_libraries(channels_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME channels_tests COMMAND channels_tests)

add_executable(blend_tests blend_tests.cpp)
target_link_libraries(blend_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME blend_tests COMMAND blend_tests)

add_executable(kernel_tests kernel_tests.cpp)
target_link_libraries(kernel_tests PRIVATE nekophoto::core nekophoto::pixels nekophoto_warnings)
add_test(NAME kernel_tests COMMAND kernel_tests)

add_executable(nekophoto_bench bench.cpp)
target_link_libraries(nekophoto_bench PRIVATE nekophoto::core nekophoto_warnings)

add_executable(golden_tests golden_tests.cpp)
target_link_libraries(golden_tests PRIVATE nekophoto::core nekophoto_warnings)
target_compile_definitions(golden_tests PRIVATE GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")
add_test(NAME golden_tests COMMAND golden_tests)

# Render hashes over golden scenes, blend modes, brushes, filters and adjustments (docs/high-bit-depth-plan.md, P0).
# NEKOPHOTO_UPDATE_RENDER_HASHES=1 rewrites tests/render_hashes.txt.
add_executable(render_hash_tests render_hash_tests.cpp)
target_link_libraries(render_hash_tests PRIVATE nekophoto::core nekophoto_warnings)
target_compile_definitions(render_hash_tests PRIVATE RENDER_HASHES_FILE="${CMAKE_CURRENT_SOURCE_DIR}/render_hashes.txt"
                           MYPAINT_BRUSHES_DIR="${CMAKE_SOURCE_DIR}/src/app/brushes/mypaint")
add_test(NAME render_hash_tests COMMAND render_hash_tests)

# The Mosh effects: fingerprints (NEKOPHOTO_UPDATE_MOSH_HASHES=1 rewrites tests/mosh_hashes.txt), threads, depths.
add_executable(mosh_tests mosh_tests.cpp)
target_link_libraries(mosh_tests PRIVATE nekophoto::core nekophoto_warnings)
target_include_directories(mosh_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/core/src)
target_compile_definitions(mosh_tests PRIVATE MOSH_HASHES_FILE="${CMAKE_CURRENT_SOURCE_DIR}/mosh_hashes.txt")
add_test(NAME mosh_tests COMMAND mosh_tests)

# Timings for render, brush, blur and adjustments; not a test. Numbers in docs/benchmarks.md.
add_executable(bench_core bench_core.cpp)
target_link_libraries(bench_core PRIVATE nekophoto::core nekophoto_warnings)
target_include_directories(bench_core PRIVATE ${CMAKE_SOURCE_DIR}/src/core/src)   # the render plan, for the layer-count lines

add_executable(resample_tests resample_tests.cpp)
target_link_libraries(resample_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME resample_tests COMMAND resample_tests)

add_executable(heal_tests heal_tests.cpp)
target_link_libraries(heal_tests PRIVATE nekophoto::core nekophoto::pixels nekophoto_warnings)
add_test(NAME heal_tests COMMAND heal_tests)

# The font picker's grouping is plain QtCore logic; test it when Qt is around.
find_package(Qt6 6.4 QUIET COMPONENTS Core)
if(TARGET Qt6::Core)
    # The G'MIC catalogue parser (app code; Qt Core only, with moc for the runner in the same file).
    add_executable(gmic_catalogue_tests gmic_catalogue_tests.cpp ${CMAKE_SOURCE_DIR}/src/app/Gmic.cpp ${CMAKE_SOURCE_DIR}/src/app/Gmic.h)
    set_target_properties(gmic_catalogue_tests PROPERTIES AUTOMOC ON)
    target_include_directories(gmic_catalogue_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/app)
    target_link_libraries(gmic_catalogue_tests PRIVATE Qt6::Core nekophoto::core nekophoto_warnings)
    add_test(NAME gmic_catalogue_tests COMMAND gmic_catalogue_tests)
    # G'MIC downloaded on request: verifying and unpacking the archive, and where the executable is looked for.
    find_package(Qt6 6.4 QUIET COMPONENTS Network)
    if(TARGET Qt6::Network)
        add_executable(gmic_store_tests gmic_store_tests.cpp ${CMAKE_SOURCE_DIR}/src/app/GmicStore.cpp ${CMAKE_SOURCE_DIR}/src/app/GmicStore.h
                       ${CMAKE_SOURCE_DIR}/src/app/Gmic.cpp ${CMAKE_SOURCE_DIR}/src/app/Gmic.h)
        set_target_properties(gmic_store_tests PROPERTIES AUTOMOC ON)
        target_include_directories(gmic_store_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/app)
        target_link_libraries(gmic_store_tests PRIVATE Qt6::Core Qt6::Network nekophoto::core nekophoto_warnings)
        add_test(NAME gmic_store_tests COMMAND gmic_store_tests)
    endif()
    add_executable(font_group_tests font_group_tests.cpp ${CMAKE_SOURCE_DIR}/src/app/FontGroups.cpp)
    target_include_directories(font_group_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/app)
    target_link_libraries(font_group_tests PRIVATE Qt6::Core nekophoto_warnings)
    add_test(NAME font_group_tests COMMAND font_group_tests)
    # The automation socket's limits and the checks on imported actions (app code; Qt Core only).
    add_executable(automation_guard_tests automation_guard_tests.cpp ${CMAKE_SOURCE_DIR}/src/app/AutomationGuard.cpp
                   ${CMAKE_SOURCE_DIR}/src/app/ActionLibrary.cpp ${CMAKE_SOURCE_DIR}/src/app/ActionLibrary.h)
    set_target_properties(automation_guard_tests PROPERTIES AUTOMOC ON)
    target_include_directories(automation_guard_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/app)
    target_link_libraries(automation_guard_tests PRIVATE Qt6::Core nekophoto_warnings)
    add_test(NAME automation_guard_tests COMMAND automation_guard_tests)
    # Scrubby labels (app code on Qt Widgets, run offscreen).
    find_package(Qt6 6.4 QUIET COMPONENTS Widgets)
    if(TARGET Qt6::Widgets)
        add_executable(scrub_tests scrub_tests.cpp ${CMAKE_SOURCE_DIR}/src/app/Scrub.cpp)
        target_include_directories(scrub_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/app)
        target_link_libraries(scrub_tests PRIVATE Qt6::Widgets nekophoto_warnings)
        add_test(NAME scrub_tests COMMAND scrub_tests)
        set_tests_properties(scrub_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
endif()
find_package(Qt6 6.4 QUIET COMPONENTS Gui)
if(TARGET Qt6::Gui)
    # The text editor's document and a text layer's style runs (app code; Qt Gui, an offscreen platform).
    add_executable(rich_text_tests rich_text_tests.cpp ${CMAKE_SOURCE_DIR}/src/app/RichText.cpp)
    target_include_directories(rich_text_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/app)
    target_link_libraries(rich_text_tests PRIVATE Qt6::Gui nekophoto::core nekophoto_warnings)
    add_test(NAME rich_text_tests COMMAND rich_text_tests)
    # Text painted at 16 bits against 8, and a 16-bit type layer through PSD (app code; Qt Gui, offscreen).
    add_executable(depth_text_tests depth_text_tests.cpp ${CMAKE_SOURCE_DIR}/src/app/TextLayer.cpp)
    target_include_directories(depth_text_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/app)
    target_link_libraries(depth_text_tests PRIVATE Qt6::Gui nekophoto::core nekophoto_warnings)
    add_test(NAME depth_text_tests COMMAND depth_text_tests)
endif()

add_executable(psd_tests psd_tests.cpp)
target_link_libraries(psd_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME psd_tests COMMAND psd_tests)
add_executable(artboard_tests artboard_tests.cpp)
target_link_libraries(artboard_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME artboard_tests COMMAND artboard_tests)

add_executable(depth_format_tests depth_format_tests.cpp)
target_link_libraries(depth_format_tests PRIVATE nekophoto::core nekophoto_warnings z)
add_test(NAME depth_format_tests COMMAND depth_format_tests)

add_executable(depth_float_tests depth_float_tests.cpp float_reference.cpp)
target_link_libraries(depth_float_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME depth_float_tests COMMAND depth_float_tests)

add_executable(depth_float_edit_tests depth_float_edit_tests.cpp float_reference.cpp)
target_link_libraries(depth_float_edit_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME depth_float_edit_tests COMMAND depth_float_edit_tests)

# Painting at 32 bits and in CMYK and Lab (P5c, P7 E), against the double reference and Little CMS.
add_executable(paint_modes_tests paint_modes_tests.cpp float_reference.cpp)
target_link_libraries(paint_modes_tests PRIVATE nekophoto::core nekophoto_warnings)
target_compile_definitions(paint_modes_tests PRIVATE BRUSHES_DIR="${CMAKE_SOURCE_DIR}/src/app/brushes/mypaint")
add_test(NAME paint_modes_tests COMMAND paint_modes_tests)
add_executable(adjust_modes_tests adjust_modes_tests.cpp)
target_link_libraries(adjust_modes_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME adjust_modes_tests COMMAND adjust_modes_tests)
add_executable(select_modes_tests select_modes_tests.cpp)
target_link_libraries(select_modes_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME select_modes_tests COMMAND select_modes_tests)
add_executable(retouch_modes_tests retouch_modes_tests.cpp)
target_link_libraries(retouch_modes_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME retouch_modes_tests COMMAND retouch_modes_tests)
add_executable(content_modes_tests content_modes_tests.cpp)
target_link_libraries(content_modes_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME content_modes_tests COMMAND content_modes_tests)
add_executable(transform_modes_tests transform_modes_tests.cpp)
target_link_libraries(transform_modes_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME transform_modes_tests COMMAND transform_modes_tests)

add_executable(depth_float_format_tests depth_float_format_tests.cpp)
target_link_libraries(depth_float_format_tests PRIVATE nekophoto::core nekophoto_warnings z)
add_test(NAME depth_float_format_tests COMMAND depth_float_format_tests)

add_executable(depth_edit_tests depth_edit_tests.cpp)
target_link_libraries(depth_edit_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME depth_edit_tests COMMAND depth_edit_tests)

# Shapes, vector masks and layer styles at 16 bits against their 8-bit paths.
add_executable(depth_vector_tests depth_vector_tests.cpp)
target_link_libraries(depth_vector_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME depth_vector_tests COMMAND depth_vector_tests)

add_executable(depth_paint_tests depth_paint_tests.cpp)
target_link_libraries(depth_paint_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME depth_paint_tests COMMAND depth_paint_tests)

add_executable(psd_writer_tests psd_writer_tests.cpp)
target_link_libraries(psd_writer_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME psd_writer_tests COMMAND psd_writer_tests)
add_executable(psd_modes_tests psd_modes_tests.cpp)
target_link_libraries(psd_modes_tests PRIVATE nekophoto::core nekophoto_warnings ZLIB::ZLIB)
# Patchy beside the main checkout (NEKOPHOTO_PATCHY_FIXTURES above); PATCHY_FIXTURES in the environment overrides it.
target_compile_definitions(psd_modes_tests PRIVATE PATCHY_FIXTURES="${NEKOPHOTO_PATCHY_FIXTURES}")
add_test(NAME psd_modes_tests COMMAND psd_modes_tests)
# Photoshop's merged image as the oracle over three corpora of Photoshop's files, none in this repository: Patchy's
# fixtures, photocraft-corpus ($PHOTOSHOP_CORPUS) and psd-tools' test files ($PSD_TOOLS_CORPUS). Each is SKIPPED (exit
# 77) when it is not there; the expectations are tests/psd_oracle*.txt (docs/compatibility.md).
add_executable(psd_composite_oracle psd_composite_oracle.cpp)
target_link_libraries(psd_composite_oracle PRIVATE nekophoto::core nekophoto_warnings)
target_compile_definitions(psd_composite_oracle PRIVATE PATCHY_FIXTURES="${NEKOPHOTO_PATCHY_FIXTURES}" TESTS_DIR="${CMAKE_SOURCE_DIR}/tests")
add_test(NAME psd_composite_oracle COMMAND psd_composite_oracle --corpus patchy)
add_test(NAME psd_oracle_photocraft COMMAND psd_composite_oracle --corpus photocraft)
add_test(NAME psd_oracle_psdtools COMMAND psd_composite_oracle --corpus psd-tools)
set_tests_properties(psd_composite_oracle psd_oracle_photocraft psd_oracle_psdtools PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800)

# Offline background-removal harness: runs the pipeline on a PNG and dumps every stage, or scores a
# directory of image/alpha pairs (SAD, MSE, MAD, Grad, Conn). Not a test.
add_executable(matte_tool matte_tool.cpp)
target_link_libraries(matte_tool PRIVATE nekophoto::core nekophoto_warnings)

add_executable(mypaint_tests mypaint_tests.cpp)
target_link_libraries(mypaint_tests PRIVATE nekophoto::core nekophoto_warnings)
target_compile_definitions(mypaint_tests PRIVATE BRUSHES_DIR="${CMAKE_SOURCE_DIR}/src/app/brushes/mypaint")
add_test(NAME mypaint_tests COMMAND mypaint_tests)

add_executable(tipbrush_tests tipbrush_tests.cpp)
target_link_libraries(tipbrush_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME tipbrush_tests COMMAND tipbrush_tests)

add_executable(abr_tests abr_tests.cpp)
target_link_libraries(abr_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME abr_tests COMMAND abr_tests)

add_executable(procreate_tests procreate_tests.cpp)
target_link_libraries(procreate_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME procreate_tests COMMAND procreate_tests)

add_executable(sut_tests sut_tests.cpp)
# zlib compresses the test's layer tiles, as Clip Studio does.
find_package(ZLIB REQUIRED)
target_link_libraries(sut_tests PRIVATE nekophoto::core nekophoto_warnings z)
if(NEKOPHOTO_HAVE_SQLITE)
    find_package(SQLite3 REQUIRED)
    if(TARGET SQLite3::SQLite3)
        target_link_libraries(sut_tests PRIVATE SQLite3::SQLite3)
    else()
        target_link_libraries(sut_tests PRIVATE SQLite::SQLite3)
    endif()
    target_compile_definitions(sut_tests PRIVATE NEKOPHOTO_HAVE_SQLITE)
endif()
add_test(NAME sut_tests COMMAND sut_tests)

# The brush parity harness (docs/brush-engine.md): standard motions and recorded strokes painted with every kind of
# brush, hashed and measured against tests/brush_parity_baseline.txt (NEKOPHOTO_UPDATE_BRUSH_PARITY=1 rewrites it).
# brush_parity_tool writes the images and measurements out; it is not a test.
add_library(brush_harness STATIC brush_harness.cpp)
target_link_libraries(brush_harness PUBLIC nekophoto::core nekophoto_warnings ZLIB::ZLIB)
target_compile_definitions(brush_harness PUBLIC BRUSH_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/brush_fixtures"
                           MYPAINT_BRUSHES_DIR="${CMAKE_SOURCE_DIR}/src/app/brushes/mypaint"
                           PROCREATE_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/brushes/procreate"
                           LOCAL_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/local-fixtures")
if(NEKOPHOTO_HAVE_SQLITE)
    if(TARGET SQLite3::SQLite3)
        target_link_libraries(brush_harness PUBLIC SQLite3::SQLite3)
    else()
        target_link_libraries(brush_harness PUBLIC SQLite::SQLite3)
    endif()
    target_compile_definitions(brush_harness PUBLIC NEKOPHOTO_HAVE_SQLITE)
endif()
add_executable(brush_parity brush_parity.cpp)
target_link_libraries(brush_parity PRIVATE brush_harness)
target_compile_definitions(brush_parity PRIVATE BRUSH_PARITY_BASELINE="${CMAKE_CURRENT_SOURCE_DIR}/brush_parity_baseline.txt")
add_test(NAME brush_parity COMMAND brush_parity)
add_executable(brush_dynamics_tests brush_dynamics_tests.cpp)
target_link_libraries(brush_dynamics_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME brush_dynamics_tests COMMAND brush_dynamics_tests)
# Moving grain under torture: phase, spin, wrap, sampling, rotation, zoom, depth and threads, from each dab's trace.
# Moving (stroke-anchored) grain is not applied (docs/legal-boundaries.md): its torture tests were removed with it.
add_executable(brush_smoothing_tests brush_smoothing_tests.cpp)
target_link_libraries(brush_smoothing_tests PRIVATE brush_harness)
add_test(NAME brush_smoothing_tests COMMAND brush_smoothing_tests)
# Imported brushes behaving as in their own application: taper, the stroke's own inputs, curves and controls.
add_executable(brush_feel_tests brush_feel_tests.cpp)
target_link_libraries(brush_feel_tests PRIVATE brush_harness)
add_test(NAME brush_feel_tests COMMAND brush_feel_tests)
add_executable(brush_parity_tool brush_parity_tool.cpp)
target_link_libraries(brush_parity_tool PRIVATE brush_harness)

add_executable(clip_tests clip_tests.cpp)
target_link_libraries(clip_tests PRIVATE nekophoto::core nekophoto_warnings z)
if(NEKOPHOTO_HAVE_SQLITE)
    if(TARGET SQLite3::SQLite3)
        target_link_libraries(clip_tests PRIVATE SQLite3::SQLite3)
    else()
        target_link_libraries(clip_tests PRIVATE SQLite::SQLite3)
    endif()
    target_compile_definitions(clip_tests PRIVATE NEKOPHOTO_HAVE_SQLITE)
endif()
add_test(NAME clip_tests COMMAND clip_tests)
add_executable(wand_bench wand_bench.cpp)
target_link_libraries(wand_bench PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME wand_bench_check COMMAND wand_bench check)

# Round trip of what a PSD carries (psd_carry.h) over a folder of real files, e.g. Patchy's fixtures.
add_executable(psd_roundtrip psd_roundtrip.cpp)
target_link_libraries(psd_roundtrip PRIVATE nekophoto::core nekophoto_warnings)

add_executable(layerstyle_tests layerstyle_tests.cpp)
target_link_libraries(layerstyle_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME layerstyle_tests COMMAND layerstyle_tests)

add_executable(style_modes_tests style_modes_tests.cpp)
target_link_libraries(style_modes_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME style_modes_tests COMMAND style_modes_tests)

add_executable(smartobject_modes_tests smartobject_modes_tests.cpp)
target_link_libraries(smartobject_modes_tests PRIVATE nekophoto::core nekophoto_warnings nekophoto_lcms2)
add_test(NAME smartobject_modes_tests COMMAND smartobject_modes_tests)

add_executable(smartobject_tests smartobject_tests.cpp)
target_link_libraries(smartobject_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME smartobject_tests COMMAND smartobject_tests)

add_executable(smartfilter_tests smartfilter_tests.cpp)
target_link_libraries(smartfilter_tests PRIVATE nekophoto::core nekophoto_warnings)
# Patchy beside the main checkout for Photoshop's Radial Blur Zoom preview; PATCHY_FIXTURES in the environment overrides it.
target_compile_definitions(smartfilter_tests PRIVATE PATCHY_FIXTURES="${NEKOPHOTO_PATCHY_FIXTURES}")
add_test(NAME smartfilter_tests COMMAND smartfilter_tests)

add_executable(vectormask_tests vectormask_tests.cpp)
target_link_libraries(vectormask_tests PRIVATE nekophoto::core nekophoto_warnings)
target_compile_definitions(vectormask_tests PRIVATE PATCHY_FIXTURES="${NEKOPHOTO_PATCHY_FIXTURES}")
add_test(NAME vectormask_tests COMMAND vectormask_tests)

add_executable(cameraraw_tests cameraraw_tests.cpp)
target_link_libraries(cameraraw_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME cameraraw_tests COMMAND cameraraw_tests)

add_executable(restyle_check restyle_check.cpp)
target_link_libraries(restyle_check PRIVATE nekophoto::core nekophoto_warnings)

add_executable(toning_tests toning_tests.cpp)
target_link_libraries(toning_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME toning_tests COMMAND toning_tests)

add_executable(whitebalance_tests whitebalance_tests.cpp)
target_link_libraries(whitebalance_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME whitebalance_tests COMMAND whitebalance_tests)

add_executable(raw_tests raw_tests.cpp)
target_link_libraries(raw_tests PRIVATE nekophoto::core nekophoto_warnings)
target_compile_definitions(raw_tests PRIVATE RAW_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/raw/synthetic.dng"
                                             RAW_DUAL_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/raw/synthetic-dual.dng")
add_test(NAME raw_tests COMMAND raw_tests)

# Camera Raw parity on RAWmakase's synthetic chart (MIT, tests/fixtures/camera-raw-parity), held to
# tests/camera_raw_parity_baseline.txt (NEKOPHOTO_BLESS_CAMERA_RAW_PARITY=1 rewrites it; docs/camera-raw.md).
add_executable(camera_raw_parity camera_raw_parity.cpp)
target_link_libraries(camera_raw_parity PRIVATE nekophoto::core nekophoto_warnings)
target_compile_definitions(camera_raw_parity PRIVATE CAMERA_RAW_PARITY_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/camera-raw-parity"
                                                     CAMERA_RAW_PARITY_BASELINE="${CMAKE_CURRENT_SOURCE_DIR}/camera_raw_parity_baseline.txt")
add_test(NAME camera_raw_parity COMMAND camera_raw_parity)

add_executable(vectorlayer_tests vectorlayer_tests.cpp)
target_link_libraries(vectorlayer_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME vectorlayer_tests COMMAND vectorlayer_tests)

add_executable(formats_tests formats_tests.cpp)
target_link_libraries(formats_tests PRIVATE nekophoto::core nekophoto_warnings z)
add_test(NAME formats_tests COMMAND formats_tests)

add_executable(affinity_tests affinity_tests.cpp)
target_link_libraries(affinity_tests PRIVATE nekophoto::core nekophoto_warnings z)
add_test(NAME affinity_tests COMMAND affinity_tests)

# Not a test: dumps what the Affinity import makes of files (see the file's header).
add_executable(affinity_check affinity_check.cpp)
target_link_libraries(affinity_check PRIVATE nekophoto::core nekophoto_warnings)

add_executable(presets_tests presets_tests.cpp)
target_link_libraries(presets_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME presets_tests COMMAND presets_tests)

add_executable(svg_tests svg_tests.cpp)
target_link_libraries(svg_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME svg_tests COMMAND svg_tests)

add_executable(hostile_input_tests hostile_input_tests.cpp)
target_link_libraries(hostile_input_tests PRIVATE nekophoto::core nekophoto_warnings z)
add_test(NAME hostile_input_tests COMMAND hostile_input_tests)

add_executable(seamcarve_tests seamcarve_tests.cpp)
target_link_libraries(seamcarve_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME seamcarve_tests COMMAND seamcarve_tests)

# Every Japanese string translated and none left over from removed code (tools/check_translations.py runs lupdate
# over src/app into a copy of the .ts, so a new tr() string without a translation fails here).
# It checks the sources against the .ts, which is the same on every platform, so it runs where the Linux CI runs it;
# on the Windows runner lupdate or Python exited without a word, failing a check that has nothing Windows-specific.
find_package(Qt6 QUIET COMPONENTS LinguistTools)
find_package(Python3 QUIET COMPONENTS Interpreter)
if(NEKOPHOTO_BUILD_APP AND NOT WIN32 AND TARGET Qt6::lupdate AND Python3_Interpreter_FOUND)
    add_test(NAME translations_check COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_translations.py
        --lupdate $<TARGET_FILE:Qt6::lupdate>)
endif()
# The legal boundaries' source checks (docs/legal-boundaries.md).
if(Python3_Interpreter_FOUND)
    add_test(NAME legal_boundaries_check COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/check_legal_boundaries.py)
    # docs/compatibility.md's "At a glance" counts and README.md's measured-results badges, generated from the
    # repository: stale fails (COMPAT_TABLE_WRITE=1 rewrites them).
    add_test(NAME compat_table_check COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/compat_table.py --check)
endif()

# Colour management: transforms against Little CMS (linked directly for the reference values), proofing, Convert.
add_executable(colormgmt_tests colormgmt_tests.cpp)
target_link_libraries(colormgmt_tests PRIVATE nekophoto::core nekophoto_warnings nekophoto_lcms2)
add_test(NAME colormgmt_tests COMMAND colormgmt_tests)

add_executable(colormgmt_cmyk_tests colormgmt_cmyk_tests.cpp)
target_link_libraries(colormgmt_cmyk_tests PRIVATE nekophoto::core nekophoto_warnings nekophoto_lcms2)
add_test(NAME colormgmt_cmyk_tests COMMAND colormgmt_cmyk_tests)
# The display's precalculated CMYK and Lab transform against Little CMS (display_grid_tests --measure prints the error).
add_executable(display_grid_tests display_grid_tests.cpp)
target_link_libraries(display_grid_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME display_grid_tests COMMAND display_grid_tests)
set_tests_properties(display_grid_tests PROPERTIES ENVIRONMENT "DISPLAY_GRID_WORST=1")
# A fixed CMYK or Lab canvas workload for counting instructions (not a test; tests/display_work.cpp).
add_executable(display_work display_work.cpp)
target_link_libraries(display_work PRIVATE nekophoto::core nekophoto_warnings)

if(TARGET nekophoto)
    # The whole window: the converted commands give the same document and history from the menus, dialogs and
    # canvas as from automation, as before the command path, and record the same Actions steps (SelfTest.cpp).
    add_test(NAME command_path_selftest COMMAND nekophoto --self-test command-path)
    set_tests_properties(command_path_selftest PROPERTIES ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/selftest-home/config;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/selftest-home/data")
    # Ruler guides through the pointer: out of a ruler, moved, dragged off; each one undo step.
    # The canvas's context menu per tool: what applies is there, the rest left out (English, for the texts).
    add_test(NAME canvas_menus_selftest COMMAND nekophoto --lang en --self-test canvas-menus)
    set_tests_properties(canvas_menus_selftest PROPERTIES ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/selftest-home/config;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/selftest-home/data")
    add_test(NAME guides_selftest COMMAND nekophoto --self-test guides)
    set_tests_properties(guides_selftest PROPERTIES ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/selftest-home/config;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/selftest-home/data")
    # Photoshop's held keys: Alt's Eyedropper, Ctrl's Move tool, Ctrl+Space's zoom, spring-loaded tool letters, F7,
    # F12, Ctrl+Alt+Z and the type size keys, as synthesised key and mouse events.
    add_test(NAME held_keys_selftest COMMAND nekophoto --lang en --self-test held-keys)
    set_tests_properties(held_keys_selftest PROPERTIES ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/held-keys-home/config;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/held-keys-home/data")
    # File > Export > Export As, Quick Export and Layer > Export As: the estimate is the file's size, scaling, the
    # settings remembered per format, a layer cropped to its pixels.
    add_test(NAME export_as_selftest COMMAND nekophoto --self-test export-as)
    set_tests_properties(export_as_selftest PROPERTIES ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/export-home/config;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/export-home/data")
    # Edit > Search…: Ctrl+F, typed queries and their top result, Enter, Recently Used (English, for the labels).
    # The window's work for a brush stroke, an adjustment slider drag, pans and zooms, bounded by the work counters
    # (SelfTestWork.cpp, docs/work-counters.md); at a 1x display scale, which the dab-sized bounds assume.
    add_test(NAME work_counters_selftest COMMAND nekophoto --self-test work-counters)
    set_tests_properties(work_counters_selftest PROPERTIES ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1;NEKOPHOTO_WINDOW_SIZE=1200x800;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/work-counters-home/config;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/work-counters-home/data")
    add_test(NAME search_selftest COMMAND nekophoto --lang en --self-test search)
    set_tests_properties(search_selftest PROPERTIES ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/search-home/config;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/search-home/data")
    # Edit > Preferences: the autosave interval applies while the dialog is open; next-launch settings say so (English).
    add_test(NAME preferences_selftest COMMAND nekophoto --lang en --self-test preferences)
    set_tests_properties(preferences_selftest PROPERTIES ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/preferences-home/config;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/preferences-home/data")
    # Edit > Keyboard Shortcuts: keys changed, refused, moved on a conflict, reset, kept in the settings and applied
    # again, exported and imported, with no key on two actions (English: it names Group Layers).
    add_test(NAME keyboard_shortcuts_selftest COMMAND nekophoto --lang en --self-test shortcuts)
    set_tests_properties(keyboard_shortcuts_selftest PROPERTIES ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/shortcuts-home/config;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/shortcuts-home/data")
    # View > Screen Mode > Full Screen: nothing but the canvas; each edge's flyout slides out from its hot zone and back,
    # not while a button is held on the canvas; the Layers panel in the right flyout; F10, Alt, Tab, Shift+Tab, F7, Esc;
    # leaving restores the window's state byte for byte. Under the default theme (Goth Kitty).
    add_test(NAME full_screen_selftest COMMAND nekophoto --lang en --self-test full-screen)
    set_tests_properties(full_screen_selftest PROPERTIES ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/full-screen-home/config;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/full-screen-home/data")
    # Edit > Preferences > Edges: an edge autohiding in the standard window leaves it to the canvas, its flyout slides out
    # from its hot zone, not during a stroke begun on the canvas; Tab and Shift+Tab and the wheel over the strip flip its
    # pages; a tap of Tab with none out still hides the panels, a hold peeks; fields keep Tab; the page remembered, the
    # tool's own; full screen on the same pages.
    add_test(NAME edges_selftest COMMAND nekophoto --lang en --self-test edges)
    set_tests_properties(edges_selftest PROPERTIES ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/edges-home/config;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/edges-home/data")
    # Tab and Shift+Tab in the Standard Screen Mode: the panels, tools and options bar hidden and given back byte for
    # byte, a closed panel staying closed; Shift+Tab the right panels only; fields and typing keep Tab; full screen
    # and F7 while they are hidden.
    # A page turn of an edge shows the page leaving as it was drawn: held mid-flip in a window just opened, the
    # Properties page's Histogram has been counted (not an empty grey graph).
    add_test(NAME edge_flip_selftest COMMAND nekophoto --lang en --self-test edge-flip)
    set_tests_properties(edge_flip_selftest PROPERTIES ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/edge-flip-home/config;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/edge-flip-home/data")
    # The Move tool's transform fields on an adjustment layer: empty, greyed and saying why; back on a pixel
    # layer they show it again.
    add_test(NAME move_options_selftest COMMAND nekophoto --lang en --self-test move-options)
    set_tests_properties(move_options_selftest PROPERTIES ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/move-options-home/config;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/move-options-home/data")
    # The tool rail's swatches through an Adobe RGB monitor profile: the colours from the document's profile in
    # RGB, as sRGB in CMYK and Lab.
    add_test(NAME swatches_selftest COMMAND nekophoto --lang en --self-test swatches)
    set_tests_properties(swatches_selftest PROPERTIES ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/swatches-home/config;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/swatches-home/data")
    # Drops and pastes from elsewhere (SelfTestIncoming.cpp): with no document, onto a blank one, onto one worked on;
    # browser drops shaped as Firefox and Chromium make them, an http address served by the test itself.
    add_test(NAME incoming_selftest COMMAND nekophoto --lang en --self-test incoming)
    set_tests_properties(incoming_selftest PROPERTIES ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/incoming-home/config;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/incoming-home/data")
    add_test(NAME hide_panels_selftest COMMAND nekophoto --lang en --self-test hide-panels)
    set_tests_properties(hide_panels_selftest PROPERTIES ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/hide-panels-home/config;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/hide-panels-home/data")
    # The forbidden cursor: what the painting tools refuse (a group, an adjustment, a hidden layer, a smart object, type
    # and shape layers), the cursor and the status bar's reason following the layer and the tool, Rasterize Type.
    add_test(NAME forbidden_cursor_selftest COMMAND nekophoto --lang en --self-test forbidden-cursor)
    set_tests_properties(forbidden_cursor_selftest PROPERTIES ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/forbidden-cursor-home/config;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/forbidden-cursor-home/data")
    # View > Tool Ring (Alt+D): no held Eyedropper or menu bar left behind, placed clear of the pointer inside the canvas
    # and moved only when run into, F/S the brush size, a stroke under way left whole, its letters its own while open,
    # Esc, Alt+D, another tool and the idle timeout closing it, the mouse going through it, the wheel, full screen.
    add_test(NAME tool_ring_selftest COMMAND nekophoto --lang en --self-test tool-ring)
    set_tests_properties(tool_ring_selftest PROPERTIES ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/tool-ring-home/config;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/tool-ring-home/data")
    # View > Colour Ring (Alt+C): S/F hue, X/E saturation, V/R brightness, C swap, D defaults, W recent colours, the
    # wheel, no undo steps, the mouse going through it, a stroke under way carrying on, taking turns with the Tool Ring.
    add_test(NAME color_ring_selftest COMMAND nekophoto --lang en --self-test color-ring)
    set_tests_properties(color_ring_selftest PROPERTIES ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/color-ring-home/config;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/color-ring-home/data")
    # View > Layer Picker (Alt+A): exactly the layers showing under the pointer, top first; F/R and S/V stepping and
    # choosing live, Esc putting the layer back, Alt+A, Enter and a click keeping it, no document change, "No layer
    # here", the Layers panel hidden, full screen.
    add_test(NAME layer_picker_selftest COMMAND nekophoto --lang en --self-test layer-picker)
    set_tests_properties(layer_picker_selftest PROPERTIES ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/layer-picker-home/config;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/layer-picker-home/data")
    # The interface's motion on a stepped clock: the rings' pop and close (slower on the idle timeout), keys acting at
    # once during either, a value's roll, a greyed key's shake, the glide out of the pointer's way, the Layer Picker's
    # bar gliding and going back on Esc, the Colour Ring's swap, an edge's slide, a page turn, hold Tab's stagger,
    # Reduce motion turning it all into quick fades or nothing, and no document render from any of it.
    add_test(NAME motion_selftest COMMAND nekophoto --lang en --self-test motion)
    set_tests_properties(motion_selftest PROPERTIES ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/motion-home/config;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/motion-home/data")
endif()

add_executable(blendif_tests blendif_tests.cpp)
target_link_libraries(blendif_tests PRIVATE nekophoto::core nekophoto_warnings)
# Patchy beside the main checkout (NEKOPHOTO_PATCHY_FIXTURES above); PATCHY_FIXTURES in the environment overrides it.
target_compile_definitions(blendif_tests PRIVATE PATCHY_FIXTURES="${NEKOPHOTO_PATCHY_FIXTURES}")
add_test(NAME blendif_tests COMMAND blendif_tests)

# Fill in Photoshop's special eight blend modes against Photoshop's measured composites. NekoPhoto still takes Fill
# as opacity in every mode, so the special-eight case fails today: it is registered WILL_FAIL until Fill is applied
# inside the blend; then drop WILL_FAIL (ctest reports the fixed test as failing until you do).
add_executable(fill_opacity_tests fill_opacity_tests.cpp)
target_link_libraries(fill_opacity_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME fill_opacity_tests COMMAND fill_opacity_tests other_modes_take_fill_as_coverage)
add_test(NAME fill_opacity_separate COMMAND fill_opacity_tests opacity_edit_keeps_fill)
add_test(NAME fill_opacity_project COMMAND fill_opacity_tests project_before_fill_splits_the_folded_opacity)
add_test(NAME fill_opacity_special_eight COMMAND fill_opacity_tests special_eight_fill_matches_photoshop)
add_test(NAME fill_opacity_depths COMMAND fill_opacity_tests special_fill_at_16_bits_and_with_effects)
add_test(NAME fill_opacity_cmyk COMMAND fill_opacity_tests special_fill_in_cmyk)

add_executable(photoshop_adjustment_tests photoshop_adjustment_tests.cpp)
target_link_libraries(photoshop_adjustment_tests PRIVATE nekophoto::core nekophoto_warnings)
target_compile_definitions(photoshop_adjustment_tests PRIVATE PATCHY_FIXTURES="${NEKOPHOTO_PATCHY_FIXTURES}")
add_test(NAME photoshop_adjustment_tests COMMAND photoshop_adjustment_tests)

add_executable(gradient_method_tests gradient_method_tests.cpp)
target_link_libraries(gradient_method_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME gradient_method_tests COMMAND gradient_method_tests)
add_executable(histogram_tests histogram_tests.cpp)
target_link_libraries(histogram_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME histogram_tests COMMAND histogram_tests)

add_executable(document_file_tests document_file_tests.cpp)
target_link_libraries(document_file_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME document_file_tests COMMAND document_file_tests)

# The Filter menu's grid filters: the Smart Filter kernels run destructively and the PhotoCraft ports.
add_executable(gridfilter_tests gridfilter_tests.cpp)
target_link_libraries(gridfilter_tests PRIVATE nekophoto::core nekophoto_warnings)
add_test(NAME gridfilter_tests COMMAND gridfilter_tests)

# The standard headers the tests include, precompiled once in core_tests (NEKOPHOTO_PCH) and reused by every test on
# the core alone; the ones on Qt are compiled with other flags and parse their headers themselves.
if(NEKOPHOTO_PCH)
    nekophoto_precompile_headers(core_tests algorithm array cmath cstdint cstdio cstdlib cstring filesystem fstream functional map
                                  memory optional random set string vector)
    get_property(nekophoto_test_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(test_target IN LISTS nekophoto_test_targets)
        get_target_property(test_type ${test_target} TYPE)
        get_target_property(test_libraries ${test_target} LINK_LIBRARIES)
        if(NOT test_target STREQUAL "core_tests" AND test_type MATCHES "^(EXECUTABLE|STATIC_LIBRARY)$" AND NOT test_libraries MATCHES "Qt6::")
            set_target_properties(${test_target} PROPERTIES PRECOMPILE_HEADERS_REUSE_FROM core_tests)
        endif()
    endforeach()
endif()
