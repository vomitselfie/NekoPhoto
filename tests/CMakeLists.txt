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

add_executable(kernel_tests kernel_tests.cpp)
target_link_libraries(kernel_tests PRIVATE compositor::core compositor::pixels compositor_warnings)
add_test(NAME kernel_tests COMMAND kernel_tests)

add_executable(compositor_bench bench.cpp)
target_link_libraries(compositor_bench PRIVATE compositor::core compositor_warnings)

add_executable(golden_tests golden_tests.cpp)
target_link_libraries(golden_tests PRIVATE compositor::core compositor_warnings)
target_compile_definitions(golden_tests PRIVATE GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")
add_test(NAME golden_tests COMMAND golden_tests)
