// A fixed canvas workload for counting the work of the CMYK and Lab display (docs/color-management.md, "The
// display grid"). Not a test and not timed: run it under `perf stat -e instructions:u` (or callgrind) and subtract the
// `none` run, which builds the same document and draws nothing.
//   build/tests/display_work <cmyk|lab> <exact|grid> <none|native|full|pan|zoom> [frames]
// full: the 3840 x 2160 document at 100 %; pan: the strips a pan by 64 device pixels renders (eight of 3840 x 64);
// zoom: the whole document at 50 %; native: the full frame at the document's layout, not transformed. "exact" draws
// through the Little CMS transform, "grid" through displayTransformBetween (its `none` run includes building it).
#include "nekophoto/colormgmt.h"
#include "nekophoto/colormodes.h"
#include "nekophoto/depth.h"
#include "nekophoto/document.h"
#include "nekophoto/render.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

using namespace nekophoto;

namespace {

/// One opaque layer of smooth ramps and some texture, every channel moving: inks or L, a and b over their range.
Document syntheticDocument(ColorMode mode, int w, int h) {
    Document doc(w, h);
    doc.colorMode = mode;
    doc.sampleType = SampleType::U16;
    const int channels = colorModeChannels(mode);
    auto image = std::make_shared<Image16>(w, h, channels);
    uint32_t seed = 12345;
    for (int y = 0; y < h; y++) {
        uint16_t* p = image->row(y);
        for (int x = 0; x < w; x++, p += channels) {
            seed = seed * 1664525u + 1013904223u;
            const uint32_t noise = (seed >> 24) & 63;
            const uint32_t u = uint32_t(x) * one16 / uint32_t(w - 1), v = uint32_t(y) * one16 / uint32_t(h - 1);
            const uint32_t values[4] = {u, v, (u + v) / 2, (one16 - u / 2 + noise * 16) % (one16 + 1)};
            for (int c = 0; c < channels - 1; c++) p[c] = uint16_t(std::min(one16, values[c] ^ noise));
            p[channels - 1] = uint16_t(one16);
        }
    }
    doc.layers.push_back(Layer(Asset::make(Image16Ptr(image), "ramps"), Point(0, 0)));
    return doc;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: display_work <cmyk|lab> <exact|grid> <none|native|full|pan|zoom> [frames]\n");
        return 2;
    }
    const ColorMode mode = std::strcmp(argv[1], "lab") == 0 ? ColorMode::Lab : ColorMode::CMYK;
    const bool grid = std::strcmp(argv[2], "grid") == 0;
    const std::string work = argv[3];
    const int frames = argc > 4 ? std::max(1, std::atoi(argv[4])) : 1;
    const int w = 3840, h = 2160;
    const Document doc = syntheticDocument(mode, w, h);
    const PixelFormat layout = pixelFormatFor(SampleType::U16, mode);
    const ConvertOptions options{RenderingIntent::RelativeColorimetric, true};
    const ColorTransformPtr display = grid ? displayTransformBetween(doc.profile, srgbProfile(), options, layout)
                                           : transformBetween(doc.profile, srgbProfile(), options, layout, PixelFormat::RGBA8);
    if (!display) { std::fprintf(stderr, "no display transform\n"); return 1; }
    Image out;
    uint64_t sum = 0;
    for (int f = 0; f < frames; f++) {
        if (work == "native") {
            // The full frame at the document's layout, before any display transform: what the frame costs without it.
            const AnyImage native = renderNative(doc);
            sum += native.u16()->row(h / 2)[w];
        } else if (work == "full" || work == "zoom") {
            RenderOptions o;
            o.display = display.get();
            if (work == "zoom") o.scale = 0.5;
            render(doc, o, out);
            sum += out.row(out.height() / 2)[out.width() * 2];
        } else if (work == "pan") {
            for (int strip = 0; strip < 8; strip++) {
                RenderOptions o;
                o.display = display.get();
                o.region = Rect(0, strip * 64, w, 64);
                render(doc, o, out);
                sum += out.row(0)[0];
            }
        }
    }
    std::printf("%s %s %s x%d: %llu\n", argv[1], argv[2], work.c_str(), frames, (unsigned long long)sum);
    return 0;
}
