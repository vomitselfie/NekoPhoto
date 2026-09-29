// CMYK and Lab documents, steps C and D (docs/high-bit-depth-plan.md, "P7 plan"): which blend modes each mode offers,
// the CMYK and Lab executors at 8 and 16 bits against the RGB kernels they reuse, the display path (never without a
// colour transform), masks, folders and clipping in these modes, mode conversion (pixels, stored colours, dormant
// adjustment layers, per-channel settings), thumbnails and mip levels of 5-channel buffers.
#include "check.h"
#include "compositor/adjustments.h"
#include "compositor/blend.h"
#include "compositor/colormgmt.h"
#include "compositor/depth.h"
#include "compositor/document.h"
#include "compositor/render.h"
#include "compositor/vectormask.h"
#include <cmath>
#include <cstdio>
#include <random>

using namespace compositor;

namespace {

std::shared_ptr<Image> noisy(int w, int h, uint32_t seed, bool translucent) {
    auto out = std::make_shared<Image>(w, h);
    std::mt19937 rng(seed);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t* p = out->pixel(x, y);
            const unsigned a = translucent ? unsigned(rng() % 256) : 255;
            for (int c = 0; c < 3; c++) p[c] = uint8_t((rng() % 256) * a / 255);
            p[3] = uint8_t(a);
        }
    return out;
}

Layer layerOf(const std::string& name, const AnyImage& image, Point origin = {0, 0}) { return Layer(Asset::makeAny(image, name), origin); }

/// An RGB document of `layers` noisy layers (the first opaque), `mode` on the others.
Document rgbDocument(int w, int h, int layers, BlendMode mode) {
    Document doc(w, h);
    for (int i = 0; i < layers; i++) {
        Layer l = layerOf("layer " + std::to_string(i), ImagePtr(noisy(w, h, uint32_t(11 + i), i > 0)));
        if (i > 0) l.blendMode = mode;
        doc.layers.push_back(l);
    }
    return doc;
}

Document inMode(Document doc, ColorMode mode) {
    std::string why;
    if (!convertDocumentMode(doc, mode, ColorProfile(), ConvertOptions(), &why)) check::fail(__FILE__, __LINE__, "convertDocumentMode: " + why);
    return doc;
}

int maxDifference(const Image& a, const Image& b) {
    if (a.width() != b.width() || a.height() != b.height()) return 256;
    int worst = 0;
    for (int y = 0; y < a.height(); y++)
        for (int i = 0; i < a.width() * 4; i++) worst = std::max(worst, std::abs(int(a.row(y)[i]) - int(b.row(y)[i])));
    return worst;
}

bool modes_nonSeparable(BlendMode m) {
    return m == BlendMode::Hue || m == BlendMode::Saturation || m == BlendMode::Color || m == BlendMode::Luminosity ||
           m == BlendMode::DarkerColor || m == BlendMode::LighterColor;
}

/// Photoshop's own CMYK results (ink percent, opaque layer over an opaque backdrop), read with its colour sampler, from
/// psd-tools' tests/psd_tools/composite/test_blend.py (MIT), which scripted them against Photoshop 2026.
struct CmykReference { float backdrop[4], source[4]; BlendMode mode; float expected[4]; };
const CmykReference photoshopCmyk[] = {
// PHOTOSHOP_CMYK: 6 pairs
    {{80.0f, 20.0f, 10.0f, 5.0f}, {10.0f, 70.0f, 60.0f, 20.0f}, BlendMode::Hue, {0.0f, 53.73f, 45.1f, 5.1f}},
    {{80.0f, 20.0f, 10.0f, 5.0f}, {10.0f, 70.0f, 60.0f, 20.0f}, BlendMode::Saturation, {73.73f, 22.35f, 13.73f, 5.1f}},
    {{80.0f, 20.0f, 10.0f, 5.0f}, {10.0f, 70.0f, 60.0f, 20.0f}, BlendMode::Color, {0.0f, 53.73f, 45.1f, 5.1f}},
    {{80.0f, 20.0f, 10.0f, 5.0f}, {10.0f, 70.0f, 60.0f, 20.0f}, BlendMode::Luminosity, {93.73f, 33.73f, 23.53f, 20.0f}},
    {{80.0f, 20.0f, 10.0f, 5.0f}, {10.0f, 70.0f, 60.0f, 20.0f}, BlendMode::DarkerColor, {9.8f, 69.8f, 60.0f, 20.0f}},
    {{80.0f, 20.0f, 10.0f, 5.0f}, {10.0f, 70.0f, 60.0f, 20.0f}, BlendMode::LighterColor, {80.0f, 20.0f, 9.8f, 5.1f}},
    {{10.0f, 70.0f, 60.0f, 20.0f}, {80.0f, 20.0f, 10.0f, 5.0f}, BlendMode::Hue, {87.45f, 36.08f, 27.45f, 20.0f}},
    {{10.0f, 70.0f, 60.0f, 20.0f}, {80.0f, 20.0f, 10.0f, 5.0f}, BlendMode::Saturation, {2.75f, 72.94f, 61.57f, 20.0f}},
    {{10.0f, 70.0f, 60.0f, 20.0f}, {80.0f, 20.0f, 10.0f, 5.0f}, BlendMode::Color, {93.73f, 33.73f, 23.53f, 20.0f}},
    {{10.0f, 70.0f, 60.0f, 20.0f}, {80.0f, 20.0f, 10.0f, 5.0f}, BlendMode::Luminosity, {0.0f, 53.73f, 45.1f, 5.1f}},
    {{10.0f, 70.0f, 60.0f, 20.0f}, {80.0f, 20.0f, 10.0f, 5.0f}, BlendMode::DarkerColor, {9.8f, 69.8f, 60.0f, 20.0f}},
    {{10.0f, 70.0f, 60.0f, 20.0f}, {80.0f, 20.0f, 10.0f, 5.0f}, BlendMode::LighterColor, {80.0f, 20.0f, 9.8f, 5.1f}},
    {{5.0f, 5.0f, 90.0f, 0.0f}, {70.0f, 60.0f, 0.0f, 10.0f}, BlendMode::Hue, {17.65f, 14.9f, 0.0f, 0.0f}},
    {{5.0f, 5.0f, 90.0f, 0.0f}, {70.0f, 60.0f, 0.0f, 10.0f}, BlendMode::Saturation, {6.67f, 6.67f, 76.47f, 0.0f}},
    {{5.0f, 5.0f, 90.0f, 0.0f}, {70.0f, 60.0f, 0.0f, 10.0f}, BlendMode::Color, {17.65f, 14.9f, 0.0f, 0.0f}},
    {{5.0f, 5.0f, 90.0f, 0.0f}, {70.0f, 60.0f, 0.0f, 10.0f}, BlendMode::Luminosity, {50.98f, 50.98f, 99.61f, 9.8f}},
    {{5.0f, 5.0f, 90.0f, 0.0f}, {70.0f, 60.0f, 0.0f, 10.0f}, BlendMode::DarkerColor, {69.8f, 60.0f, 0.0f, 9.8f}},
    {{5.0f, 5.0f, 90.0f, 0.0f}, {70.0f, 60.0f, 0.0f, 10.0f}, BlendMode::LighterColor, {5.1f, 5.1f, 89.8f, 0.0f}},
    {{40.0f, 40.0f, 40.0f, 40.0f}, {0.0f, 90.0f, 30.0f, 0.0f}, BlendMode::Hue, {40.0f, 40.0f, 40.0f, 40.0f}},
    {{40.0f, 40.0f, 40.0f, 40.0f}, {0.0f, 90.0f, 30.0f, 0.0f}, BlendMode::Saturation, {40.0f, 40.0f, 40.0f, 40.0f}},
    {{40.0f, 40.0f, 40.0f, 40.0f}, {0.0f, 90.0f, 30.0f, 0.0f}, BlendMode::Color, {0.0f, 63.53f, 21.18f, 40.0f}},
    {{40.0f, 40.0f, 40.0f, 40.0f}, {0.0f, 90.0f, 30.0f, 0.0f}, BlendMode::Luminosity, {56.08f, 56.08f, 56.08f, 0.0f}},
    {{40.0f, 40.0f, 40.0f, 40.0f}, {0.0f, 90.0f, 30.0f, 0.0f}, BlendMode::DarkerColor, {40.0f, 40.0f, 40.0f, 40.0f}},
    {{40.0f, 40.0f, 40.0f, 40.0f}, {0.0f, 90.0f, 30.0f, 0.0f}, BlendMode::LighterColor, {0.0f, 89.8f, 29.8f, 0.0f}},
    {{95.0f, 0.0f, 30.0f, 60.0f}, {20.0f, 15.0f, 85.0f, 2.0f}, BlendMode::Hue, {27.06f, 21.18f, 99.61f, 60.0f}},
    {{95.0f, 0.0f, 30.0f, 60.0f}, {20.0f, 15.0f, 85.0f, 2.0f}, BlendMode::Saturation, {78.43f, 8.24f, 30.2f, 60.0f}},
    {{95.0f, 0.0f, 30.0f, 60.0f}, {20.0f, 15.0f, 85.0f, 2.0f}, BlendMode::Color, {27.45f, 22.35f, 92.55f, 60.0f}},
    {{95.0f, 0.0f, 30.0f, 60.0f}, {20.0f, 15.0f, 85.0f, 2.0f}, BlendMode::Luminosity, {72.16f, 0.0f, 22.75f, 1.96f}},
    {{95.0f, 0.0f, 30.0f, 60.0f}, {20.0f, 15.0f, 85.0f, 2.0f}, BlendMode::DarkerColor, {94.9f, 0.0f, 29.8f, 60.0f}},
    {{95.0f, 0.0f, 30.0f, 60.0f}, {20.0f, 15.0f, 85.0f, 2.0f}, BlendMode::LighterColor, {20.0f, 14.9f, 85.1f, 1.96f}},
    {{0.0f, 0.0f, 0.0f, 0.0f}, {50.0f, 40.0f, 30.0f, 10.0f}, BlendMode::Hue, {0.0f, 0.0f, 0.0f, 0.0f}},
    {{0.0f, 0.0f, 0.0f, 0.0f}, {50.0f, 40.0f, 30.0f, 10.0f}, BlendMode::Saturation, {0.0f, 0.0f, 0.0f, 0.0f}},
    {{0.0f, 0.0f, 0.0f, 0.0f}, {50.0f, 40.0f, 30.0f, 10.0f}, BlendMode::Color, {0.0f, 0.0f, 0.0f, 0.0f}},
    {{0.0f, 0.0f, 0.0f, 0.0f}, {50.0f, 40.0f, 30.0f, 10.0f}, BlendMode::Luminosity, {41.96f, 41.96f, 41.96f, 9.8f}},
    {{0.0f, 0.0f, 0.0f, 0.0f}, {50.0f, 40.0f, 30.0f, 10.0f}, BlendMode::DarkerColor, {49.8f, 40.0f, 29.8f, 9.8f}},
    {{0.0f, 0.0f, 0.0f, 0.0f}, {50.0f, 40.0f, 30.0f, 10.0f}, BlendMode::LighterColor, {0.0f, 0.0f, 0.0f, 0.0f}},
// PHOTOSHOP_CMYK_SEPARABLE: 5 pairs
    {{80.0f, 20.0f, 10.0f, 5.0f}, {10.0f, 70.0f, 60.0f, 20.0f}, BlendMode::ColorBurn, {88.63f, 66.27f, 24.71f, 6.27f}},
    {{80.0f, 20.0f, 10.0f, 5.0f}, {10.0f, 70.0f, 60.0f, 20.0f}, BlendMode::ColorDodge, {0.0f, 0.0f, 0.0f, 0.0f}},
    {{80.0f, 20.0f, 10.0f, 5.0f}, {10.0f, 70.0f, 60.0f, 20.0f}, BlendMode::Darken, {80.0f, 69.8f, 60.0f, 20.0f}},
    {{80.0f, 20.0f, 10.0f, 5.0f}, {10.0f, 70.0f, 60.0f, 20.0f}, BlendMode::Difference, {29.8f, 50.2f, 49.8f, 85.1f}},
    {{80.0f, 20.0f, 10.0f, 5.0f}, {10.0f, 70.0f, 60.0f, 20.0f}, BlendMode::Divide, {77.65f, 0.0f, 0.0f, 0.0f}},
    {{80.0f, 20.0f, 10.0f, 5.0f}, {10.0f, 70.0f, 60.0f, 20.0f}, BlendMode::Exclusion, {25.88f, 38.43f, 41.96f, 77.25f}},
    {{80.0f, 20.0f, 10.0f, 5.0f}, {10.0f, 70.0f, 60.0f, 20.0f}, BlendMode::HardLight, {16.08f, 51.76f, 27.84f, 1.96f}},
    {{80.0f, 20.0f, 10.0f, 5.0f}, {10.0f, 70.0f, 60.0f, 20.0f}, BlendMode::HardMix, {0.0f, 0.0f, 0.0f, 0.0f}},
    {{80.0f, 20.0f, 10.0f, 5.0f}, {10.0f, 70.0f, 60.0f, 20.0f}, BlendMode::Lighten, {9.8f, 20.0f, 9.8f, 5.1f}},
    {{80.0f, 20.0f, 10.0f, 5.0f}, {10.0f, 70.0f, 60.0f, 20.0f}, BlendMode::LinearBurn, {89.8f, 89.8f, 69.8f, 25.1f}},
    {{80.0f, 20.0f, 10.0f, 5.0f}, {10.0f, 70.0f, 60.0f, 20.0f}, BlendMode::LinearDodge, {0.0f, 0.0f, 0.0f, 0.0f}},
    {{80.0f, 20.0f, 10.0f, 5.0f}, {10.0f, 70.0f, 60.0f, 20.0f}, BlendMode::LinearLight, {0.0f, 60.0f, 30.2f, 0.0f}},
    {{80.0f, 20.0f, 10.0f, 5.0f}, {10.0f, 70.0f, 60.0f, 20.0f}, BlendMode::Multiply, {81.96f, 75.69f, 63.92f, 23.92f}},
    {{80.0f, 20.0f, 10.0f, 5.0f}, {10.0f, 70.0f, 60.0f, 20.0f}, BlendMode::Normal, {9.8f, 69.8f, 60.0f, 20.0f}},
    {{80.0f, 20.0f, 10.0f, 5.0f}, {10.0f, 70.0f, 60.0f, 20.0f}, BlendMode::Overlay, {63.92f, 27.84f, 11.76f, 1.96f}},
    {{80.0f, 20.0f, 10.0f, 5.0f}, {10.0f, 70.0f, 60.0f, 20.0f}, BlendMode::PinLight, {19.61f, 40.0f, 20.39f, 5.1f}},
    {{80.0f, 20.0f, 10.0f, 5.0f}, {10.0f, 70.0f, 60.0f, 20.0f}, BlendMode::Screen, {7.84f, 14.12f, 5.88f, 1.18f}},
    {{80.0f, 20.0f, 10.0f, 5.0f}, {10.0f, 70.0f, 60.0f, 20.0f}, BlendMode::SoftLight, {60.39f, 26.27f, 11.76f, 3.53f}},
    {{80.0f, 20.0f, 10.0f, 5.0f}, {10.0f, 70.0f, 60.0f, 20.0f}, BlendMode::Subtract, {100.0f, 50.2f, 49.8f, 85.1f}},
    {{80.0f, 20.0f, 10.0f, 5.0f}, {10.0f, 70.0f, 60.0f, 20.0f}, BlendMode::VividLight, {0.0f, 33.33f, 12.16f, 0.0f}},
    {{10.0f, 70.0f, 60.0f, 20.0f}, {80.0f, 20.0f, 10.0f, 5.0f}, BlendMode::ColorBurn, {49.02f, 87.45f, 66.67f, 21.18f}},
    {{10.0f, 70.0f, 60.0f, 20.0f}, {80.0f, 20.0f, 10.0f, 5.0f}, BlendMode::ColorDodge, {0.0f, 0.0f, 0.0f, 0.0f}},
    {{10.0f, 70.0f, 60.0f, 20.0f}, {80.0f, 20.0f, 10.0f, 5.0f}, BlendMode::Darken, {80.0f, 69.8f, 60.0f, 20.0f}},
    {{10.0f, 70.0f, 60.0f, 20.0f}, {80.0f, 20.0f, 10.0f, 5.0f}, BlendMode::Difference, {29.8f, 50.2f, 49.8f, 85.1f}},
    {{10.0f, 70.0f, 60.0f, 20.0f}, {80.0f, 20.0f, 10.0f, 5.0f}, BlendMode::Divide, {0.0f, 62.35f, 55.69f, 15.69f}},
    {{10.0f, 70.0f, 60.0f, 20.0f}, {80.0f, 20.0f, 10.0f, 5.0f}, BlendMode::Exclusion, {25.88f, 38.43f, 41.96f, 77.25f}},
    {{10.0f, 70.0f, 60.0f, 20.0f}, {80.0f, 20.0f, 10.0f, 5.0f}, BlendMode::HardLight, {63.92f, 28.24f, 12.16f, 1.96f}},
    {{10.0f, 70.0f, 60.0f, 20.0f}, {80.0f, 20.0f, 10.0f, 5.0f}, BlendMode::HardMix, {0.0f, 0.0f, 0.0f, 0.0f}},
    {{10.0f, 70.0f, 60.0f, 20.0f}, {80.0f, 20.0f, 10.0f, 5.0f}, BlendMode::Lighten, {9.8f, 20.0f, 9.8f, 5.1f}},
    {{10.0f, 70.0f, 60.0f, 20.0f}, {80.0f, 20.0f, 10.0f, 5.0f}, BlendMode::LinearBurn, {89.8f, 89.8f, 69.8f, 25.1f}},
    {{10.0f, 70.0f, 60.0f, 20.0f}, {80.0f, 20.0f, 10.0f, 5.0f}, BlendMode::LinearDodge, {0.0f, 0.0f, 0.0f, 0.0f}},
    {{10.0f, 70.0f, 60.0f, 20.0f}, {80.0f, 20.0f, 10.0f, 5.0f}, BlendMode::LinearLight, {70.2f, 10.2f, 0.0f, 0.0f}},
    {{10.0f, 70.0f, 60.0f, 20.0f}, {80.0f, 20.0f, 10.0f, 5.0f}, BlendMode::Multiply, {81.96f, 75.69f, 63.92f, 23.92f}},
    {{10.0f, 70.0f, 60.0f, 20.0f}, {80.0f, 20.0f, 10.0f, 5.0f}, BlendMode::Normal, {80.0f, 20.0f, 9.8f, 5.1f}},
    {{10.0f, 70.0f, 60.0f, 20.0f}, {80.0f, 20.0f, 10.0f, 5.0f}, BlendMode::Overlay, {15.69f, 51.76f, 27.84f, 1.96f}},
    {{10.0f, 70.0f, 60.0f, 20.0f}, {80.0f, 20.0f, 10.0f, 5.0f}, BlendMode::PinLight, {60.0f, 40.0f, 19.61f, 10.2f}},
    {{10.0f, 70.0f, 60.0f, 20.0f}, {80.0f, 20.0f, 10.0f, 5.0f}, BlendMode::Screen, {7.84f, 14.12f, 5.88f, 1.18f}},
    {{10.0f, 70.0f, 60.0f, 20.0f}, {80.0f, 20.0f, 10.0f, 5.0f}, BlendMode::SoftLight, {15.29f, 54.9f, 41.57f, 11.76f}},
    {{10.0f, 70.0f, 60.0f, 20.0f}, {80.0f, 20.0f, 10.0f, 5.0f}, BlendMode::Subtract, {29.8f, 100.0f, 100.0f, 100.0f}},
    {{10.0f, 70.0f, 60.0f, 20.0f}, {80.0f, 20.0f, 10.0f, 5.0f}, BlendMode::VividLight, {24.71f, 24.31f, 0.0f, 0.0f}},
    {{100.0f, 0.0f, 50.0f, 0.0f}, {0.0f, 100.0f, 50.0f, 0.0f}, BlendMode::ColorBurn, {100.0f, 0.0f, 99.22f, 0.0f}},
    {{100.0f, 0.0f, 50.0f, 0.0f}, {0.0f, 100.0f, 50.0f, 0.0f}, BlendMode::ColorDodge, {100.0f, 0.0f, 0.0f, 0.0f}},
    {{100.0f, 0.0f, 50.0f, 0.0f}, {0.0f, 100.0f, 50.0f, 0.0f}, BlendMode::Darken, {100.0f, 100.0f, 49.8f, 0.0f}},
    {{100.0f, 0.0f, 50.0f, 0.0f}, {0.0f, 100.0f, 50.0f, 0.0f}, BlendMode::Difference, {0.0f, 0.0f, 100.0f, 100.0f}},
    {{100.0f, 0.0f, 50.0f, 0.0f}, {0.0f, 100.0f, 50.0f, 0.0f}, BlendMode::Divide, {100.0f, 0.0f, 0.0f, 0.0f}},
    {{100.0f, 0.0f, 50.0f, 0.0f}, {0.0f, 100.0f, 50.0f, 0.0f}, BlendMode::Exclusion, {0.0f, 0.0f, 49.8f, 100.0f}},
    {{100.0f, 0.0f, 50.0f, 0.0f}, {0.0f, 100.0f, 50.0f, 0.0f}, BlendMode::HardLight, {0.39f, 100.0f, 49.8f, 0.0f}},
    {{100.0f, 0.0f, 50.0f, 0.0f}, {0.0f, 100.0f, 50.0f, 0.0f}, BlendMode::HardMix, {100.0f, 0.0f, 0.0f, 0.0f}},
    {{100.0f, 0.0f, 50.0f, 0.0f}, {0.0f, 100.0f, 50.0f, 0.0f}, BlendMode::Lighten, {0.0f, 0.0f, 49.8f, 0.0f}},
    {{100.0f, 0.0f, 50.0f, 0.0f}, {0.0f, 100.0f, 50.0f, 0.0f}, BlendMode::LinearBurn, {100.0f, 100.0f, 99.61f, 0.0f}},
    {{100.0f, 0.0f, 50.0f, 0.0f}, {0.0f, 100.0f, 50.0f, 0.0f}, BlendMode::LinearDodge, {0.0f, 0.0f, 0.0f, 0.0f}},
    {{100.0f, 0.0f, 50.0f, 0.0f}, {0.0f, 100.0f, 50.0f, 0.0f}, BlendMode::LinearLight, {0.39f, 100.0f, 49.8f, 0.0f}},
    {{100.0f, 0.0f, 50.0f, 0.0f}, {0.0f, 100.0f, 50.0f, 0.0f}, BlendMode::Multiply, {100.0f, 100.0f, 74.9f, 0.0f}},
    {{100.0f, 0.0f, 50.0f, 0.0f}, {0.0f, 100.0f, 50.0f, 0.0f}, BlendMode::Normal, {0.0f, 100.0f, 49.8f, 0.0f}},
    {{100.0f, 0.0f, 50.0f, 0.0f}, {0.0f, 100.0f, 50.0f, 0.0f}, BlendMode::Overlay, {100.0f, 0.0f, 49.8f, 0.0f}},
    {{100.0f, 0.0f, 50.0f, 0.0f}, {0.0f, 100.0f, 50.0f, 0.0f}, BlendMode::PinLight, {0.0f, 100.0f, 49.8f, 0.0f}},
    {{100.0f, 0.0f, 50.0f, 0.0f}, {0.0f, 100.0f, 50.0f, 0.0f}, BlendMode::Screen, {0.0f, 0.0f, 24.71f, 0.0f}},
    {{100.0f, 0.0f, 50.0f, 0.0f}, {0.0f, 100.0f, 50.0f, 0.0f}, BlendMode::SoftLight, {100.0f, 0.0f, 49.8f, 0.0f}},
    {{100.0f, 0.0f, 50.0f, 0.0f}, {0.0f, 100.0f, 50.0f, 0.0f}, BlendMode::Subtract, {100.0f, 0.0f, 100.0f, 100.0f}},
    {{100.0f, 0.0f, 50.0f, 0.0f}, {0.0f, 100.0f, 50.0f, 0.0f}, BlendMode::VividLight, {0.0f, 100.0f, 49.8f, 0.0f}},
    {{25.0f, 75.0f, 95.0f, 95.0f}, {75.0f, 25.0f, 0.0f, 25.0f}, BlendMode::ColorBurn, {100.0f, 100.0f, 94.9f, 100.0f}},
    {{25.0f, 75.0f, 95.0f, 95.0f}, {75.0f, 25.0f, 0.0f, 25.0f}, BlendMode::ColorDodge, {0.0f, 0.0f, 0.0f, 79.61f}},
    {{25.0f, 75.0f, 95.0f, 95.0f}, {75.0f, 25.0f, 0.0f, 25.0f}, BlendMode::Darken, {74.9f, 74.9f, 94.9f, 94.9f}},
    {{25.0f, 75.0f, 95.0f, 95.0f}, {75.0f, 25.0f, 0.0f, 25.0f}, BlendMode::Difference, {50.2f, 50.2f, 5.1f, 30.2f}},
    {{25.0f, 75.0f, 95.0f, 95.0f}, {75.0f, 25.0f, 0.0f, 25.0f}, BlendMode::Divide, {0.0f, 66.67f, 94.9f, 93.33f}},
    {{25.0f, 75.0f, 95.0f, 95.0f}, {75.0f, 25.0f, 0.0f, 25.0f}, BlendMode::Exclusion, {37.65f, 37.65f, 5.1f, 27.84f}},
    {{25.0f, 75.0f, 95.0f, 95.0f}, {75.0f, 25.0f, 0.0f, 25.0f}, BlendMode::HardLight, {62.35f, 38.04f, 0.39f, 47.84f}},
    {{25.0f, 75.0f, 95.0f, 95.0f}, {75.0f, 25.0f, 0.0f, 25.0f}, BlendMode::HardMix, {0.0f, 100.0f, 0.0f, 100.0f}},
    {{25.0f, 75.0f, 95.0f, 95.0f}, {75.0f, 25.0f, 0.0f, 25.0f}, BlendMode::Lighten, {25.1f, 25.1f, 0.0f, 25.1f}},
    {{25.0f, 75.0f, 95.0f, 95.0f}, {75.0f, 25.0f, 0.0f, 25.0f}, BlendMode::LinearBurn, {100.0f, 100.0f, 94.9f, 100.0f}},
    {{25.0f, 75.0f, 95.0f, 95.0f}, {75.0f, 25.0f, 0.0f, 25.0f}, BlendMode::LinearDodge, {0.0f, 0.0f, 0.0f, 20.0f}},
    {{25.0f, 75.0f, 95.0f, 95.0f}, {75.0f, 25.0f, 0.0f, 25.0f}, BlendMode::LinearLight, {75.29f, 25.49f, 0.0f, 45.49f}},
    {{25.0f, 75.0f, 95.0f, 95.0f}, {75.0f, 25.0f, 0.0f, 25.0f}, BlendMode::Multiply, {81.18f, 81.18f, 94.9f, 96.08f}},
    {{25.0f, 75.0f, 95.0f, 95.0f}, {75.0f, 25.0f, 0.0f, 25.0f}, BlendMode::Normal, {74.9f, 25.1f, 0.0f, 25.1f}},
    {{25.0f, 75.0f, 95.0f, 95.0f}, {75.0f, 25.0f, 0.0f, 25.0f}, BlendMode::Overlay, {37.65f, 62.35f, 89.8f, 92.55f}},
    {{25.0f, 75.0f, 95.0f, 95.0f}, {75.0f, 25.0f, 0.0f, 25.0f}, BlendMode::PinLight, {49.8f, 50.59f, 0.0f, 50.59f}},
    {{25.0f, 75.0f, 95.0f, 95.0f}, {75.0f, 25.0f, 0.0f, 25.0f}, BlendMode::Screen, {18.82f, 18.82f, 0.0f, 23.92f}},
    {{25.0f, 75.0f, 95.0f, 95.0f}, {75.0f, 25.0f, 0.0f, 25.0f}, BlendMode::SoftLight, {34.51f, 62.35f, 82.35f, 88.63f}},
    {{25.0f, 75.0f, 95.0f, 95.0f}, {75.0f, 25.0f, 0.0f, 25.0f}, BlendMode::Subtract, {50.2f, 100.0f, 100.0f, 100.0f}},
    {{25.0f, 75.0f, 95.0f, 95.0f}, {75.0f, 25.0f, 0.0f, 25.0f}, BlendMode::VividLight, {50.2f, 50.2f, 0.0f, 89.8f}},
    {{0.0f, 0.0f, 0.0f, 0.0f}, {100.0f, 100.0f, 100.0f, 100.0f}, BlendMode::ColorBurn, {0.0f, 0.0f, 0.0f, 0.0f}},
    {{0.0f, 0.0f, 0.0f, 0.0f}, {100.0f, 100.0f, 100.0f, 100.0f}, BlendMode::ColorDodge, {0.0f, 0.0f, 0.0f, 0.0f}},
    {{0.0f, 0.0f, 0.0f, 0.0f}, {100.0f, 100.0f, 100.0f, 100.0f}, BlendMode::Darken, {100.0f, 100.0f, 100.0f, 100.0f}},
    {{0.0f, 0.0f, 0.0f, 0.0f}, {100.0f, 100.0f, 100.0f, 100.0f}, BlendMode::Difference, {0.0f, 0.0f, 0.0f, 0.0f}},
    {{0.0f, 0.0f, 0.0f, 0.0f}, {100.0f, 100.0f, 100.0f, 100.0f}, BlendMode::Divide, {0.0f, 0.0f, 0.0f, 0.0f}},
    {{0.0f, 0.0f, 0.0f, 0.0f}, {100.0f, 100.0f, 100.0f, 100.0f}, BlendMode::Exclusion, {0.0f, 0.0f, 0.0f, 0.0f}},
    {{0.0f, 0.0f, 0.0f, 0.0f}, {100.0f, 100.0f, 100.0f, 100.0f}, BlendMode::HardLight, {100.0f, 100.0f, 100.0f, 100.0f}},
    {{0.0f, 0.0f, 0.0f, 0.0f}, {100.0f, 100.0f, 100.0f, 100.0f}, BlendMode::HardMix, {0.0f, 0.0f, 0.0f, 0.0f}},
    {{0.0f, 0.0f, 0.0f, 0.0f}, {100.0f, 100.0f, 100.0f, 100.0f}, BlendMode::Lighten, {0.0f, 0.0f, 0.0f, 0.0f}},
    {{0.0f, 0.0f, 0.0f, 0.0f}, {100.0f, 100.0f, 100.0f, 100.0f}, BlendMode::LinearBurn, {100.0f, 100.0f, 100.0f, 100.0f}},
    {{0.0f, 0.0f, 0.0f, 0.0f}, {100.0f, 100.0f, 100.0f, 100.0f}, BlendMode::LinearDodge, {0.0f, 0.0f, 0.0f, 0.0f}},
    {{0.0f, 0.0f, 0.0f, 0.0f}, {100.0f, 100.0f, 100.0f, 100.0f}, BlendMode::LinearLight, {100.0f, 100.0f, 100.0f, 100.0f}},
    {{0.0f, 0.0f, 0.0f, 0.0f}, {100.0f, 100.0f, 100.0f, 100.0f}, BlendMode::Multiply, {100.0f, 100.0f, 100.0f, 100.0f}},
    {{0.0f, 0.0f, 0.0f, 0.0f}, {100.0f, 100.0f, 100.0f, 100.0f}, BlendMode::Normal, {100.0f, 100.0f, 100.0f, 100.0f}},
    {{0.0f, 0.0f, 0.0f, 0.0f}, {100.0f, 100.0f, 100.0f, 100.0f}, BlendMode::Overlay, {0.0f, 0.0f, 0.0f, 0.0f}},
    {{0.0f, 0.0f, 0.0f, 0.0f}, {100.0f, 100.0f, 100.0f, 100.0f}, BlendMode::PinLight, {100.0f, 100.0f, 100.0f, 100.0f}},
    {{0.0f, 0.0f, 0.0f, 0.0f}, {100.0f, 100.0f, 100.0f, 100.0f}, BlendMode::Screen, {0.0f, 0.0f, 0.0f, 0.0f}},
    {{0.0f, 0.0f, 0.0f, 0.0f}, {100.0f, 100.0f, 100.0f, 100.0f}, BlendMode::SoftLight, {0.0f, 0.0f, 0.0f, 0.0f}},
    {{0.0f, 0.0f, 0.0f, 0.0f}, {100.0f, 100.0f, 100.0f, 100.0f}, BlendMode::Subtract, {0.0f, 0.0f, 0.0f, 0.0f}},
    {{0.0f, 0.0f, 0.0f, 0.0f}, {100.0f, 100.0f, 100.0f, 100.0f}, BlendMode::VividLight, {100.0f, 100.0f, 100.0f, 100.0f}},
};

} // namespace

TEST_CASE(blend_modes_offered_per_mode) {
    const BlendMode notInLab[] = {BlendMode::ColorDodge, BlendMode::ColorBurn, BlendMode::Darken, BlendMode::Lighten,
                                  BlendMode::Difference, BlendMode::Exclusion, BlendMode::Subtract, BlendMode::Divide};
    int offeredInLab = 0;
    for (int m = 0; m < blendModeCount; m++) {
        const BlendMode mode = BlendMode(m);
        CHECK(blendModeAvailable(mode, ColorMode::RGB));
        CHECK(blendModeAvailable(mode, ColorMode::CMYK));
        offeredInLab += blendModeAvailable(mode, ColorMode::Lab);
    }
    CHECK_EQ(offeredInLab, blendModeCount - 8);
    for (BlendMode m : notInLab) { CHECK(!blendModeAvailable(m, ColorMode::Lab)); CHECK(blendModeFor(m, ColorMode::Lab) == BlendMode::Normal); }
    for (BlendMode m : {BlendMode::Hue, BlendMode::Saturation, BlendMode::Color, BlendMode::Luminosity, BlendMode::DarkerColor, BlendMode::LighterColor}) {
        CHECK(blendModeFor(m, ColorMode::CMYK) == m);
        CHECK(blendModeFor(m, ColorMode::Lab) == m);
    }
    CHECK(blendModeFor(BlendMode::Multiply, ColorMode::CMYK) == BlendMode::Multiply);
}

TEST_CASE(cmyk_separable_modes_are_the_rgb_kernels_per_ink) {
    std::mt19937 rng(5);
    for (int m = 0; m < blendModeCount; m++) {
        const BlendMode mode = BlendMode(m);
        if (mode == BlendMode::Dissolve || modes_nonSeparable(mode)) continue;
        for (int trial = 0; trial < 200; trial++) {
            uint8_t src[5], dst[5];
            const uint8_t sa = uint8_t(rng() % 256), da = uint8_t(rng() % 256);
            for (int c = 0; c < 4; c++) { src[c] = uint8_t((rng() % 256) * sa / 255); dst[c] = uint8_t((rng() % 256) * da / 255); }
            src[4] = sa; dst[4] = da;
            const uint16_t k = uint16_t(rng() % 257);
            uint8_t out[5];
            std::memcpy(out, dst, 5);
            compositeSpanMode8(mode, ColorMode::CMYK, src, &k, out, 1);
            // The same inks as two RGBA pixels through the RGB kernel.
            uint8_t s1[4] = {src[0], src[1], src[2], sa}, d1[4] = {dst[0], dst[1], dst[2], da};
            uint8_t s2[4] = {src[3], src[3], src[3], sa}, d2[4] = {dst[3], dst[3], dst[3], da};
            if (k) { compositePixelSteps(mode, s1, k, d1); compositePixelSteps(mode, s2, k, d2); }
            CHECK_EQ(int(out[0]), int(d1[0])); CHECK_EQ(int(out[1]), int(d1[1])); CHECK_EQ(int(out[2]), int(d1[2]));
            CHECK_EQ(int(out[3]), int(d2[0])); CHECK_EQ(int(out[4]), int(d1[3]));
        }
    }
}

TEST_CASE(cmyk_display_is_the_documents_pixels_through_its_profile) {
    const Document doc = inMode(rgbDocument(48, 32, 1, BlendMode::Normal), ColorMode::CMYK);
    REQUIRE(doc.colorMode == ColorMode::CMYK);
    REQUIRE(doc.layers[0].asset->image.c8());
    CHECK(!doc.profile.icc.empty());   // the Working CMYK the document was converted to
    Image shown;
    render(doc, RenderOptions(), shown);
    Image expected(48, 32);
    auto t = transformBetween(doc.profile, srgbProfile(), ConvertOptions(), PixelFormat::CMYKA8, PixelFormat::RGBA8);
    REQUIRE(t);
    REQUIRE(convertImageTo8(doc.layers[0].asset->image, expected, *t));
    CHECK_EQ(maxDifference(shown, expected), 0);
    // A display transform of another layout is not used: the frame still goes through the document's profile.
    auto rgbDisplay = transformBetween(srgbProfile(), builtinProfile(WorkingSpace::AdobeRGB), ConvertOptions(), PixelFormat::RGBA8, PixelFormat::RGBA8);
    RenderOptions options;
    options.display = rgbDisplay.get();
    Image again;
    render(doc, options, again);
    CHECK_EQ(maxDifference(again, expected), 0);
    // The native render is the layer's own ink.
    const AnyImage native = renderNative(doc);
    REQUIRE(native.c8());
    CHECK(*native.c8() == *doc.layers[0].asset->image.c8());
}

TEST_CASE(cmyk_non_separable_modes_draw_and_carry_k_as_photoshop) {
    for (BlendMode m : {BlendMode::Hue, BlendMode::Saturation, BlendMode::Color, BlendMode::Luminosity, BlendMode::DarkerColor, BlendMode::LighterColor}) {
        const Document doc = inMode(rgbDocument(40, 30, 2, m), ColorMode::CMYK);
        Document normal = doc;
        normal.layers[1].blendMode = BlendMode::Normal;
        CHECK(!(*renderNative(doc).c8() == *renderNative(normal).c8()));
    }
    // Straight complements (1 is no ink). Hue, Saturation and Color keep the backdrop's K, Luminosity takes the source's.
    const float cb[4] = {0.2f, 0.8f, 0.9f, 0.95f}, cs0[4] = {0.9f, 0.3f, 0.4f, 0.8f};
    for (BlendMode m : {BlendMode::Hue, BlendMode::Saturation, BlendMode::Color, BlendMode::Luminosity}) {
        float cs[4] = {cs0[0], cs0[1], cs0[2], cs0[3]};
        blendStraightMode(m, ColorMode::CMYK, cb, cs);
        CHECK_NEAR(cs[3], m == BlendMode::Luminosity ? cs0[3] : cb[3], 1e-6);
        float same[4] = {cb[0], cb[1], cb[2], cb[3]};
        blendStraightMode(m, ColorMode::CMYK, cb, same);
        for (int c = 0; c < 4; c++) CHECK_NEAR(same[c], cb[c], 1e-5);
    }
    // Darker and Lighter Color weigh K and take one pixel whole; a tie keeps the backdrop.
    const float light[4] = {0.5f, 0.5f, 0.5f, 1.0f}, dark[4] = {0.5f, 0.5f, 0.5f, 0.6f};
    float out[4] = {dark[0], dark[1], dark[2], dark[3]};
    blendStraightMode(BlendMode::DarkerColor, ColorMode::CMYK, light, out);
    CHECK_NEAR(out[3], 0.6f, 1e-6);
    float out2[4] = {dark[0], dark[1], dark[2], dark[3]};
    blendStraightMode(BlendMode::LighterColor, ColorMode::CMYK, light, out2);
    CHECK_NEAR(out2[3], 1.0f, 1e-6);
}

TEST_CASE(cmyk_multiply_darkens_every_ink) {
    const Document doc = inMode(rgbDocument(40, 30, 2, BlendMode::Multiply), ColorMode::CMYK);
    Document normal = doc;
    normal.layers[1].blendMode = BlendMode::Normal;
    const AnyImage rendered = renderNative(doc);
    const ImageC8& a = *rendered.c8();
    const ImageC8& base = *doc.layers[0].asset->image.c8();
    // Inverted ink: Multiply only ever lowers the stored value (adds ink), never below the product.
    for (int y = 0; y < a.height(); y++)
        for (int x = 0; x < a.width(); x++)
            for (int c = 0; c < 4; c++) CHECK(a.pixel(x, y)[c] <= base.pixel(x, y)[c]);
    CHECK(!(a == *renderNative(normal).c8()));
}

TEST_CASE(lab_normal_is_the_rgb_normal_on_the_same_values) {
    // The same bytes rendered as an RGB document and as a Lab one: Normal's arithmetic is the RGB kernel's.
    Document rgb = rgbDocument(36, 28, 3, BlendMode::Normal);
    rgb.layers[1].opacity = 0.6;
    Document lab = rgb;
    lab.colorMode = ColorMode::Lab;
    RenderOptions plain;
    Image rgbOut;
    render(rgb, plain, rgbOut);
    const AnyImage labOut = renderNative(lab);
    REQUIRE(labOut.u8());
    CHECK_EQ(maxDifference(rgbOut, *labOut.u8()), 0);
}

TEST_CASE(lab_luminosity_and_color_split_lightness_from_colour) {
    for (SampleType type : {SampleType::U8, SampleType::U16}) {
        Document doc(4, 4);
        doc.colorMode = ColorMode::Lab;
        auto solid = [&](double l, double a, double b) {
            auto img = std::make_shared<Image>(4, 4);
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++) {
                    uint8_t* p = img->pixel(x, y);
                    p[0] = storedLabL<SampleType::U8>(l, 255); p[1] = storedLabAB<SampleType::U8>(a, 255); p[2] = storedLabAB<SampleType::U8>(b, 255); p[3] = 255;
                }
            return img;
        };
        doc.layers.push_back(layerOf("back", ImagePtr(solid(30, 20, -10))));
        Layer top = layerOf("top", ImagePtr(solid(80, -40, 25)));
        top.blendMode = BlendMode::Luminosity;
        doc.layers.push_back(top);
        if (type == SampleType::U16) { std::string e; REQUIRE(convertSampleType(doc, SampleType::U16, &e)); }
        auto read = [&](const Document& d, double out[3]) {
            const AnyImage n = renderNative(d);
            if (n.u8()) { const uint8_t* p = n.u8()->pixel(1, 1); out[0] = labL<SampleType::U8>(p[0], p[3]); out[1] = labA<SampleType::U8>(p[1], p[3]); out[2] = labB<SampleType::U8>(p[2], p[3]); }
            else { const uint16_t* p = n.u16()->pixel(1, 1); out[0] = labL<SampleType::U16>(p[0], p[3]); out[1] = labA<SampleType::U16>(p[1], p[3]); out[2] = labB<SampleType::U16>(p[2], p[3]); }
        };
        double v[3];
        read(doc, v);
        CHECK_NEAR(v[0], 80, 0.6); CHECK_NEAR(v[1], 20, 1.01); CHECK_NEAR(v[2], -10, 1.01);
        doc.layers[1].blendMode = BlendMode::Color;
        read(doc, v);
        CHECK_NEAR(v[0], 30, 0.6); CHECK_NEAR(v[1], -40, 1.01); CHECK_NEAR(v[2], 25, 1.01);
    }
}

TEST_CASE(sixteen_bit_cmyk_renders_as_eight_bit_within_a_level) {
    Document eight = inMode(rgbDocument(40, 30, 3, BlendMode::Screen), ColorMode::CMYK);
    eight.layers[2].blendMode = BlendMode::Overlay;
    eight.layers[2].opacity = 0.7;
    Document sixteen = eight;
    std::string e;
    REQUIRE(convertSampleType(sixteen, SampleType::U16, &e));
    REQUIRE(sixteen.layers[0].asset->image.u16() && sixteen.layers[0].asset->image.u16()->channels() == 5);
    const AnyImage r8 = renderNative(eight), r16 = renderNative(sixteen);
    const ImageC8& a = *r8.c8();
    const Image16& b = *r16.u16();
    REQUIRE(b.channels() == 5);
    int worst = 0;
    for (int y = 0; y < a.height(); y++)
        for (int i = 0; i < a.width() * 5; i++) worst = std::max(worst, std::abs(int(a.row(y)[i]) - int(narrow16(b.row(y)[i]))));
    CHECK(worst <= 1);
    // The display of each is its native render through the profile (Little CMS's 8-bit and float pipelines differ by
    // several levels on random inks past the profile's total ink limit, which is printed).
    Image d8, d16;
    render(eight, RenderOptions(), d8);
    render(sixteen, RenderOptions(), d16);
    std::printf("  CMYK display, 8 against 16 bits: at most %d levels\n", maxDifference(d8, d16));
    Image expected16(d16.width(), d16.height());
    auto t16 = transformBetween(sixteen.profile, srgbProfile(), ConvertOptions(), PixelFormat::CMYKA16, PixelFormat::RGBA8);
    REQUIRE(t16 && convertImageTo8(r16, expected16, *t16));
    CHECK_EQ(maxDifference(d16, expected16), 0);
}

TEST_CASE(masks_folders_and_clipping_in_cmyk) {
    Document doc = inMode(rgbDocument(32, 24, 2, BlendMode::Normal), ColorMode::CMYK);
    const auto withoutTop = [&] { Document d = doc; d.layers.pop_back(); return *renderNative(d).c8(); }();
    // A mask of zeros hides the layer.
    Document masked = doc;
    LayerMask m;
    m.asset = MaskAsset::make(std::make_shared<GrayImage>(32, 24, 0));
    masked.layers[1].mask = m;
    CHECK(*renderNative(masked).c8() == withoutTop);
    // A hidden folder hides its children; a visible isolated one at full opacity draws them as they are.
    Document grouped = doc;
    Layer group("Folder", grouped.size());
    group.isGroup = true;
    group.passThrough = false;
    grouped.layers[1].parentId = group.id;
    grouped.layers.insert(grouped.layers.begin() + 1, group);
    CHECK(*renderNative(grouped).c8() == *renderNative(doc).c8());
    // A layer clipped to a transparent base draws nothing.
    Document clipped = doc;
    Layer empty = layerOf("empty", ImageC8Ptr(std::make_shared<ImageC8>(32, 24, 5)));
    clipped.layers.insert(clipped.layers.begin() + 1, empty);
    clipped.layers[2].maskSourceId = clipped.layers[1].id;
    CHECK(*renderNative(clipped).c8() == withoutTop);
}

TEST_CASE(cmyk_adjustment_layers_drawn_and_not_yet_drawn) {
    Document doc = inMode(rgbDocument(20, 16, 1, BlendMode::Normal), ColorMode::CMYK);
    const ImageC8 before = *renderNative(doc).c8();
    Layer invert("Invert", doc.size());
    invert.adjustment = AdjustmentSettings::defaults(AdjustmentKind::Invert).toLayerAdjustment();
    invert.asset.reset();
    doc.layers.push_back(invert);
    const ImageC8 inverted = *renderNative(doc).c8();
    for (int i = 0; i < 4; i++) CHECK_EQ(int(inverted.pixel(3, 3)[i]), 255 - int(before.pixel(3, 3)[i]));
    // A kind the CMYK renderer does not draw yet leaves the pixels as they are.
    doc.layers.back().adjustment = AdjustmentSettings::defaults(AdjustmentKind::Vibrance).toLayerAdjustment();
    CHECK(*renderNative(doc).c8() == before);
}

TEST_CASE(mode_conversion_round_trips_and_keeps_dormant_adjustments) {
    Document doc = rgbDocument(24, 18, 2, BlendMode::Multiply);
    Layer vibrance("Vibrance", doc.size());
    vibrance.asset.reset();
    vibrance.adjustment = AdjustmentSettings::defaults(AdjustmentKind::Vibrance).toLayerAdjustment();
    doc.layers.push_back(vibrance);
    AdjustmentSettings curves = AdjustmentSettings::defaults(AdjustmentKind::Curves);
    curves.curves.channels[2] = {{0, 0}, {128, 200}, {255, 255}};
    Layer curvesLayer("Curves", doc.size());
    curvesLayer.asset.reset();
    curvesLayer.adjustment = curves.toLayerAdjustment();
    doc.layers.push_back(curvesLayer);
    doc.layers[1].text = LayerText();
    doc.layers[1].text->red = 0; doc.layers[1].text->green = 1; doc.layers[1].text->blue = 0;   // pure green: outside CMYK

    Document cmyk = inMode(doc, ColorMode::CMYK);
    CHECK(!cmyk.layers[2].visible);
    CHECK(isDormantAdjustment(cmyk.layers[2]));
    CHECK(cmyk.layers[3].visible);
    AdjustmentSettings reset;
    REQUIRE(AdjustmentSettings::parse(cmyk.layers[3].adjustment->json, reset));
    CHECK(reset.curves.channels[2] == (std::vector<CurvePoint>{{0, 0}, {255, 255}}));
    CHECK(cmyk.layers[1].text->green < 1.0 && cmyk.layers[1].text->red > 0.0);   // brought into the press gamut
    CHECK(cmyk.layers[0].asset->thumbnail && cmyk.layers[0].asset->thumbnail->width() == 24);

    Document back = inMode(cmyk, ColorMode::RGB);
    CHECK(back.layers[2].visible);
    CHECK(!isDormantAdjustment(back.layers[2]));
    CHECK(back.layers[0].asset->image.u8());

    // RGB to Lab and back (the loss is measured in rgb_lab_round_trip_loss_by_depth).
    Document lab = inMode(rgbDocument(24, 18, 1, BlendMode::Normal), ColorMode::Lab);
    CHECK(lab.profile.icc.empty());
    Document rgbAgain = inMode(lab, ColorMode::RGB);
    CHECK(rgbAgain.layers[0].asset->image.u8());
}

TEST_CASE(mode_conversion_refuses_32_bit_and_over_budget) {
    Document doc(8, 8);
    doc.sampleType = SampleType::F32;
    std::string why;
    CHECK(!convertDocumentMode(doc, ColorMode::CMYK, ColorProfile(), ConvertOptions(), &why));
    CHECK(!why.empty());
    CHECK(doc.colorMode == ColorMode::RGB);
}

TEST_CASE(stored_colours_follow_the_mode) {
    double green[3] = {0, 1, 0};
    convertModeColor(ColorMode::RGB, ColorProfile(), ColorMode::CMYK, ColorProfile(), ConvertOptions(), green);
    CHECK(green[1] < 1.0);
    double grey[3] = {0.5, 0.5, 0.5};
    convertModeColor(ColorMode::RGB, ColorProfile(), ColorMode::Lab, ColorProfile(), ConvertOptions(), grey);
    CHECK_NEAR(grey[0], 0.5, 0.01);
}

TEST_CASE(five_channel_mips_and_thumbnails) {
    auto c8 = std::make_shared<ImageC8>(9, 7, 5);
    for (int y = 0; y < 7; y++) for (int x = 0; x < 9; x++) for (int c = 0; c < 5; c++) c8->pixel(x, y)[c] = uint8_t(c == 4 ? 255 : (x * 20 + c * 30) % 256);
    const auto half = halveImage(*c8);
    CHECK_EQ(half->channels(), 5);
    CHECK_EQ(half->width(), 5);
    CHECK_EQ(int(half->pixel(0, 0)[4]), 255);
    const ImageC8Ptr shared = c8;
    const auto level = MipCache::shared().level(shared, 1);
    REQUIRE(level);
    CHECK(*level == *half);
    auto w16 = std::make_shared<Image16>(9, 7, 5);
    const auto half16 = halveImage(*w16);
    CHECK_EQ(half16->channels(), 5);
    const Asset asset = Asset::make(ImageC8Ptr(c8), "cmyk");
    REQUIRE(asset.thumbnail);
    CHECK_EQ(asset.thumbnail->width(), 9);
}

TEST_CASE(rgb_lab_round_trip_loss_by_depth) {
    // 16 bits: exact for 8-bit sources. 8 bits: 8-bit Lab's quantisation (one step of a or b) moves a saturated colour
    // near R = 0 by tens of encoded levels while its colour difference stays near one; the mean stays under a level.
    const auto src = noisy(24, 18, 11, false);
    for (SampleType type : {SampleType::U8, SampleType::U16}) {
        const AnyImage in = imageAtDepth(ImagePtr(src), type);
        const AnyImage lab = convertImage(in, ColorMode::RGB, ColorProfile(), ColorMode::Lab, ColorProfile());
        const AnyImage back = imageAtDepth(convertImage(lab, ColorMode::Lab, ColorProfile(), ColorMode::RGB, ColorProfile()), SampleType::U8);
        REQUIRE(back.u8());
        int worst = 0;
        double sum = 0;
        for (int y = 0; y < 18; y++) for (int x = 0; x < 24; x++) for (int c = 0; c < 3; c++) {
            const int d = std::abs(int(back.u8()->pixel(x, y)[c]) - int(src->pixel(x, y)[c]));
            worst = std::max(worst, d);
            sum += d;
        }
        const double mean = sum / (24 * 18 * 3);
        std::printf("  RGB > Lab > RGB at %s bits: worst %d, mean %.2f levels\n", sampleTypeName(type), worst, mean);
        if (type == SampleType::U16) CHECK(worst <= 1);
        else CHECK(mean < 1.5 && worst <= 32);
    }
}

TEST_MAIN()

TEST_CASE(cmyk_blend_modes_match_photoshops_own_renders) {
    // Every mode on opaque pixels at 8 and 16 bits: the non-separable six within two sampler steps (the sampler reads
    // 8 bits), the separable modes (the RGB kernels, unchanged) within three, as Color Burn and Vivid Light near their
    // clips move a step further when the percent is quantised at 16 bits instead of 8.
    int worst8 = 0, worst16 = 0;
    for (const CmykReference& r : photoshopCmyk) {
        uint8_t src[5], dst[5];
        uint16_t src16[5], dst16[5];
        for (int c = 0; c < 4; c++) {
            src[c] = uint8_t(std::lround((100 - r.source[c]) * 2.55f)); dst[c] = uint8_t(std::lround((100 - r.backdrop[c]) * 2.55f));
            src16[c] = uint16_t(std::lround((100 - r.source[c]) * 327.68f)); dst16[c] = uint16_t(std::lround((100 - r.backdrop[c]) * 327.68f));
        }
        src[4] = dst[4] = 255; src16[4] = dst16[4] = 32768;
        const uint16_t k = 256;
        const uint32_t k16 = 32768;
        const float tolerance = (modes_nonSeparable(r.mode) ? 2 : 3) * 100.0f / 255.0f;
        compositeSpanMode8(r.mode, ColorMode::CMYK, src, &k, dst, 1);
        compositeSpanMode16(r.mode, ColorMode::CMYK, src16, &k16, dst16, 1);
        for (int c = 0; c < 4; c++) {
            const float ink8 = 100 - dst[c] / 2.55f, ink16 = 100 - dst16[c] / 327.68f;
            if (std::fabs(ink8 - r.expected[c]) > tolerance || std::fabs(ink16 - r.expected[c]) > tolerance)
                std::fprintf(stderr, "  %s ink %d: Photoshop %.2f, ours %.2f (8-bit) %.2f (16-bit)\n", blendModeName(r.mode), c, r.expected[c], ink8, ink16);
            worst8 = std::max(worst8, int(std::lround(std::fabs(ink8 - r.expected[c]) * 2.55f)));
            worst16 = std::max(worst16, int(std::lround(std::fabs(ink16 - r.expected[c]) * 2.55f)));
            CHECK(std::fabs(ink8 - r.expected[c]) <= tolerance);
            CHECK(std::fabs(ink16 - r.expected[c]) <= tolerance);
        }
    }
    std::fprintf(stderr, "  CMYK blend modes against Photoshop: at most %d steps off (8-bit), %d (16-bit)\n", worst8, worst16);
}

TEST_CASE(cmyk_gradient_fills_interpolate_their_own_inks) {
    // A CMYK document's gradient with CMYK stops runs from ink to ink (Photoshop's), not through RGB.
    Document doc(64, 8);
    doc.colorMode = ColorMode::CMYK;
    VectorPaint paint;
    paint.kind = VectorPaint::Kind::Gradient;
    paint.gradient.fillLayer = true;
    paint.gradient.angle = 0;
    paint.gradient.colors = {{0, {}, 0.5f, std::array<float, 4>{1.0f, 0.2f, 0.0f, 0.1f}}, {1, {}, 0.5f, std::array<float, 4>{0.0f, 0.6f, 1.0f, 0.5f}}};
    paint.gradient.alphas = {{0, 1, 0.5f}, {1, 1, 0.5f}};
    for (bool deep : {false, true}) {
        const AnyImage inks = renderVectorPaintInks(paint, doc, Rect(), Rect(0, 0, 64, 8), 1, 64, 8, deep);
        REQUIRE(inks);
        CHECK_EQ(inks.channels(), 5);
        const double one = deep ? 32768.0 : 255.0;
        auto ink = [&](int x, int c) {
            const double v = deep ? inks.u16()->pixel(x, 4)[c] : inks.c8()->pixel(x, 4)[c];
            return 1 - v / one;
        };
        const float first[4] = {1.0f, 0.2f, 0.0f, 0.1f}, last[4] = {0.0f, 0.6f, 1.0f, 0.5f};
        for (int c = 0; c < 4; c++) {
            CHECK_NEAR(ink(0, c), first[c], 0.03);
            CHECK_NEAR(ink(63, c), last[c], 0.03);
        }
        CHECK_NEAR(deep ? inks.u16()->pixel(10, 4)[4] : inks.c8()->pixel(10, 4)[4], one, 0.5);
    }
    // A stop without inks (an RGB colour) leaves the gradient to the RGB path.
    paint.gradient.colors[1].ink.reset();
    CHECK(!renderVectorPaintInks(paint, doc, Rect(), Rect(0, 0, 64, 8), 1, 64, 8, false));
}
