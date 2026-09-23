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
    add_executable(font_group_tests font_group_tests.cpp ${CMAKE_SOURCE_DIR}/src/app/FontGroups.cpp)
    target_include_directories(font_group_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/app)
    target_link_libraries(font_group_tests PRIVATE Qt6::Core compositor_warnings)
    add_test(NAME font_group_tests COMMAND font_group_tests)
endif()

add_executable(psd_tests psd_tests.cpp)
target_link_libraries(psd_tests PRIVATE compositor::core compositor_warnings)
add_test(NAME psd_tests COMMAND psd_tests)

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
