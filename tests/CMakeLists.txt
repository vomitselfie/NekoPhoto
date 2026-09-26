add_executable(pixels_tests pixels_tests.cpp)
target_link_libraries(pixels_tests PRIVATE compositor::pixels compositor_warnings)
add_test(NAME pixels_tests COMMAND pixels_tests)

add_executable(core_tests core_tests.cpp)
target_link_libraries(core_tests PRIVATE compositor::core compositor_warnings)
add_test(NAME core_tests COMMAND core_tests)

add_executable(blur_tests blur_tests.cpp)
target_link_libraries(blur_tests PRIVATE compositor::core compositor_warnings)
add_test(NAME blur_tests COMMAND blur_tests)

add_executable(selection_tests selection_tests.cpp)
target_link_libraries(selection_tests PRIVATE compositor::core compositor_warnings)
add_test(NAME selection_tests COMMAND selection_tests)

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
endif()

add_executable(psd_tests psd_tests.cpp)
target_link_libraries(psd_tests PRIVATE compositor::core compositor_warnings)
add_test(NAME psd_tests COMMAND psd_tests)
add_executable(psd_writer_tests psd_writer_tests.cpp)
target_link_libraries(psd_writer_tests PRIVATE compositor::core compositor_warnings)
add_test(NAME psd_writer_tests COMMAND psd_writer_tests)

# Offline background-removal harness: runs the pipeline on a PNG and dumps every stage, or scores a
# directory of image/alpha pairs (SAD, MSE, MAD, Grad, Conn). Not a test; see docs/background-removal-review.md.
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
target_link_libraries(sut_tests PRIVATE compositor::core compositor_warnings ZLIB::ZLIB)
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

add_executable(clip_tests clip_tests.cpp)
target_link_libraries(clip_tests PRIVATE compositor::core compositor_warnings ZLIB::ZLIB)
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
target_link_libraries(formats_tests PRIVATE compositor::core compositor_warnings ZLIB::ZLIB)
add_test(NAME formats_tests COMMAND formats_tests)

add_executable(affinity_tests affinity_tests.cpp)
target_link_libraries(affinity_tests PRIVATE compositor::core compositor_warnings ZLIB::ZLIB)
add_test(NAME affinity_tests COMMAND affinity_tests)

# Not a test: dumps what the Affinity import makes of files (see the file's header).
add_executable(affinity_check affinity_check.cpp)
target_link_libraries(affinity_check PRIVATE compositor::core compositor_warnings)

add_executable(presets_tests presets_tests.cpp)
target_link_libraries(presets_tests PRIVATE compositor::core compositor_warnings)
add_test(NAME presets_tests COMMAND presets_tests)
