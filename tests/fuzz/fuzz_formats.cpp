// libFuzzer targets for the small format readers; FUZZ_FORMAT picks one per executable.
#include "compositor/adjustments.h"
#include "compositor/affinity.h"
#include "compositor/aseprite.h"
#include "compositor/gif.h"
#include "compositor/ico.h"
#include "compositor/presets.h"
#include "compositor/svg.h"
#include "compositor/tga.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using namespace compositor;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::vector<uint8_t> bytes(data, data + size);
    std::string error;
    std::vector<std::string> notes;
#if FUZZ_FORMAT == 0
    (void)isTgaData(data, size);
    (void)decodeTgaImage(data, size, &error);
#elif FUZZ_FORMAT == 1
    (void)importIcoBytes(bytes, &error);
#elif FUZZ_FORMAT == 2
    (void)gifFrameCount(bytes);
    (void)importGifBytes(bytes, &error);
#elif FUZZ_FORMAT == 3
    (void)importAsepriteBytes(bytes, &error);
#elif FUZZ_FORMAT == 4
    (void)importAffinityBytes(bytes, &error);
#elif FUZZ_FORMAT == 5
    if (auto svg = importSvg(bytes, &error)) (void)writeSvg(svg->document);
#elif FUZZ_FORMAT == 6
    if (size < 1) return 0;
    const std::vector<uint8_t> rest(data + 1, data + size);
    switch (data[0] % 3) {
    case 0: if (auto p = readPat(rest, &error, &notes)) for (const auto& x : *p) (void)decodePattern(x); break;
    case 1: (void)readAsl(rest, &error, &notes); break;
    case 2: (void)readGrd(rest, &error, &notes); break;
    }
#elif FUZZ_FORMAT == 7
    if (size < 1) return 0;
    ColorLookupSettings s;
    const std::string text(reinterpret_cast<const char*>(data + 1), size - 1);
    switch (data[0] % 3) {
    case 0: s.format = "cube"; s.data = text; break;
    case 1: s.format = "3dl"; s.data = text; break;
    case 2: s.format = "icc"; s.data = toBase64(std::vector<uint8_t>(data + 1, data + size)); break;
    }
    s.dither = data[0] & 4;
    Image image(8, 8);
    for (int y = 0; y < 8; y++) for (int x = 0; x < 32; x++) image.row(y)[x] = uint8_t(x * 8 + y);
    if (colorLookupReadable(s)) applyColorLookup(image, s);
#elif FUZZ_FORMAT == 8
    (void)importSvg(bytes, &error);
#endif
    return 0;
}
