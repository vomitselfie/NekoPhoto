// libFuzzer target: whole files through the PSD/PSB importer (smart objects nested, text, styles, masks).
#include "compositor/psd.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string error;
    auto imported = compositor::importPsdBytes(std::vector<uint8_t>(data, data + size), &error);
    (void)imported;
    return 0;
}
