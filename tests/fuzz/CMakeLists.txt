# libFuzzer targets for the file parsers (docs/fuzzing.md). Clang only; configure a separate build with
# -DCOMPOSITOR_BUILD_FUZZERS=ON and the sanitizer flags there, so the core is instrumented too.
foreach(target fuzz_psd fuzz_psd_parts)
    add_executable(${target} ${target}.cpp)
    target_link_libraries(${target} PRIVATE compositor::core)
    target_compile_options(${target} PRIVATE -fsanitize=fuzzer)
    target_link_options(${target} PRIVATE -fsanitize=fuzzer)
endforeach()
