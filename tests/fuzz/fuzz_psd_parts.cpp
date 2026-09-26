// libFuzzer target: each block parser on its own (the first byte picks one), so the fuzzer reaches their depths
// without first building a valid PSD around them.
#include "compositor/layerstyle.h"
#include "compositor/psd.h"
#include "compositor/psd_carry.h"
#include "compositor/smartfilter.h"
#include "compositor/smartobject.h"
#include "compositor/vectormask.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using namespace compositor;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) return 0;
    const uint8_t which = data[0];
    const std::vector<uint8_t> bytes(data + 1, data + size);
    std::string why;
    switch (which % 12) {
    case 0: (void)parseVectorMask(bytes, 640, 480); break;
    case 1: (void)readPhotoshopType(bytes.data(), bytes.size(), &why); break;
    case 2: (void)parsePsdPlacement("SoLd", bytes); break;
    case 3: (void)parsePsdPlacement("PlLd", bytes); break;
    case 4: (void)parseSmartFilterStack("SoLd", bytes); break;
    case 5: (void)findSmartFilterCache({{"FEid", bytes}}, "11111111-2222-3333-4444-555555555555"); break;
    case 6: (void)parsePsdLinkBlock(bytes); break;
    case 7: (void)parseMaskParameters(bytes); break;
    case 8: (void)parseFillGradient(bytes); (void)parseFillPattern(bytes); break;
    case 9: (void)parseSmartObjectSource(bytes); (void)parseSmartObjectInstance(bytes); break;
    case 10: (void)parsePsdLayerCarry(bytes); (void)parsePsdDocumentCarry(bytes); break;
    case 11: {
        // A placement parsed, then patched and warped as an edit would.
        if (auto p = parsePsdPlacement("SoLd", bytes)) {
            (void)patchPsdPlacement("SoLd", bytes, p->quad);
            (void)warpPsdPlacement("SoLd", bytes, identityWarpMesh(0, 0, 10, 10, 4, 4), p->quad);
            (void)replaceSmartFilterRecords(bytes, {});
        }
        break;
    }
    }
    return 0;
}
