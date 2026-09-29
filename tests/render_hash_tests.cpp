// Render hashes: the 8-bit engine's output frozen as FNV-1a 64 hashes of the pixel bytes, compared with
// tests/render_hashes.txt. This is the gate for the high-bit-depth work (docs/high-bit-depth-plan.md, P0):
// a refactor that is meant to change nothing must leave every hash as it is.
//
// Covered: every scene golden_tests renders, one layered document per blend mode, tip-brush and MyPaint strokes,
// and the core filters and adjustments. Every scene is rendered twice, once on the worker pool and once serially
// (from inside a parallel loop, where nested loops run on the calling thread), and both must hash the same.
//
// COMPOSITOR_UPDATE_RENDER_HASHES=1 rewrites the baseline after an intentional rendering change; otherwise the
// test prints every entry that changed, appeared or went missing.
//
// 16-bit scenes ("u16/..."): the same documents converted to 16 bits and rendered at that depth (render16), hashed
// over their 16-bit samples; and a check that each renders within one 8-bit level of its 8-bit render once reduced.
//
// 32-bit scenes ("f32/..."): documents converted to 32 bits (linear float) and rendered at that depth (renderF), hashed
// over their float samples; the canvas frame through the view (exposure, Highlight Compression) and HDR Toning's
// conversion back to 8 bits, hashed as bytes. Their accuracy is held against a double-precision reference in
// depth_float_tests, not here.
//
// Not covered, on purpose:
// - Knockout: the engine has no knockout groups yet; an isolated (non pass-through) group stands in for it.
// - Color Lookup: needs a LUT file; its interpolation is exercised by its own tests.
#include "check.h"
#include "compositor/adjustments.h"
#include "compositor/animation.h"
#include "compositor/blur.h"
#include "compositor/brush.h"
#include "compositor/cameraraw.h"
#include "compositor/depth.h"
#include "compositor/document.h"
#include "compositor/filters.h"
#include "compositor/mypaint.h"
#include "compositor/parallel.h"
#include "compositor/render.h"
#include "compositor/colormgmt.h"
#include "compositor/blend.h"
#include "compositor/tipbrush.h"
#include "compositor/toning.h"
#include "compositor/layerstyle.h"
#include "compositor/matte.h"
#include "compositor/modeedit.h"
#include "compositor/morphology.h"
#include "compositor/presets.h"
#include "compositor/png.h"
#include "compositor/psd_carry.h"
#include "compositor/smartfilter.h"
#include "compositor/smartobject_edit.h"
#include "compositor/svg.h"
#include "compositor/vectorlayer.h"
#include "compositor/vectormask.h"
#include "compositor/warp.h"

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <tuple>

using namespace compositor;
namespace fs = std::filesystem;

/// REQUIRE for the scene functions, which return a hash.
#define NEED(expr) do { if (!(expr)) { check::fail(__FILE__, __LINE__, #expr); return uint64_t(0); } } while (0)

namespace {

// ---- hashing -----------------------------------------------------------------------------------------------------

struct Fnv {
    uint64_t h = 1469598103934665603ull;
    void bytes(const uint8_t* p, size_t n) { for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; } }
    void u32(uint32_t v) { uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; bytes(b, 4); }
};

uint64_t hashImage(const Image& image) {
    Fnv f;
    f.u32(uint32_t(image.width()));
    f.u32(uint32_t(image.height()));
    for (int y = 0; y < image.height(); y++) f.bytes(image.row(y), size_t(image.width()) * 4);
    return f.h;
}

uint64_t hashGray(const GrayImage& image) {
    Fnv f;
    f.u32(uint32_t(image.width()));
    f.u32(uint32_t(image.height()));
    f.u32(0x67726179u);   // "gray", so a mask never collides with an RGBA image
    for (int y = 0; y < image.height(); y++) f.bytes(image.row(y), size_t(image.width()));
    return f.h;
}

uint64_t hashImage16(const Image16& image) {
    Fnv f;
    f.u32(uint32_t(image.width()));
    f.u32(uint32_t(image.height()));
    f.u32(0x31366269u);   // "16bi"
    for (int y = 0; y < image.height(); y++) f.bytes(reinterpret_cast<const uint8_t*>(image.row(y)), size_t(image.width()) * 8);
    return f.h;
}

/// A scene: a name and a function producing its hash.
std::map<std::string, std::function<uint64_t()>>& scenes() { static std::map<std::string, std::function<uint64_t()>> s; return s; }
void scene(const std::string& name, std::function<uint64_t()> body) { scenes()[name] = std::move(body); }

/// Runs `body` with every nested parallel loop serial: the caller of parallelFor marks itself as inside a loop.
uint64_t serially(const std::function<uint64_t()>& body) {
    if (workerCount() <= 1) return body();
    uint64_t result = 0;
    parallelFor(0, 2, 1, [&](int y0, int) { if (y0 == 0) result = body(); });
    return result;
}

// ---- inputs ------------------------------------------------------------------------------------------------------

// The golden_tests fixtures, byte for byte.
std::shared_ptr<Image> gradient(int w, int h) {
    auto img = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t* p = img->pixel(x, y);
            uint8_t a = uint8_t(255 * (x + 1) / w);
            p[0] = uint8_t(255 * x / (w - 1) * a / 255);
            p[1] = uint8_t(255 * y / (h - 1) * a / 255);
            p[2] = uint8_t(128 * a / 255);
            p[3] = a;
        }
    return img;
}

std::shared_ptr<Image> checker(int w, int h, int cell) {
    auto img = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            bool light = ((x / cell) + (y / cell)) % 2 == 0;
            uint8_t* p = img->pixel(x, y);
            p[0] = light ? 230 : 40; p[1] = light ? 230 : 40; p[2] = light ? 230 : 200; p[3] = 255;
        }
    return img;
}

uint32_t mix(uint32_t v) { v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15; v *= 0x846ca68bu; v ^= v >> 16; return v; }

/// An opaque gradient with integer-hashed noise on it: a busy base for blends, filters and adjustments.
std::shared_ptr<Image> noisyBase(int w, int h, uint32_t seed = 1) {
    auto img = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t n = mix(seed * 0x9E3779B1u ^ uint32_t(y * w + x));
            uint8_t* p = img->pixel(x, y);
            p[0] = uint8_t(std::clamp(255 * x / (w - 1) + int(n & 31) - 16, 0, 255));
            p[1] = uint8_t(std::clamp(255 * y / (h - 1) + int((n >> 8) & 31) - 16, 0, 255));
            p[2] = uint8_t(std::clamp(255 - 255 * (x + y) / (w + h - 2) + int((n >> 16) & 31) - 16, 0, 255));
            p[3] = 255;
        }
    return img;
}

/// Colourful, partly transparent content: hue sweeps, alpha from a radial ramp with a noisy fringe.
std::shared_ptr<Image> paint(int w, int h, uint32_t seed = 2) {
    auto img = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            double d = std::hypot(x - w / 2.0, y - h / 2.0) / (std::min(w, h) / 2.0);
            int a = std::clamp(int((1.15 - d) * 400), 0, 255);
            uint32_t n = mix(seed ^ uint32_t(y * w + x));
            if (a > 0 && a < 255) a = std::clamp(a + int(n & 15) - 8, 0, 255);
            int r = (x * 7 + y * 3) % 256, g = (x * 2 + y * 5 + 80) % 256, b = (255 - x * 3 + y) & 255;
            uint8_t* p = img->pixel(x, y);
            p[0] = uint8_t((r * a + 127) / 255); p[1] = uint8_t((g * a + 127) / 255); p[2] = uint8_t((b * a + 127) / 255); p[3] = uint8_t(a);
        }
    return img;
}

std::shared_ptr<GrayImage> radialMask(int w, int h) {
    auto mask = std::make_shared<GrayImage>(w, h, 0);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        double d = std::hypot(x - w / 2.0, y - h / 2.0);
        mask->at(x, y) = uint8_t(std::max(0.0, std::min(255.0, (std::min(w, h) * 0.45 - d) * 8)));
    }
    return mask;
}

Layer layerOf(const std::string& name, std::shared_ptr<Image> image, Point origin) { return Layer(Asset::make(image, name), origin); }

LayerMask maskOf(std::shared_ptr<GrayImage> mask) { LayerMask m; m.asset = MaskAsset::make(mask); return m; }

std::string slug(std::string name) {
    for (auto& ch : name) ch = ch == ' ' ? '_' : char(std::tolower(static_cast<unsigned char>(ch)));
    return name;
}

// ---- golden_tests scenes ---------------------------------------------------------------------------------------

Document goldenBase() {
    Document doc(96, 64);
    doc.id = "00000000-0000-4000-8000-000000000001";
    doc.layers.push_back(layerOf("checker", checker(96, 64, 8), {0, 0}));
    return doc;
}

void addGoldenScenes() {
    scene("golden/normal_blend", [] {
        Document doc = goldenBase();
        doc.layers.push_back(layerOf("gradient", gradient(48, 32), {24, 16}));
        return hashImage(*renderFlattened(doc));
    });
    const BlendMode modes[] = {BlendMode::Multiply, BlendMode::Screen, BlendMode::Overlay, BlendMode::Darken, BlendMode::Lighten,
                               BlendMode::Difference, BlendMode::ColorDodge, BlendMode::ColorBurn, BlendMode::Hue, BlendMode::Saturation,
                               BlendMode::Color, BlendMode::Luminosity};
    for (BlendMode mode : modes)
        scene("golden/blend_" + slug(blendModeName(mode)), [mode] {
            Document doc = goldenBase();
            Layer top = layerOf("gradient", gradient(96, 64), {0, 0});
            top.blendMode = mode;
            top.opacity = 0.8;
            doc.layers.push_back(top);
            return hashImage(*renderFlattened(doc));
        });
    auto transformed = [](int variant) {
        Document doc = goldenBase();
        Layer top = layerOf("gradient", gradient(32, 32), {20, 10});
        top.transform.size = {50, 40};
        top.transform.rotation = 25;
        top.transform.flipX = true;
        if (variant == 1) top.transform.sampling = Sampling::Nearest;
        if (variant == 2) {
            top.asset = Asset::make(gradient(256, 256), "big");
            top.transform.size = {30, 30};
            top.transform.rotation = 0;
            top.transform.flipX = false;
        }
        doc.layers.push_back(top);
        return hashImage(*renderFlattened(doc));
    };
    scene("golden/transformed_layer", [=] { return transformed(0); });
    scene("golden/transformed_layer_nearest", [=] { return transformed(1); });
    scene("golden/reduced_layer", [=] { return transformed(2); });
    auto masked = [](bool placed) {
        Document doc = goldenBase();
        Layer top = layerOf("gradient", gradient(64, 48), {16, 8});
        auto mask = std::make_shared<GrayImage>(64, 48, 0);
        for (int y = 0; y < 48; y++) for (int x = 0; x < 64; x++) {
            double d = std::hypot(x - 32.0, y - 24.0);
            mask->at(x, y) = uint8_t(std::max(0.0, std::min(255.0, (28 - d) * 32)));
        }
        top.mask = maskOf(mask);
        if (placed) top.mask->placement = LayerTransform(Point(30, 16), Size(64, 48));
        doc.layers.push_back(top);
        return hashImage(*renderFlattened(doc));
    };
    scene("golden/masked_layer", [=] { return masked(false); });
    scene("golden/placed_mask", [=] { return masked(true); });
    scene("golden/folder_mask_clipping", [] {
        Document doc = goldenBase();
        Layer group("Folder", doc.size());
        group.isGroup = true;
        auto fmask = std::make_shared<GrayImage>(96, 64, 0);
        for (int y = 0; y < 64; y++) for (int x = 0; x < 96; x++) fmask->at(x, y) = uint8_t(255 * x / 95);
        group.mask = maskOf(fmask);
        Layer base = layerOf("base", gradient(40, 40), {10, 10});
        base.parentId = group.id;
        base.opacity = 0.9;
        Layer clipped = layerOf("clipped", checker(96, 64, 4), {0, 0});
        clipped.parentId = group.id;
        clipped.maskSourceId = base.id;
        clipped.blendMode = BlendMode::Screen;
        doc.layers.push_back(group);
        doc.layers.push_back(base);
        doc.layers.push_back(clipped);
        return hashImage(*renderFlattened(doc));
    });
    auto zoomed = [](bool reduced) {
        Document doc = goldenBase();
        doc.layers.push_back(layerOf("gradient", gradient(48, 32), {24, 16}));
        Image out;
        RenderOptions options;
        if (reduced) options.scale = 0.25;
        else { options.region = {20, 12, 40, 30}; options.scale = 3; }
        render(doc, options, out);
        return hashImage(out);
    };
    scene("golden/zoomed_region", [=] { return zoomed(false); });
    scene("golden/reduced_document", [=] { return zoomed(true); });
}

// ---- one document per blend mode ---------------------------------------------------------------------------------

/// Base, a masked layer in `mode`, a layer clipped to it in `mode`, and an isolated group in `mode` holding a
/// Normal layer and one in `mode`. Opacity varies with the mode so no two documents share every setting.
Document blendDocument(BlendMode mode) {
    const int W = 256, H = 192, index = int(mode);
    Document doc(W, H);
    doc.id = "00000000-0000-4000-8000-000000000002";
    doc.layers.push_back(layerOf("base", noisyBase(W, H), {0, 0}));

    Layer top = layerOf("top", paint(160, 128, 3), {20, 12});
    top.blendMode = mode;
    top.opacity = 0.35 + 0.65 * double((index * 7) % 13) / 12.0;
    top.mask = maskOf(radialMask(160, 128));
    doc.layers.push_back(top);

    Layer clipped = layerOf("clipped", checker(W, H, 6), {0, 0});
    clipped.maskSourceId = top.id;
    clipped.blendMode = mode;
    clipped.opacity = 0.6;
    doc.layers.push_back(clipped);

    Layer group("Group", doc.size());
    group.isGroup = true;
    group.passThrough = false;
    group.blendMode = mode;
    group.opacity = 0.8;
    Layer inner = layerOf("inner", paint(120, 120, 5), {110, 60});
    inner.parentId = group.id;
    Layer innerMode = layerOf("inner mode", gradient(140, 100), {90, 80});
    innerMode.parentId = group.id;
    innerMode.blendMode = mode;
    innerMode.opacity = 0.9;
    doc.layers.push_back(group);
    doc.layers.push_back(inner);
    doc.layers.push_back(innerMode);
    return doc;
}

void addBlendScenes() {
    for (int m = 0; m < blendModeCount; m++) {
        BlendMode mode = BlendMode(m);
        std::string name = slug(blendModeName(mode));
        scene("blend/" + name, [mode] { return hashImage(*renderFlattened(blendDocument(mode))); });
        // The same document at a reduced scale and over a region, through the mip path.
        scene("blend/" + name + "@0.5", [mode] {
            Image out;
            RenderOptions options;
            options.region = {16, 8, 200, 160};
            options.scale = 0.5;
            render(blendDocument(mode), options, out);
            return hashImage(out);
        });
    }
    // A pass-through group holding a masked layer, the default folder behaviour.
    scene("blend/pass_through_group", [] {
        Document doc = blendDocument(BlendMode::Multiply);
        for (Layer& l : doc.layers) if (l.isGroup) { l.passThrough = true; l.blendMode = BlendMode::Normal; l.opacity = 1; }
        return hashImage(*renderFlattened(doc));
    });
}

// ---- CMYK and Lab (P7) ---------------------------------------------------------------------------------------------
//
// The blend documents converted with Image > Mode (the bundled Working CMYK, Lab D50), at 8 and 16 bits: the native
// render's samples ("cmyk/", "lab/", "cmyk16/", "lab16/") in every mode the colour mode offers. The display (through
// the profile to sRGB) is not hashed: Little CMS's float maths rounds differently with the system's libm, so it is
// checked against the same transform in colormodes_render_tests instead.

uint64_t hashNative(const AnyImage& image) {
    Fnv f;
    f.u32(uint32_t(image.width()));
    f.u32(uint32_t(image.height()));
    f.u32(uint32_t(image.channels()) << 8 | uint32_t(image.sampleType()));
    const size_t n = size_t(image.width()) * size_t(image.channels());
    for (int y = 0; y < image.height(); y++) {
        if (auto c8 = image.c8()) f.bytes(c8->row(y), n);
        else if (auto u8 = image.u8()) f.bytes(u8->row(y), n);
        else if (auto u16 = image.u16()) f.bytes(reinterpret_cast<const uint8_t*>(u16->row(y)), n * 2);
    }
    return f.h;
}

Document inColorMode(Document doc, ColorMode mode, SampleType type) {
    std::string error;
    if (!convertDocumentMode(doc, mode, ColorProfile(), ConvertOptions(), &error)) check::fail(__FILE__, __LINE__, "convertDocumentMode: " + error);
    if (type != SampleType::U8 && !convertSampleType(doc, type, &error)) check::fail(__FILE__, __LINE__, "convertSampleType: " + error);
    return doc;
}

void addColorModeScenes() {
    for (ColorMode colorMode : {ColorMode::CMYK, ColorMode::Lab}) {
        for (SampleType type : {SampleType::U8, SampleType::U16}) {
            const std::string prefix = std::string(colorMode == ColorMode::CMYK ? "cmyk" : "lab") + (type == SampleType::U16 ? "16" : "") + "/";
            for (int m = 0; m < blendModeCount; m++) {
                const BlendMode mode = BlendMode(m);
                if (!blendModeAvailable(mode, colorMode)) continue;
                const std::string name = slug(blendModeName(mode));
                scene(prefix + "blend/" + name, [=] { return hashNative(renderNative(inColorMode(blendDocument(mode), colorMode, type))); });
            }
            scene(prefix + "blend/multiply@0.5", [=] {
                RenderOptions options;
                options.region = {16, 8, 200, 160};
                options.scale = 0.5;
                return hashNative(renderNative(inColorMode(blendDocument(BlendMode::Multiply), colorMode, type), options));
            });
            // P7 E: adjustment layers on the document's own samples (modeedit.h), the kinds that take no colour
            // through Little CMS's float transform; and the filters on a converted layer's pixels.
            const bool cmyk = colorMode == ColorMode::CMYK;
            auto adjusted = [=](std::vector<AdjustmentSettings> kinds) {
                Document doc = inColorMode(blendDocument(BlendMode::Normal), colorMode, type);
                for (const AdjustmentSettings& s : kinds) {
                    Layer layer(adjustmentKindName(s.kind), doc.size());
                    layer.adjustment = s.toLayerAdjustment();
                    doc.layers.push_back(layer);
                }
                return hashNative(renderNative(doc));
            };
            scene(prefix + "adjust/levels_curves", [=] {
                AdjustmentSettings levels = AdjustmentSettings::defaults(AdjustmentKind::Levels);
                levels.levels.ranges[0] = {12, 1.2, 240, 0, 255};
                levels.levels.ranges[cmyk ? 4 : 2] = {0, 0.8, 200, 20, 255};
                AdjustmentSettings curves = AdjustmentSettings::defaults(AdjustmentKind::Curves);
                curves.curves.channels[1] = {{0, 0}, {90, 140}, {255, 255}};
                curves.curves.channels[3] = {{0, 30}, {255, 220}};
                return adjusted({levels, curves});
            });
            scene(prefix + "adjust/tones", [=] {
                AdjustmentSettings bc = AdjustmentSettings::defaults(AdjustmentKind::BrightnessContrast);
                bc.brightnessContrast = {20, 30, false};
                AdjustmentSettings posterize = AdjustmentSettings::defaults(AdjustmentKind::Posterize);
                posterize.posterize.levels = 6;
                AdjustmentSettings invert = AdjustmentSettings::defaults(AdjustmentKind::Invert);
                if (cmyk) return adjusted({bc, posterize, invert});
                AdjustmentSettings exposure = AdjustmentSettings::defaults(AdjustmentKind::Exposure);
                exposure.exposure = {0.7, 0.01, 1.1};
                return adjusted({bc, exposure, posterize, invert});
            });
            if (cmyk)
                scene(prefix + "adjust/colour", [=] {
                    AdjustmentSettings selective = AdjustmentSettings::defaults(AdjustmentKind::SelectiveColor);
                    selective.selectiveColor.ranges[0] = {-20, 10, 0, 30};
                    selective.selectiveColor.ranges[7] = {10, -5, 20, -10};
                    AdjustmentSettings mixer = AdjustmentSettings::defaults(AdjustmentKind::ChannelMixer);
                    mixer.channelMixer.inks[0] = {80, 20, 0, 0, 5};
                    mixer.channelMixer.inks[3] = {10, 10, 10, 90, 0};
                    AdjustmentSettings hsv = AdjustmentSettings::defaults(AdjustmentKind::HueSaturation);
                    hsv.hsv.adjustments[0] = {25, -30, 5};
                    AdjustmentSettings balance = AdjustmentSettings::defaults(AdjustmentKind::ColorBalance);
                    balance.colorBalance.ranges[1] = {30, -20, 10};
                    return adjusted({selective, mixer, hsv, balance});
                });
            auto filtered = [=](FilterKind kind, FilterSettings settings) {
                const Document doc = inColorMode(blendDocument(BlendMode::Normal), colorMode, type);
                for (const Layer& l : doc.layers)
                    if (l.asset && l.asset->image) return hashNative(filteredInMode(kind, l.asset->image, colorMode, settings, 1, 5));
                return uint64_t(0);
            };
            FilterSettings blur;
            blur.radius = 3;
            scene(prefix + "filter/gaussian_blur", [=] { return filtered(FilterKind::GaussianBlur, blur); });
            FilterSettings motion;
            motion.angle = 30;
            motion.distance = 12;
            scene(prefix + "filter/motion_blur", [=] { return filtered(FilterKind::MotionBlur, motion); });
            FilterSettings noise;
            noise.amount = 20;
            noise.gaussian = true;
            scene(prefix + "filter/add_noise", [=] { return filtered(FilterKind::AddNoise, noise); });
            noise.monochromatic = true;
            scene(prefix + "filter/add_noise_mono", [=] { return filtered(FilterKind::AddNoise, noise); });
            FilterSettings lens;
            lens.distortion = 40;
            lens.bicubic = true;
            scene(prefix + "filter/lens_correction", [=] { return filtered(FilterKind::LensCorrection, lens); });
        }
    }
}

// ---- 16 bits ------------------------------------------------------------------------------------------------------

Document sixteen(Document doc) {
    std::string error;
    if (!convertSampleType(doc, SampleType::U16, &error)) check::fail(__FILE__, __LINE__, "convertSampleType: " + error);
    return doc;
}

uint64_t hash16(const Document& doc, const RenderOptions& options = {}) {
    Image16 out;
    render16(doc, options, out);
    return hashImage16(out);
}

RenderOptions reducedRegion() {
    RenderOptions options;
    options.region = {16, 8, 200, 160};
    options.scale = 0.5;
    return options;
}

void add16BitScenes() {
    for (int m = 0; m < blendModeCount; m++) {
        const BlendMode mode = BlendMode(m);
        const std::string name = slug(blendModeName(mode));
        scene("u16/blend/" + name, [mode] { return hash16(sixteen(blendDocument(mode))); });
        scene("u16/blend/" + name + "@0.5", [mode] { return hash16(sixteen(blendDocument(mode)), reducedRegion()); });
    }
    scene("u16/blend/pass_through_group", [] {
        Document doc = blendDocument(BlendMode::Multiply);
        for (Layer& l : doc.layers) if (l.isGroup) { l.passThrough = true; l.blendMode = BlendMode::Normal; l.opacity = 0.7; }
        return hash16(sixteen(doc));
    });
    scene("u16/golden/transformed_layer", [] {
        Document doc = goldenBase();
        Layer top = layerOf("gradient", gradient(32, 32), {20, 10});
        top.transform.size = {50, 40};
        top.transform.rotation = 25;
        top.transform.flipX = true;
        doc.layers.push_back(top);
        return hash16(sixteen(doc));
    });
    scene("u16/golden/placed_mask", [] {
        Document doc = goldenBase();
        Layer top = layerOf("gradient", gradient(64, 48), {16, 8});
        top.mask = maskOf(radialMask(64, 48));
        top.mask->placement = LayerTransform(Point(30, 16), Size(64, 48));
        doc.layers.push_back(top);
        return hash16(sixteen(doc));
    });
    scene("u16/adjust_layer/levels", [] {
        Document doc(128, 96);
        doc.layers.push_back(layerOf("base", noisyBase(128, 96), {0, 0}));
        Layer adj("Levels", doc.size());
        AdjustmentSettings s = AdjustmentSettings::defaults(AdjustmentKind::Levels);
        s.levels.ranges[0] = {15, 1.2, 240, 0, 255};
        adj.adjustment = s.toLayerAdjustment();
        adj.opacity = 0.75;
        doc.layers.push_back(adj);
        return hash16(sixteen(doc));
    });
}

// ---- 16-bit shapes, vector masks and layer styles -----------------------------------------------------------------

/// A shape layer made in a 16-bit document (its pixels at 16 bits), over the golden base.
uint64_t shapeScene16(const VectorShape& shape) {
    Document doc = sixteen(goldenBase());
    Layer layer = layerOf("shape", std::make_shared<Image>(1, 1), {0, 0});
    setVectorShape(layer, doc, shape);
    doc.layers.push_back(layer);
    return hash16(doc);
}

StyleGradient twoStops(StyleColor from, StyleColor to, float angle) {
    StyleGradient g;
    g.colors = {{0, from, 0.5f}, {1, to, 0.5f}};
    g.alphas = {{0, 1, 0.5f}, {1, 1, 0.5f}};
    g.angle = angle;
    return g;
}

/// Soft paint carrying `style` over the golden base, converted to 16 bits.
Document styledScene(const LayerStyle& style) {
    Document doc = goldenBase();
    std::vector<uint8_t> rgba;
    for (int i = 0; i < 16; i++) { rgba.push_back(uint8_t(60 * (i % 4))); rgba.push_back(uint8_t(60 * (i / 4))); rgba.push_back(200); rgba.push_back(255); }
    addDocumentPatterns(doc, {makePattern("tile", "Tile", 4, 4, rgba)});
    Layer top = layerOf("paint", paint(56, 40, 5), {20, 12});
    setLayerStyle(top, style);
    doc.layers.push_back(top);
    return sixteen(doc);
}

void add16BitVectorScenes() {
    VectorShape ellipse;
    ellipse.path = ellipsePath(Rect(12.5, 8.25, 60, 44));
    ellipse.r = 200; ellipse.g = 60; ellipse.b = 30;
    ellipse.stroke.enabled = false;
    scene("u16/shape/solid_ellipse", [ellipse] { return shapeScene16(ellipse); });
    VectorShape stroked = ellipse;
    stroked.path = rectanglePath(Rect(16, 10, 60, 40), 8);
    stroked.stroke.enabled = true;
    stroked.stroke.width = 4;
    stroked.stroke.dashes = {2, 1};
    scene("u16/shape/dashed_stroke", [stroked] { return shapeScene16(stroked); });
    VectorShape gradient = stroked;
    gradient.stroke.dashes.clear();
    gradient.fillPaint.kind = VectorPaint::Kind::Gradient;
    gradient.fillPaint.gradient = twoStops({255, 0, 0}, {0, 0, 255}, 30);
    gradient.stroke.paint.kind = VectorPaint::Kind::Gradient;
    gradient.stroke.paint.gradient = twoStops({255, 255, 0}, {0, 128, 0}, 90);
    scene("u16/shape/gradient_fill_and_stroke", [gradient] { return shapeScene16(gradient); });
    VectorShape combined = ellipse;
    addShapeComponent(combined.path, polygonPath(Rect(30, 14, 30, 30), 5, 0.5), VectorPath::Op::Subtract);
    scene("u16/shape/subtracted_star", [combined] { return shapeScene16(combined); });
    scene("u16/vector_mask/pixel_layer", [] {
        Document doc = goldenBase();
        Layer top = layerOf("paint", paint(64, 48), {16, 8});
        setLayerVectorMask(top, doc, ellipsePath(Rect(20.5, 10.5, 50, 36)));
        doc.layers.push_back(top);
        return hash16(sixteen(doc));
    });
    scene("u16/vector_mask/pixel_layer@0.5", [] {
        Document doc = goldenBase();
        Layer top = layerOf("paint", paint(64, 48), {16, 8});
        setLayerVectorMask(top, doc, ellipsePath(Rect(20.5, 10.5, 50, 36)));
        doc.layers.push_back(top);
        return hash16(sixteen(doc), reducedRegion());
    });
    scene("u16/fill_layer/gradient", [] {
        Document doc = goldenBase();
        Layer fill("Gradient Fill", doc.size());
        auto carry = std::make_shared<PsdLayerCarry>();
        carry->blocks.push_back({"GdFl", authorGradientFill(twoStops({250, 200, 10}, {10, 40, 220}, 60))});
        fill.psdCarry = carry;
        fill.opacity = 0.7;
        doc.layers.push_back(fill);
        return hash16(sixteen(doc));
    });
    // Each effect alone, then all ten together, then a folder's style.
    std::vector<std::pair<std::string, LayerStyle>> styles;
    auto effect = [&](const std::string& name, const std::function<void(LayerStyle&)>& make) { LayerStyle s; make(s); styles.push_back({name, s}); };
    effect("drop_shadow", [](LayerStyle& s) { DropShadow d; d.distance = 4; d.size = 5; s.dropShadows.push_back(d); });
    effect("inner_shadow", [](LayerStyle& s) { InnerShadow d; d.size = 4; s.innerShadows.push_back(d); });
    effect("outer_glow", [](LayerStyle& s) { OuterGlow g; g.size = 6; s.outerGlows.push_back(g); });
    effect("inner_glow", [](LayerStyle& s) { InnerGlow g; g.size = 5; s.innerGlows.push_back(g); });
    effect("satin", [](LayerStyle& s) { Satin t; t.size = 8; s.satins.push_back(t); });
    effect("color_overlay", [](LayerStyle& s) { ColorOverlay c; c.color = {20, 180, 90}; c.opacity = 0.6f; s.colorOverlays.push_back(c); });
    effect("gradient_overlay", [](LayerStyle& s) { GradientOverlay g; g.gradient = twoStops({255, 0, 0}, {0, 0, 255}, 90); s.gradientOverlays.push_back(g); });
    effect("pattern_overlay", [](LayerStyle& s) { PatternOverlay p; p.patternId = "tile"; p.opacity = 0.7f; s.patternOverlays.push_back(p); });
    effect("stroke", [](LayerStyle& s) { Stroke k; k.size = 3; s.strokes.push_back(k); });
    effect("bevel", [](LayerStyle& s) { Bevel b; b.size = 5; s.bevels.push_back(b); });
    LayerStyle all;
    for (auto& [name, style] : styles) {
        for (auto& v : style.dropShadows) all.dropShadows.push_back(v);
        for (auto& v : style.innerShadows) all.innerShadows.push_back(v);
        for (auto& v : style.outerGlows) all.outerGlows.push_back(v);
        for (auto& v : style.innerGlows) all.innerGlows.push_back(v);
        for (auto& v : style.satins) all.satins.push_back(v);
        for (auto& v : style.colorOverlays) all.colorOverlays.push_back(v);
        for (auto& v : style.gradientOverlays) all.gradientOverlays.push_back(v);
        for (auto& v : style.patternOverlays) all.patternOverlays.push_back(v);
        for (auto& v : style.strokes) all.strokes.push_back(v);
        for (auto& v : style.bevels) all.bevels.push_back(v);
    }
    styles.push_back({"all_ten", all});
    for (auto& [name, style] : styles) {
        const LayerStyle copy = style;
        scene("u16/style/" + name, [copy] { return hash16(styledScene(copy)); });
    }
    scene("u16/style/all_ten@0.5", [all] { return hash16(styledScene(all), reducedRegion()); });
    scene("u16/style/folder", [] {
        Document doc = goldenBase();
        Layer folder("Folder", doc.size());
        folder.isGroup = true;
        LayerStyle style;
        OuterGlow glow; glow.size = 6; style.outerGlows.push_back(glow);
        Stroke stroke; stroke.size = 2; stroke.color = {255, 255, 255}; style.strokes.push_back(stroke);
        setLayerStyle(folder, style);
        Layer a = layerOf("a", paint(48, 36, 3), {10, 10});
        a.parentId = folder.id;
        doc.layers.push_back(folder);
        doc.layers.push_back(a);
        return hash16(sixteen(doc));
    });
}

// ---- 32 bits ------------------------------------------------------------------------------------------------------

uint64_t hashImageF(const ImageF& image) {
    Fnv f;
    f.u32(uint32_t(image.width()));
    f.u32(uint32_t(image.height()));
    f.u32(0x33326266u);   // "32bf"
    for (int y = 0; y < image.height(); y++) f.bytes(reinterpret_cast<const uint8_t*>(image.row(y)), size_t(image.width()) * 16);
    return f.h;
}

Document thirtyTwo(Document doc) {
    std::string error;
    if (!convertSampleType(doc, SampleType::F32, &error)) check::fail(__FILE__, __LINE__, "convertSampleType: " + error);
    return doc;
}

uint64_t hashF(const Document& doc, const RenderOptions& options = {}) {
    ImageF out;
    renderF(doc, options, out);
    return hashImageF(out);
}

/// Values above 1: a bright layer (straight colour up to 6) in Linear Dodge over the blend scene.
Document hdrScene() {
    Document doc = thirtyTwo(blendDocument(BlendMode::Normal));
    auto bright = std::make_shared<ImageF>(96, 64);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 96; x++) {
            float* p = bright->pixel(x, y);
            const float a = float(std::min(1.0, (x + y) / 80.0));
            p[0] = 6.0f * float(x) / 95.0f * a; p[1] = 2.0f * float(y) / 63.0f * a; p[2] = 0.5f * a; p[3] = a;
        }
    Layer layer(Asset::make(ImageFPtr(bright), "bright"), Point(40, 30));
    layer.blendMode = BlendMode::LinearDodge;
    doc.layers.push_back(layer);
    return doc;
}

void add32BitScenes() {
    for (int m = 0; m < blendModeCount; m++) {
        const BlendMode mode = BlendMode(m);
        const std::string name = slug(blendModeName(mode));
        scene("f32/blend/" + name, [mode] { return hashF(thirtyTwo(blendDocument(mode))); });
        scene("f32/blend/" + name + "@0.5", [mode] { return hashF(thirtyTwo(blendDocument(mode)), reducedRegion()); });
    }
    scene("f32/blend/pass_through_group", [] {
        Document doc = blendDocument(BlendMode::Multiply);
        for (Layer& l : doc.layers) if (l.isGroup) { l.passThrough = true; l.blendMode = BlendMode::Normal; l.opacity = 0.7; }
        return hashF(thirtyTwo(doc));
    });
    scene("f32/golden/transformed_layer", [] {
        Document doc = goldenBase();
        Layer top = layerOf("gradient", gradient(32, 32), {20, 10});
        top.transform.size = {50, 40};
        top.transform.rotation = 25;
        top.transform.flipX = true;
        doc.layers.push_back(top);
        return hashF(thirtyTwo(doc));
    });
    scene("f32/golden/high_quality_scaled", [] {
        Document doc = goldenBase();
        Layer top = layerOf("gradient", gradient(64, 48), {16, 8});
        top.transform.size = {40, 30};
        top.transform.sampling = Sampling::High;
        doc.layers.push_back(top);
        return hashF(thirtyTwo(doc));
    });
    scene("f32/golden/placed_mask", [] {
        Document doc = goldenBase();
        Layer top = layerOf("gradient", gradient(64, 48), {16, 8});
        top.mask = maskOf(radialMask(64, 48));
        top.mask->placement = LayerTransform(Point(30, 16), Size(64, 48));
        doc.layers.push_back(top);
        return hashF(thirtyTwo(doc));
    });
    scene("f32/hdr/linear_dodge", [] { return hashF(hdrScene()); });
    scene("f32/hdr/linear_dodge@0.5", [] { return hashF(hdrScene(), reducedRegion()); });
    // The canvas frame: through the view, to 8 bits (no display transform: the document's curve encodes).
    scene("f32/view/exposure_0", [] { Image out; render(hdrScene(), RenderOptions(), out); return hashImage(out); });
    scene("f32/view/exposure_minus_2_gamma_1_4", [] {
        RenderOptions o;
        o.view32.exposure = -2;
        o.view32.gamma = 1.4;
        Image out;
        render(hdrScene(), o, out);
        return hashImage(out);
    });
    scene("f32/view/highlight_compression", [] {
        RenderOptions o;
        o.view32.method = ToneMethod::HighlightCompression;
        Image out;
        render(hdrScene(), o, out);
        return hashImage(out);
    });
    // HDR Toning back to 8 bits, and the untoned trip.
    scene("f32/convert/to_8_exposure_gamma", [] {
        Document doc = hdrScene();
        View32 toning;
        toning.exposure = -1.5;
        toning.gamma = 1.2;
        std::string error;
        if (!convertSampleType(doc, SampleType::U8, &error, &toning)) check::fail(__FILE__, __LINE__, error);
        return hashImage(*renderFlattened(doc));
    });
    scene("f32/convert/to_16_highlight_compression", [] {
        Document doc = hdrScene();
        View32 toning;
        toning.method = ToneMethod::HighlightCompression;
        std::string error;
        if (!convertSampleType(doc, SampleType::U16, &error, &toning)) check::fail(__FILE__, __LINE__, error);
        return hashImage16(*renderFlattened16(doc));
    });
    // Shapes, vector masks and fill layers (their coverage from the 16-bit rasterisers), and layer styles.
    scene("f32/shape/solid_ellipse", [] {
        Document doc = goldenBase();
        Layer layer = layerOf("shape", std::make_shared<Image>(1, 1), {0, 0});
        VectorShape ellipse;
        ellipse.path = ellipsePath(Rect(12.5, 8.25, 60, 44));
        ellipse.r = 40; ellipse.g = 120; ellipse.b = 220;
        ellipse.stroke.enabled = false;
        setVectorShape(layer, doc, ellipse);
        doc.layers.push_back(layer);
        return hashF(thirtyTwo(doc));
    });
    for (const char* name : {"drop_shadow", "outer_glow", "color_overlay", "gradient_overlay", "stroke", "bevel"}) {
        const std::string effect = name;
        scene("f32/style/" + effect, [effect] {
            LayerStyle s;
            if (effect == "drop_shadow") { DropShadow d; d.distance = 4; d.size = 5; s.dropShadows.push_back(d); }
            if (effect == "outer_glow") { OuterGlow g; g.size = 6; s.outerGlows.push_back(g); }
            if (effect == "color_overlay") { ColorOverlay c; c.color = {20, 180, 90}; c.opacity = 0.6f; s.colorOverlays.push_back(c); }
            if (effect == "gradient_overlay") { GradientOverlay g; g.gradient = twoStops({255, 0, 0}, {0, 0, 255}, 90); s.gradientOverlays.push_back(g); }
            if (effect == "stroke") { Stroke k; k.size = 3; s.strokes.push_back(k); }
            if (effect == "bevel") { Bevel b; b.size = 5; s.bevels.push_back(b); }
            Document doc = goldenBase();
            Layer top = layerOf("paint", paint(56, 40, 5), {20, 12});
            setLayerStyle(top, s);
            doc.layers.push_back(top);
            return hashF(thirtyTwo(doc));
        });
    }
}

// ---- adjustment layers in a document -----------------------------------------------------------------------------

AdjustmentSettings exampleAdjustment(AdjustmentKind kind) {
    AdjustmentSettings s = AdjustmentSettings::defaults(kind);
    switch (kind) {
    case AdjustmentKind::Levels:
        s.levels.ranges[0] = {20, 1.3, 230, 10, 245};
        s.levels.ranges[2].gamma = 0.8;
        break;
    case AdjustmentKind::Curves:
        s.curves.channels[0] = {{0, 0}, {64, 40}, {192, 220}, {255, 255}};
        s.curves.channels[3] = {{0, 20}, {255, 230}};
        break;
    case AdjustmentKind::HueSaturation:
        s.hsv.adjustments[0] = {25, 30, -10};
        s.hsv.adjustments[1] = {-15, -40, 5};
        break;
    case AdjustmentKind::Exposure: s.exposure = {0.7, -0.02, 1.2}; break;
    case AdjustmentKind::GradientMap: s.gradientMap.shadows = {0.1, 0, 0.3}; s.gradientMap.highlights = {1, 0.9, 0.5}; break;
    case AdjustmentKind::Grain: s.grain.amount = 40; s.grain.seed = 1234; break;
    case AdjustmentKind::BrightnessContrast: s.brightnessContrast = {30, 25, false}; break;
    case AdjustmentKind::Posterize: s.posterize.levels = 5; break;
    case AdjustmentKind::Threshold: s.threshold.level = 110; break;
    case AdjustmentKind::BlackWhite: s.blackWhite.tint = true; break;
    case AdjustmentKind::ColorBalance: s.colorBalance.ranges = {{{20, -10, 5}, {-15, 25, 10}, {5, 5, -30}}}; break;
    case AdjustmentKind::Vibrance: s.vibrance = {45, -20}; break;
    case AdjustmentKind::PhotoFilter: s.photoFilter.density = 60; break;
    case AdjustmentKind::ChannelMixer: s.channelMixer.rows[0] = {80, 30, -10, 5}; break;
    case AdjustmentKind::SelectiveColor: s.selectiveColor.ranges[0] = {-30, 20, 10, 0}; s.selectiveColor.ranges[7] = {10, 0, -15, 5}; break;
    case AdjustmentKind::Invert: case AdjustmentKind::ColorLookup: break;
    }
    return s;
}

void addAdjustmentScenes() {
    for (int k = 0; k < adjustmentKindCount; k++) {
        AdjustmentKind kind = AdjustmentKind(k);
        if (kind == AdjustmentKind::ColorLookup) continue;   // needs a LUT file (see the header)
        std::string name = slug(adjustmentKindName(kind));
        // Straight onto pixels.
        scene("adjust/" + name, [kind] {
            auto image = noisyBase(200, 150);
            Image& img = *image;
            // Partly transparent pixels too, where premultiplication matters.
            auto over = paint(200, 150, 7);
            for (int y = 0; y < 150; y++) for (int x = 0; x < 100; x++) std::memcpy(img.pixel(x, y), over->pixel(x, y), 4);
            NEED(applyAdjustment(exampleAdjustment(kind), img, Rect{0, 0, 200, 150}, 1));
            return hashImage(img);
        });
        // As a masked adjustment layer at 70% in a document, where the renderer applies it.
        scene("adjust_layer/" + name, [kind] {
            Document doc(200, 150);
            doc.id = "00000000-0000-4000-8000-000000000003";
            doc.layers.push_back(layerOf("base", noisyBase(200, 150), {0, 0}));
            doc.layers.push_back(layerOf("paint", paint(120, 100, 9), {40, 25}));
            Layer adj("Adjustment", doc.size());
            adj.adjustment = exampleAdjustment(kind).toLayerAdjustment();
            adj.opacity = 0.7;
            adj.mask = maskOf(radialMask(200, 150));
            doc.layers.push_back(adj);
            return hashImage(*renderFlattened(doc));
        });
    }
}

// ---- filters -----------------------------------------------------------------------------------------------------

std::shared_ptr<Image> filterInput() {
    auto image = noisyBase(220, 160, 11);
    auto over = paint(220, 160, 13);
    for (int y = 0; y < 160; y++) for (int x = 110; x < 220; x++) std::memcpy(image->pixel(x, y), over->pixel(x, y), 4);
    return image;
}

void addFilterScenes() {
    auto filter = [](FilterKind kind, FilterSettings s, uint32_t seed = 0) {
        auto image = filterInput();
        applyFilter(kind, *image, s, 1, seed);
        return hashImage(*image);
    };
    for (double r : {0.6, 2.0, 7.5, 30.0})
        scene("filter/gaussian_blur_" + std::to_string(int(r * 10)), [=] { FilterSettings s; s.radius = r; return filter(FilterKind::GaussianBlur, s); });
    for (double a : {0.0, 30.0, -90.0})
        scene("filter/motion_blur_" + std::to_string(int(a)), [=] { FilterSettings s; s.angle = a; s.distance = 15; return filter(FilterKind::MotionBlur, s); });
    scene("filter/add_noise_uniform", [=] { FilterSettings s; s.amount = 25; return filter(FilterKind::AddNoise, s, 42); });
    scene("filter/add_noise_gaussian_mono", [=] { FilterSettings s; s.amount = 40; s.gaussian = true; s.monochromatic = true; return filter(FilterKind::AddNoise, s, 7); });
    scene("filter/lens_correction", [=] { FilterSettings s; s.distortion = 35; return filter(FilterKind::LensCorrection, s); });
    scene("filter/lens_correction_bicubic", [=] { FilterSettings s; s.distortion = -40; s.bicubic = true; return filter(FilterKind::LensCorrection, s); });
    scene("filter/gaussian_blur_gray", [] {
        auto mask = radialMask(180, 140);
        gaussianBlur(*mask, 4.5);
        return hashGray(*mask);
    });
    scene("filter/sharpen", [] { auto image = filterInput(); sharpenImage(*image, 1.0); return hashImage(*image); });
    scene("filter/sharpen_r3", [] { auto image = filterInput(); sharpenImage(*image, 3.0); return hashImage(*image); });
    scene("filter/invert", [] { auto image = filterInput(); applyInvert(*image); return hashImage(*image); });
}

// ---- adjustments and filters at 16 bits (P3a) ---------------------------------------------------------------------

void add16BitEditScenes() {
    for (int k = 0; k < adjustmentKindCount; k++) {
        AdjustmentKind kind = AdjustmentKind(k);
        if (kind == AdjustmentKind::ColorLookup) continue;
        std::string name = slug(adjustmentKindName(kind));
        // The adjust/ scene's input converted to 16 bits, adjusted there.
        scene("u16/adjust/" + name, [kind] {
            auto image = noisyBase(200, 150);
            auto over = paint(200, 150, 7);
            for (int y = 0; y < 150; y++) for (int x = 0; x < 100; x++) std::memcpy(image->pixel(x, y), over->pixel(x, y), 4);
            auto deep = widenImage(*image);
            NEED(applyAdjustment(exampleAdjustment(kind), *deep, Rect{0, 0, 200, 150}, 1));
            return hashImage16(*deep);
        });
        // The adjust_layer/ document at 16 bits.
        scene("u16/adjust_layer/" + name, [kind] {
            Document doc(200, 150);
            doc.id = "00000000-0000-4000-8000-000000000003";
            doc.layers.push_back(layerOf("base", noisyBase(200, 150), {0, 0}));
            doc.layers.push_back(layerOf("paint", paint(120, 100, 9), {40, 25}));
            Layer adj("Adjustment", doc.size());
            adj.adjustment = exampleAdjustment(kind).toLayerAdjustment();
            adj.opacity = 0.7;
            adj.mask = maskOf(radialMask(200, 150));
            doc.layers.push_back(adj);
            return hash16(sixteen(doc));
        });
    }
    auto filter = [](FilterKind kind, FilterSettings s, uint32_t seed = 0) {
        auto image = widenImage(*filterInput());
        applyFilter(kind, *image, s, 1, seed);
        return hashImage16(*image);
    };
    for (double r : {0.6, 2.0, 7.5, 30.0})
        scene("u16/filter/gaussian_blur_" + std::to_string(int(r * 10)), [=] { FilterSettings s; s.radius = r; return filter(FilterKind::GaussianBlur, s); });
    for (double a : {0.0, 30.0, -90.0})
        scene("u16/filter/motion_blur_" + std::to_string(int(a)), [=] { FilterSettings s; s.angle = a; s.distance = 15; return filter(FilterKind::MotionBlur, s); });
    scene("u16/filter/add_noise_uniform", [=] { FilterSettings s; s.amount = 25; return filter(FilterKind::AddNoise, s, 42); });
    scene("u16/filter/add_noise_gaussian_mono", [=] { FilterSettings s; s.amount = 40; s.gaussian = true; s.monochromatic = true; return filter(FilterKind::AddNoise, s, 7); });
    scene("u16/filter/lens_correction", [=] { FilterSettings s; s.distortion = 35; return filter(FilterKind::LensCorrection, s); });
    scene("u16/filter/lens_correction_bicubic", [=] { FilterSettings s; s.distortion = -40; s.bicubic = true; return filter(FilterKind::LensCorrection, s); });
    scene("u16/filter/gaussian_blur_gray", [] {
        auto mask = widenGray(*radialMask(180, 140));
        gaussianBlur(*mask, 4.5);
        Fnv f;
        f.u32(uint32_t(mask->width())); f.u32(uint32_t(mask->height()));
        for (int y = 0; y < mask->height(); y++)
            for (int x = 0; x < mask->width(); x++) { const uint16_t v = mask->at(x, y); const uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)}; f.bytes(b, 2); }
        return f.h;
    });
    scene("u16/filter/invert", [] { auto image = widenImage(*filterInput()); applyInvert(*image); return hashImage16(*image); });
}

// ---- editing at 32 bits (P5b) ------------------------------------------------------------------------------------

uint64_t hashGrayF(const GrayF& gray) {
    Fnv f;
    f.u32(uint32_t(gray.width()));
    f.u32(uint32_t(gray.height()));
    f.u32(0x31326266u);   // "1bf2"
    for (int y = 0; y < gray.height(); y++) f.bytes(reinterpret_cast<const uint8_t*>(gray.row(y)), size_t(gray.width()) * 4);
    return f.h;
}

/// The adjust/ scene's input linearised, with a bright float strip (light up to 5) across its lower rows.
std::shared_ptr<ImageF> floatEditInput() {
    auto image = noisyBase(200, 150);
    auto over = paint(200, 150, 7);
    for (int y = 0; y < 150; y++) for (int x = 0; x < 100; x++) std::memcpy(image->pixel(x, y), over->pixel(x, y), 4);
    auto deep = lineariseImage(*image, TransferCurve::srgb());
    for (int y = 120; y < 150; y++)
        for (int x = 0; x < 200; x++) {
            float* p = deep->pixel(x, y);
            p[0] = 5.0f * float(x) / 199.0f; p[1] = 2.5f; p[2] = 0.25f + float(y - 120) / 10.0f; p[3] = 1.0f;
        }
    return deep;
}

void add32BitEditScenes() {
    for (int k = 0; k < adjustmentKindCount; k++) {
        const AdjustmentKind kind = AdjustmentKind(k);
        if (kind == AdjustmentKind::ColorLookup || !adjustmentAt32(kind)) continue;
        const std::string name = slug(adjustmentKindName(kind));
        scene("f32/adjust/" + name, [kind] {
            auto deep = floatEditInput();
            NEED(applyAdjustment(exampleAdjustment(kind), *deep, Rect{0, 0, 200, 150}, 1, TransferCurve::srgb()));
            return hashImageF(*deep);
        });
        scene("f32/adjust_layer/" + name, [kind] {
            Document doc(200, 150);
            doc.id = "00000000-0000-4000-8000-000000000032";
            doc.layers.push_back(layerOf("base", noisyBase(200, 150), {0, 0}));
            doc.layers.push_back(layerOf("paint", paint(120, 100, 9), {40, 25}));
            Layer adj("Adjustment", doc.size());
            adj.adjustment = exampleAdjustment(kind).toLayerAdjustment();
            adj.opacity = 0.7;
            adj.mask = maskOf(radialMask(200, 150));
            doc.layers.push_back(adj);
            return hashF(thirtyTwo(doc));
        });
    }
    // A kind Photoshop lacks at 32 bits is kept but not drawn: the same as the document without it.
    scene("f32/adjust_layer/posterize_not_drawn", [] {
        Document doc(200, 150);
        doc.layers.push_back(layerOf("base", noisyBase(200, 150), {0, 0}));
        Layer adj("Adjustment", doc.size());
        adj.adjustment = exampleAdjustment(AdjustmentKind::Posterize).toLayerAdjustment();
        doc.layers.push_back(adj);
        return hashF(thirtyTwo(doc));
    });
    // Twelve layers with adjustment layers between them (the bench's scene, small).
    scene("f32/adjust_layer/stack", [] {
        Document doc(200, 150);
        doc.layers.push_back(layerOf("base", noisyBase(200, 150), {0, 0}));
        for (int i = 0; i < 12; i++) {
            doc.layers.push_back(layerOf("paint", paint(90, 70, uint32_t(20 + i)), {double(i * 9), double(i * 6)}));
            if (i % 4 == 3) {
                Layer adj("Adjustment", doc.size());
                adj.adjustment = exampleAdjustment(i == 3 ? AdjustmentKind::Levels : i == 7 ? AdjustmentKind::HueSaturation : AdjustmentKind::Curves).toLayerAdjustment();
                adj.opacity = 0.8;
                doc.layers.push_back(adj);
            }
        }
        return hashF(thirtyTwo(doc));
    });
    auto filter = [](FilterKind kind, FilterSettings s, uint32_t seed = 0) {
        auto image = lineariseImage(*filterInput(), TransferCurve::srgb());
        applyFilter(kind, *image, s, TransferCurve::srgb(), 1, seed);
        return hashImageF(*image);
    };
    for (double r : {0.6, 2.0, 7.5, 30.0})
        scene("f32/filter/gaussian_blur_" + std::to_string(int(r * 10)), [=] { FilterSettings s; s.radius = r; return filter(FilterKind::GaussianBlur, s); });
    for (double a : {0.0, 30.0, -90.0})
        scene("f32/filter/motion_blur_" + std::to_string(int(a)), [=] { FilterSettings s; s.angle = a; s.distance = 15; return filter(FilterKind::MotionBlur, s); });
    scene("f32/filter/add_noise_uniform", [=] { FilterSettings s; s.amount = 25; return filter(FilterKind::AddNoise, s, 42); });
    scene("f32/filter/add_noise_gaussian_mono", [=] { FilterSettings s; s.amount = 40; s.gaussian = true; s.monochromatic = true; return filter(FilterKind::AddNoise, s, 7); });
    scene("f32/filter/lens_correction", [=] { FilterSettings s; s.distortion = 35; return filter(FilterKind::LensCorrection, s); });
    scene("f32/filter/lens_correction_bicubic", [=] { FilterSettings s; s.distortion = -40; s.bicubic = true; return filter(FilterKind::LensCorrection, s); });
    scene("f32/filter/gaussian_blur_hdr", [] { auto image = floatEditInput(); gaussianBlur(*image, 3.5); return hashImageF(*image); });
    scene("f32/filter/invert", [] { auto image = lineariseImage(*filterInput(), TransferCurve::srgb()); applyInvert(*image, TransferCurve::srgb()); return hashImageF(*image); });
    // Selections in float: feather, expand, border, smooth.
    auto selection = [] { return widenGrayF(*radialMask(180, 140)); };
    scene("f32/selection/feather", [=] { return hashGrayF(*featherSelection(*selection(), 4.5)); });
    scene("f32/selection/expand", [=] { return hashGrayF(*growSelection(*selection(), 6)); });
    scene("f32/selection/contract", [=] { return hashGrayF(*growSelection(*selection(), -6)); });
    scene("f32/selection/border", [=] { return hashGrayF(*borderSelection(*selection(), 5)); });
    scene("f32/selection/smooth", [=] { return hashGrayF(*smoothSelection(*selection(), 3)); });
    // Image Size, Distort and Warp of a layer at 32 bits.
    for (Sampling sampling : {Sampling::Nearest, Sampling::Smooth, Sampling::High})
        scene(std::string("f32/edit/image_size_") + (sampling == Sampling::Nearest ? "nearest" : sampling == Sampling::Smooth ? "smooth" : "high"), [sampling] {
            Document doc = goldenBase();
            Layer top = layerOf("gradient", gradient(64, 48), {16, 8});
            top.transform.rotation = 15;
            doc.layers.push_back(top);
            Document f = thirtyTwo(doc);
            NEED(resizeDocument(f, 150, 97, 144, sampling));
            return hashF(f);
        });
    scene("f32/edit/distort", [] {
        auto image = floatEditInput();
        const LayerTransform t(Point(10, 10), Size(200, 150));
        auto warped = warpImage(ImageFPtr(image), t, Corners{Point(20, 5), Point(200, 30), Point(190, 160), Point(5, 140)});
        NEED(bool(warped));
        return hashImageF(*warped->image);
    });
    scene("f32/edit/warp_arc", [] {
        Document doc = thirtyTwo(goldenBase());
        Layer& layer = doc.layers.back();
        std::string error;
        NEED(warpLayer(doc, layer, TextWarp{"warpArc", 35, 0, 0, false}, &error));
        return hashF(doc);
    });
}

// ---- smart objects at 16 bits ------------------------------------------------------------------------------------

/// A 16-bit document over a busy base placing `source` on a turned, scaled quad-free transform.
Document smartObjectScene(const std::shared_ptr<const SmartObjectSource>& source) {
    Document doc = sixteen([] {
        Document d(200, 150);
        d.id = "00000000-0000-4000-8000-000000000016";
        d.layers.push_back(layerOf("base", noisyBase(200, 150), {0, 0}));
        return d;
    }());
    doc.smartObjects[source->id] = source;
    doc.layers.push_back(smartObjectLayer(source, {40, 30, 150, 30, 150, 110, 40, 110}, "Contents", doc.sampleType));
    return doc;
}

std::shared_ptr<const SmartObjectSource> smartSource8() {
    SmartObjectContents c;
    c.image = ImagePtr(paint(110, 80, 5));
    encodePngImage(*c.image.u8(), c.bytes);
    c.fileName = "paint.png";
    return makeSmartObjectSource(std::move(c));
}

std::shared_ptr<const SmartObjectSource> smartSource16() {
    auto ramp = std::make_shared<Image16>(110, 80);
    for (int y = 0; y < 80; y++)
        for (int x = 0; x < 110; x++) {
            uint16_t* p = ramp->pixel(x, y);
            const uint16_t a = uint16_t(x < 8 ? x * 4096 : 32768);
            p[0] = uint16_t(uint32_t(x * 297) * a >> 15); p[1] = uint16_t(uint32_t(y * 409) * a >> 15); p[2] = uint16_t(uint32_t(12345) * a >> 15); p[3] = a;
        }
    SmartObjectContents c;
    c.image = Image16Ptr(ramp);
    encodePngImage16(*ramp, c.bytes);
    c.fileName = "ramp.png";
    return makeSmartObjectSource(std::move(c));
}

void add16BitSmartObjectScenes() {
    scene("u16/smart_object/placed_8bit_source", [] { return hash16(smartObjectScene(smartSource8())); });
    scene("u16/smart_object/placed_16bit_source", [] { return hash16(smartObjectScene(smartSource16())); });
    scene("u16/smart_object/turned@0.5", [] {
        Document doc = smartObjectScene(smartSource16());
        doc.layers[1].transform.rotation = 20;
        return hash16(doc, reducedRegion());
    });
    scene("u16/smart_object/warped", [] {
        Document doc = smartObjectScene(smartSource8());
        std::string error;
        if (!warpLayer(doc, doc.layers[1], TextWarp{"warpArc", 40, 0, 0, false}, &error)) check::fail(__FILE__, __LINE__, "warp: " + error);
        return hash16(doc);
    });
    // Every Smart Filter drawn at 16 bits, over an 8-bit source placed in a 16-bit document.
    const std::vector<std::pair<std::string, SmartFilterParameters>> filters{
        {"gaussian_blur", smartfilter::GaussianBlur{3}}, {"high_pass", smartfilter::HighPass{4}}, {"median", smartfilter::Median{2}},
        {"dust_and_scratches", smartfilter::DustAndScratches{2, 10}}, {"surface_blur", smartfilter::SurfaceBlur{5, 15}},
        {"surface_blur_wide", smartfilter::SurfaceBlur{12, 30}}, {"motion_blur", smartfilter::MotionBlur{30, 14}},
        {"plastic_wrap", smartfilter::PlasticWrap{9, 7, 5}}, {"mosaic", smartfilter::Mosaic{8}}, {"emboss", smartfilter::Emboss{135, 3, 100}},
        {"box_blur", smartfilter::BoxBlur{4}}, {"radial_blur", smartfilter::RadialBlur{10, 16}}, {"add_noise", smartfilter::AddNoise{20, true, false, 5}},
        {"unsharp_mask", smartfilter::UnsharpMask{150, 2, 8}},
    };
    for (const auto& [name, parameters] : filters)
        scene("u16/smart_filter/" + name, [parameters] {
            Document doc = smartObjectScene(smartSource8());
            SmartFilterEntry entry;
            entry.parameters = parameters;
            std::string error;
            if (!addSmartFilter(doc, doc.layers[1], entry, &error)) check::fail(__FILE__, __LINE__, "addSmartFilter: " + error);
            return hash16(doc);
        });
    // A stack: a blend and opacity per entry and a shared mask, over the 16-bit source.
    scene("u16/smart_filter/stack_blend_mask", [] {
        Document doc = smartObjectScene(smartSource16());
        SmartFilterStack stack;
        stack.supported = true;
        SmartFilterEntry blur, mosaic;
        blur.parameters = smartfilter::GaussianBlur{2.5};
        mosaic.parameters = smartfilter::Mosaic{6};
        mosaic.opacity = 0.6;
        mosaic.blend = BlendMode::Screen;
        stack.entries = {blur, mosaic};
        auto mask = std::make_shared<GrayImage>(200, 150);
        for (int y = 0; y < 150; y++) for (int x = 0; x < 200; x++) mask->at(x, y) = uint8_t(std::min(255, x + y));
        stack.mask = mask;
        stack.maskBounds = PixelRect{0, 0, 200, 150};
        std::string error;
        if (!setSmartFilters(doc, doc.layers[1], stack, &error)) check::fail(__FILE__, __LINE__, "setSmartFilters: " + error);
        return hash16(doc);
    });
}

// ---- brushes -----------------------------------------------------------------------------------------------------

Layer paper(int w, int h) {
    auto image = std::make_shared<Image>(w, h);
    image->fill(255, 255, 255, 255);
    return Layer(Asset::make(image, "Paper"), Point(0, 0));
}

BrushSettings colour(double diameter, double hardness = 1, double opacity = 1) {
    BrushSettings s;
    s.diameter = diameter;
    s.hardness = hardness;
    s.opacity = opacity;
    s.red = 0.8; s.green = 0.2; s.blue = 0.4;
    return s;
}

std::vector<Point> wave(double x0, double x1, double y, double amp) {
    std::vector<Point> pts;
    for (double x = x0; x <= x1; x += 3) pts.push_back({x, y + amp * std::sin(x / 17.0)});
    return pts;
}

uint64_t hashCommit(BrushStroke& grid) {
    grid.flush();
    auto commit = grid.commit();
    NEED(commit.asset && commit.asset->image.u8());
    Fnv f;
    f.h = hashImage(*commit.asset->image.u8());
    // Where the result lands matters as much as its pixels.
    f.u32(uint32_t(std::lround(commit.transform.origin.x * 64)));
    f.u32(uint32_t(std::lround(commit.transform.origin.y * 64)));
    return f.h;
}

void addBrushScenes() {
    scene("brush/round_hard", [] {
        Layer layer = paper(240, 160);
        BrushStroke grid(layer, false, colour(18), Size(240, 160));
        for (Point p : wave(20, 220, 80, 40)) grid.append(p);
        return hashCommit(grid);
    });
    scene("brush/round_soft_half", [] {
        Layer layer = paper(240, 160);
        BrushStroke grid(layer, false, colour(30, 0.2, 0.5), Size(240, 160));
        grid.appendAll(wave(20, 220, 70, 30));
        return hashCommit(grid);
    });
    scene("brush/round_unstamped", [] {
        Layer layer = paper(240, 160);
        BrushSettings s = colour(23.5, 0.6, 0.8);
        s.stampedDabs = false;
        BrushStroke grid(layer, false, s, Size(240, 160));
        grid.appendAll(wave(15, 225, 90, 45));
        return hashCommit(grid);
    });
    scene("brush/eraser_selection", [] {
        Layer layer = layerOf("paint", noisyBase(240, 160), {0, 0});
        GrayImage selection(240, 160, 0);
        for (int y = 30; y < 130; y++) for (int x = 0; x < 240; x++) selection.at(x, y) = uint8_t(std::min(255, (y - 30) * 6));
        BrushSettings s = colour(26, 0.5);
        s.erasing = true;
        BrushStroke grid(layer, false, s, Size(240, 160), &selection);
        grid.appendAll(wave(10, 230, 80, 50));
        return hashCommit(grid);
    });
    scene("brush/mask_paint", [] {
        Layer layer = layerOf("paint", noisyBase(200, 140), {0, 0});
        layer.mask = maskOf(std::make_shared<GrayImage>(200, 140, 255));
        BrushSettings s = colour(22, 0.3);
        s.maskValue = 0;
        BrushStroke grid(layer, true, s, Size(200, 140));
        grid.appendAll(wave(10, 190, 70, 40));
        grid.flush();
        auto commit = grid.commit();
        NEED(commit.mask && commit.mask->image);
        return hashGray(*commit.mask->image.u8());
    });
    auto tipStroke = [](BrushTip tip, double diameter, bool pressure) {
        Layer layer = paper(260, 160);
        BrushSettings s = colour(diameter);
        BrushStroke grid(layer, false, s, Size(260, 160));
        TipStroke stroke(grid, tip, diameter, 99);
        NEED(stroke.isValid());
        int i = 0;
        for (Point p : wave(20, 240, 80, 45)) {
            BrushSample input;
            input.position = p;
            input.stylus = true;
            input.pressure = pressure ? 0.2 + 0.8 * std::fabs(std::sin(i++ * 0.13)) : 1.0;
            stroke.strokeTo(input);
        }
        return hashCommit(grid);
    };
    scene("brush/tip_square", [=] {
        BrushTip tip;
        auto shape = std::make_shared<GrayImage>(48, 48, 255);
        for (int y = 0; y < 24; y++) for (int x = 0; x < 24; x++) shape->at(x, y) = 0;
        tip.shape = shape;
        tip.spacing = 0.15;
        tip.followStroke = true;
        return tipStroke(tip, 24, false);
    });
    scene("brush/tip_jitter_grain", [=] {
        BrushTip tip;
        tip.shape = radialMask(64, 64);
        auto grain = std::make_shared<GrayImage>(32, 32, 0);
        for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) grain->at(x, y) = uint8_t(mix(uint32_t(y * 32 + x)) & 255);
        tip.grain = grain;
        tip.spacing = 0.1;
        tip.dynamics = legacyDynamics({.sizeJitter = 0.4, .flowJitter = 0.3, .angleJitter = 60, .pressureSize = 1, .minimumSize = 0.2, .pressureFlow = 0.5});
        tip.scatter = 0.5;
        tip.count = 2;
        tip.roundness = 0.6;
        return tipStroke(tip, 30, true);
    });
    // MyPaint is exact across runs (libmypaint's jitter is seeded; mypaint_tests checks that). Its output also
    // depends on the libmypaint build, so a different library version may need the baseline regenerated.
    if (myPaintSupported()) {
        for (const char* preset : {"classic/pencil", "classic/charcoal", "deevad/watercolor_glazing"}) {
            std::string file = std::string(MYPAINT_BRUSHES_DIR) + "/" + preset + ".myb";
            if (!fs::exists(file)) continue;
            scene(std::string("mypaint/") + preset, [file] {
                std::ifstream in(file);
                std::stringstream text;
                text << in.rdbuf();
                Layer layer = paper(240, 140);
                BrushSettings s = colour(14);
                BrushStroke grid(layer, false, s, Size(240, 140));
                MyPaintStroke stroke(grid, text.str(), s);
                NEED(stroke.isValid());
                int i = 0;
                for (Point p : wave(20, 220, 70, 40)) {
                    BrushSample input;   // a pen, events exactly 1/60 s apart
                    input.position = p;
                    input.stylus = true;
                    input.pressure = 0.3 + 0.6 * std::fabs(std::sin(i++ * 0.1));
                    input.dt = 1.0 / 60;
                    stroke.strokeTo(input);
                }
                stroke.finish();
                return hashCommit(grid);
            });
        }
    }
}

// ---- baseline ----------------------------------------------------------------------------------------------------

std::string hex(uint64_t h) { char b[17]; std::snprintf(b, sizeof b, "%016" PRIx64, h); return b; }

std::map<std::string, std::string> readBaseline(const std::string& path) {
    std::map<std::string, std::string> out;
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream fields(line);
        std::string name, value;
        if (fields >> name >> value) out[name] = value;
    }
    return out;
}

// ---- Painting at 32 bits and in CMYK and Lab (P5c, P7 E) -------------------------------------------------------------
//
// Strokes committed into the blend scene's top pixel layer, then the document rendered at its own layout: "paint/f32/"
// (brush, soft brush over light above white, eraser, clone, gradient, healing, a moved selection) and "paint/cmyk/",
// "paint/cmyk16/", "paint/lab/", "paint/lab16/" (eraser, clone, a moved selection). The CMYK and
// Lab scenes take no colour through Little CMS's float transform, whose last bit follows the system's libm (the colour
// conversion is checked against Little CMS in paint_modes_tests instead).

/// The index of the blend scene's top pixel layer (what the strokes paint).
size_t paintTarget(const Document& doc) {
    for (size_t i = doc.layers.size(); i-- > 0;)
        if (doc.layers[i].asset && doc.layers[i].asset->image && !doc.layers[i].isGroup) return i;
    return 0;
}

/// Runs `stroke` on the target layer and commits it, as the app's commitRasterEdit does.
void paintInto(Document& doc, const BrushSettings& settings, const std::function<void(BrushStroke&)>& stroke) {
    Layer& layer = doc.layers[paintTarget(doc)];
    BrushStroke raster(layer, false, settings, doc);
    if (!raster.isValid()) { check::fail(__FILE__, __LINE__, "stroke: " + raster.error()); return; }
    stroke(raster);
    BrushStroke::Commit commit = raster.commit();
    if (commit.asset) { layer.asset = commit.asset; layer.transform = commit.transform; }
}

BrushSettings strokeSettings(double diameter, double hardness, double opacity) {
    BrushSettings s;
    s.diameter = diameter; s.hardness = hardness; s.opacity = opacity;
    s.red = 0.9; s.green = 0.35; s.blue = 0.1;
    return s;
}

uint64_t hashPainted(const Document& doc) {
    if (doc.sampleType == SampleType::F32) return hashF(doc);
    return hashNative(renderNative(doc));
}

void addPaintScenes() {
    const std::vector<Point> path = {{20, 30}, {90, 60}, {160, 40}, {230, 110}};
    auto strokeAlong = [path](BrushStroke& s) { for (const Point& p : path) s.append(p); };
    auto moved = [](Document& doc) {
        auto selection = std::make_shared<GrayImage>(doc.width, doc.height);
        for (int y = 40; y < 120; y++) for (int x = 60; x < 150; x++) selection->at(x, y) = uint8_t(std::min(255, (x - 60) * 6));
        Selection s;
        s.coverage = GrayPtr(selection);
        if (doc.sampleType == SampleType::U16) s.coverage = Gray16Ptr(widenGray(*selection));
        if (doc.sampleType == SampleType::F32) {
            auto f = std::make_shared<GrayF>(doc.width, doc.height);
            for (int y = 0; y < doc.height; y++) for (int x = 0; x < doc.width; x++) f->at(x, y) = selection->at(x, y) / 255.0f;
            s.coverage = GrayFPtr(f);
        }
        doc.selection = s;
        paintInto(doc, BrushSettings(), [](BrushStroke& r) { if (r.liftSelection()) r.moveLifted({37, -11}, false); });
        doc.selection.reset();
    };
    auto cloned = [strokeAlong](Document& doc) {
        CloneSource source;
        source.setImage(doc.sampleType == SampleType::F32 ? AnyImage(ImageFPtr(renderFlattenedF(doc))) : renderNative(doc));
        source.offset = {-15, 25};
        Layer& layer = doc.layers[paintTarget(doc)];
        BrushStroke raster(layer, false, strokeSettings(24, 0.5, 0.8), doc);
        raster.setClone(source);
        strokeAlong(raster);
        BrushStroke::Commit commit = raster.commit();
        if (commit.asset) { layer.asset = commit.asset; layer.transform = commit.transform; }
    };

    // 32 bits: the colour linearised, light above white under a soft brush, the healers on the 15-bit encoding.
    scene("paint/f32/brush_hard", [strokeAlong] {
        Document doc = thirtyTwo(blendDocument(BlendMode::Normal));
        paintInto(doc, strokeSettings(18, 1, 1), strokeAlong);
        return hashPainted(doc);
    });
    scene("paint/f32/brush_soft_over_hdr", [strokeAlong] {
        Document doc = hdrScene();
        paintInto(doc, strokeSettings(40, 0, 0.6), strokeAlong);
        return hashPainted(doc);
    });
    scene("paint/f32/eraser", [strokeAlong] {
        Document doc = thirtyTwo(blendDocument(BlendMode::Normal));
        BrushSettings s = strokeSettings(30, 0.3, 0.7);
        s.erasing = true;
        paintInto(doc, s, strokeAlong);
        return hashPainted(doc);
    });
    scene("paint/f32/clone", [cloned] { Document doc = thirtyTwo(blendDocument(BlendMode::Normal)); cloned(doc); return hashPainted(doc); });
    scene("paint/f32/gradient", [] {
        Document doc = thirtyTwo(blendDocument(BlendMode::Normal));
        GradientStops stops;
        stops.colors = {GradientColorStop{0, {0.9f, 0.2f, 0.1f}, 0.5f}, GradientColorStop{0.6f, {0.1f, 0.3f, 0.9f}, 0.3f}, GradientColorStop{1, {1, 1, 1}, 0.5f}};
        stops.alphas = {GradientAlphaStop{0, 1, 0.5f}, GradientAlphaStop{1, 0.2f, 0.5f}};
        paintInto(doc, BrushSettings(), [&](BrushStroke& r) { r.fillGradientOver(1, {120, 90}, {220, 90}, stops, 0.8); });
        return hashPainted(doc);
    });
    scene("paint/f32/spot_healing", [] {
        Document doc = thirtyTwo(blendDocument(BlendMode::Normal));
        BrushSettings s = strokeSettings(16, 1, 1);
        s.healing = true;
        s.healingMode = 2;   // Proximity Match: deterministic without a seed
        s.healingSeed = 3;
        paintInto(doc, s, [](BrushStroke& r) { r.append({100, 70}); r.append({110, 74}); });
        return hashPainted(doc);
    });
    scene("paint/f32/moved_selection", [moved] { Document doc = thirtyTwo(blendDocument(BlendMode::Normal)); moved(doc); return hashPainted(doc); });

    for (ColorMode colorMode : {ColorMode::CMYK, ColorMode::Lab})
        for (SampleType type : {SampleType::U8, SampleType::U16}) {
            const std::string prefix = std::string("paint/") + (colorMode == ColorMode::CMYK ? "cmyk" : "lab") + (type == SampleType::U16 ? "16" : "") + "/";
            scene(prefix + "eraser", [=] {
                Document doc = inColorMode(blendDocument(BlendMode::Normal), colorMode, type);
                BrushSettings s = strokeSettings(30, 0.3, 0.7);
                s.erasing = true;
                paintInto(doc, s, strokeAlong);
                return hashPainted(doc);
            });
            scene(prefix + "clone", [=] { Document doc = inColorMode(blendDocument(BlendMode::Normal), colorMode, type); cloned(doc); return hashPainted(doc); });
            scene(prefix + "moved_selection", [=] { Document doc = inColorMode(blendDocument(BlendMode::Normal), colorMode, type); moved(doc); return hashPainted(doc); });
        }
}

} // namespace

// ---- 16 bits: Camera Raw, Remove Background's matte, artboards and the timeline ------------------------------------

void add16BitLateScenes() {
    auto cameraRaw = [](std::function<void(CameraRawSettings&)> set) {
        return [set] {
            auto image = widenImage(*filterInput());
            CameraRawSettings s;
            set(s);
            NEED(applyCameraRaw(*image, s, 1, 3));
            return hashImage16(*image);
        };
    };
    scene("u16/camera_raw/light", cameraRaw([](CameraRawSettings& s) { s.exposure = 0.6; s.contrast = 25; s.shadows = 40; s.vibrance = 30; }));
    scene("u16/camera_raw/color", cameraRaw([](CameraRawSettings& s) {
        s.curve.rgb = CameraRawCurveSettings::mediumContrast(); s.mixer.hue[1] = 20; s.grading.shadows = {220, 30, 0}; }));
    scene("u16/camera_raw/effects", cameraRaw([](CameraRawSettings& s) { s.clarity = 30; s.dehaze = 20; s.vignetteAmount = -30; s.grainAmount = 20; }));
    scene("u16/camera_raw/detail_optics", cameraRaw([](CameraRawSettings& s) {
        s.detail.sharpenAmount = 60; s.detail.noiseLuminance = 30; s.optics.distortion = 20; s.optics.removeChromaticAberration = true; }));
    scene("u16/camera_raw/geometry", cameraRaw([](CameraRawSettings& s) { s.geometry.rotate = 4; s.geometry.vertical = 15; }));
    // Artboards at 16 bits: a coloured and a white background, a child clipped at an artboard's edge, a soft layer
    // across both; and the timeline's second frame (the child hidden, the soft layer moved).
    auto artboards = [] {
        Document doc = goldenBase();
        Layer board("Artboard 1", doc.size());
        board.isGroup = true;
        Artboard a;
        a.x = 20; a.y = 10; a.width = 90; a.height = 70;
        a.background = Artboard::Other;
        a.red = 0.2; a.green = 0.5; a.blue = 0.8;
        board.artboard = a;
        Layer child = layerOf("paint", paint(64, 48, 3), {70, 30});   // reaches past the right edge (110)
        child.parentId = board.id;
        Layer second("Artboard 2", doc.size());
        second.isGroup = true;
        Artboard b;
        b.x = 130; b.y = 20; b.width = 60; b.height = 50;
        b.background = Artboard::White;
        second.artboard = b;
        Layer soft = layerOf("soft", paint(80, 40, 5), {100, 40});
        soft.opacity = 0.6;
        soft.parentId = second.id;
        std::vector<Layer> layers = doc.layers;
        layers.insert(layers.end(), {child, board, soft, second});
        doc.layers = layers;
        return sixteen(doc);
    };
    scene("u16/artboard/backgrounds_and_clipping", [artboards] { return hash16(artboards()); });
    scene("u16/timeline/second_frame", [artboards] {
        Document doc = artboards();
        ensureAnimation(doc);
        NEED(duplicateFrame(doc, 0));
        for (const Layer& l : doc.layers) {
            if (l.name == "paint") doc.animation.frames[1].layers[l.id].visible = false;
            if (l.name == "soft") doc.animation.frames[1].layers[l.id].position = l.transform.origin + Point(-30, 12);
        }
        applyFrame(doc, doc.animation.frames[1]);
        return hash16(doc);
    });
    // Exporting from a 16-bit document: an artboard's rectangle as Export Artboards renders it, and the SVG's markup.
    scene("u16/export/artboard_rect", [artboards] {
        Image16 out;
        RenderOptions options;
        options.region = {20, 10, 90, 70};
        render16(artboards(), options, out);
        return hashImage16(out);
    });
    scene("u16/export/svg", [artboards] {
        // The markup, without the PNG payloads (their bytes depend on the zlib build; the pixels are the scene above).
        std::string text = writeSvg(artboards());
        const std::string marker = "base64,";
        for (size_t at = text.find(marker); at != std::string::npos; at = text.find(marker, at + marker.size()))
            text.erase(at + marker.size(), text.find('"', at) - at - marker.size());
        Fnv f;
        f.bytes(reinterpret_cast<const uint8_t*>(text.data()), text.size());
        return f.h;
    });
    // Remove Background at 16 bits: a coarse mask refined on the float plane (guided filter, matting, cleanup) against
    // the 8-bit guide, the 16-bit layer decontaminated with it, and the 16-bit mask laid over it.
    scene("u16/remove_background/refined_and_decontaminated", [] {
        auto guide = filterInput();
        const int w = guide->width(), h = guide->height();
        GrayImage coarse(w, h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) coarse.at(x, y) = uint8_t(std::clamp(int(std::lround((std::hypot(x - w / 2.0, y - h / 2.0) - w / 4.0) * -20)), 0, 255));
        MatteSettings s;
        s.matting = 12;
        const AlphaPlane plane = refineMatte(AlphaPlane(coarse), *guide, s, 0);
        auto out = estimateForeground(*widenImage(*guide), plane);
        const auto mask = plane.toGray16();
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                for (int c = 0; c < 4; c++) out->pixel(x, y)[c] = uint16_t(mul15(out->pixel(x, y)[c], mask->at(x, y)));
        return hashImage16(*out);
    });
}

TEST_CASE(render_hashes_match_the_baseline_on_the_pool_and_serially) {
    addGoldenScenes();
    addBlendScenes();
    addColorModeScenes();
    addAdjustmentScenes();
    addFilterScenes();
    addBrushScenes();
    add16BitScenes();
    add16BitEditScenes();
    add16BitVectorScenes();
    add16BitSmartObjectScenes();
    add16BitLateScenes();
    add32BitScenes();
    add32BitEditScenes();
    addPaintScenes();

    std::map<std::string, std::string> actual;
    int threadMismatch = 0;
    for (auto& [name, body] : scenes()) {
        uint64_t pooled = body();
        uint64_t serial = serially(body);
        if (pooled != serial) {
            std::fprintf(stderr, "  %s: %s on %d threads, %s serially\n", name.c_str(), hex(pooled).c_str(), workerCount(), hex(serial).c_str());
            threadMismatch++;
        }
        actual[name] = hex(pooled);
    }
    CHECK_EQ(threadMismatch, 0);
    std::fprintf(stderr, "  %zu scenes hashed (%d worker threads)\n", actual.size(), workerCount());

    const std::string path = RENDER_HASHES_FILE;
    if (std::getenv("COMPOSITOR_UPDATE_RENDER_HASHES")) {
        std::ofstream out(path);
        out << "# Render hashes (FNV-1a 64 over width, height and RGBA/gray bytes), from tests/render_hash_tests.cpp.\n"
               "# Regenerate after an intentional rendering change: COMPOSITOR_UPDATE_RENDER_HASHES=1 build/tests/render_hash_tests\n";
        for (auto& [name, h] : actual) out << name << ' ' << h << '\n';
        std::fprintf(stderr, "  wrote %s\n", path.c_str());
        return;
    }
    auto expected = readBaseline(path);
    REQUIRE(!expected.empty());
    int changed = 0, added = 0, missing = 0, unchecked = 0;
    for (auto& [name, h] : actual) {
#ifdef _WIN32
        // 32-bit scenes render through float pow/exp, which MinGW's maths library rounds differently from glibc's; their
        // kernels are checked within a tolerance everywhere (float_reference), and bit for bit on Linux here.
        if (name.rfind("f32/", 0) == 0 || name.find("/f32/") != std::string::npos) { unchecked++; continue; }
#endif
        auto it = expected.find(name);
        if (it == expected.end()) { std::fprintf(stderr, "  new      %s %s\n", name.c_str(), h.c_str()); added++; }
        else if (it->second != h) { std::fprintf(stderr, "  changed  %s %s -> %s\n", name.c_str(), it->second.c_str(), h.c_str()); changed++; }
    }
    for (auto& [name, h] : expected)
        if (!actual.count(name)) {
            // MyPaint scenes exist only in builds with libmypaint.
            if (name.rfind("mypaint/", 0) == 0 && !myPaintSupported()) continue;
            std::fprintf(stderr, "  missing  %s %s\n", name.c_str(), h.c_str());
            missing++;
        }
    if (changed || added || missing)
        std::fprintf(stderr, "  %d changed, %d new, %d missing, %d not compared on this platform; COMPOSITOR_UPDATE_RENDER_HASHES=1 rewrites %s\n", changed, added, missing, unchecked, path.c_str());
    CHECK_EQ(changed, 0);
    CHECK_EQ(added, 0);
    CHECK_EQ(missing, 0);
}

/// How far the 16-bit render of a document, reduced to 8 bits, is from its 8-bit render: the largest difference in
/// 8-bit levels and the share of samples more than a level apart.
struct Apart { int worst = 0; double beyondOne = 0; };
Apart levelsApart(const Document& doc, const RenderOptions& options) {
    Image eight;
    render(doc, options, eight);
    Image16 deep;
    render16(sixteen(doc), options, deep);
    auto reduced = narrowImage(deep);
    if (reduced->width() != eight.width() || reduced->height() != eight.height()) return {256, 1};
    Apart apart;
    size_t beyond = 0, total = 0;
    for (int y = 0; y < eight.height(); y++)
        for (int i = 0; i < eight.width() * 4; i++, total++) {
            const int d = std::abs(int(eight.row(y)[i]) - int(reduced->row(y)[i]));
            apart.worst = std::max(apart.worst, d);
            beyond += d > 1;
        }
    apart.beyondOne = total ? double(beyond) / double(total) : 0;
    return apart;
}

/// The calibration gate: an 8-bit document converted to 16 bits renders as its 8-bit render does, within a level.
/// Exactly so where the 8-bit engine rounds nothing but the blend itself (opaque pixels at full coverage). With
/// opacity, masks, soft edges and stacked layers the 8-bit engine rounds coverage to 1/256 and every layer's result
/// to a byte, which the 16-bit engine does not; there the two may differ by more than a level on a small share of
/// samples, most where a mode divides (dodge, burn, Vivid Light, Divide) or turns on hue (Hue, Saturation) and so
/// magnifies the 8-bit rounding.
TEST_CASE(sixteen_bit_renders_of_eight_bit_documents_are_within_a_level_in_every_mode) {
    int failures = 0;
    for (int m = 0; m < blendModeCount; m++) {
        const BlendMode mode = BlendMode(m);
        // An opaque layer in the mode over a busy opaque base, at 1:1.
        Document opaque(256, 192);
        opaque.layers.push_back(layerOf("base", noisyBase(256, 192), {0, 0}));
        Layer top = layerOf("top", noisyBase(200, 150, 9), {30, 20});
        top.blendMode = mode;
        opaque.layers.push_back(top);
        const Apart exact = levelsApart(opaque, RenderOptions());
        // Photoshop's 8-bit Vivid Light rounds its divisor to a byte; the 16-bit one does not, which is two levels at most.
        const int allowed = mode == BlendMode::VividLight ? 2 : 1;
        if (exact.worst > allowed) { std::fprintf(stderr, "  %s, opaque: %d levels apart\n", blendModeName(mode), exact.worst); failures++; }

        // A soft, masked, partly transparent layer at an opacity, and the blend scene (clipping, an isolated folder),
        // at 1:1 and reduced.
        Document single(256, 192);
        single.layers.push_back(layerOf("base", noisyBase(256, 192), {0, 0}));
        Layer soft = layerOf("top", paint(160, 128, 3), {20, 12});
        soft.blendMode = mode;
        soft.opacity = 0.35 + 0.65 * double((m * 7) % 13) / 12.0;
        soft.mask = maskOf(radialMask(160, 128));
        single.layers.push_back(soft);
        Document full = blendDocument(mode);
        for (auto [doc, options, what] : {std::tuple{&single, RenderOptions(), "soft layer"}, std::tuple{&single, reducedRegion(), "soft layer @0.5"},
                                          std::tuple{&full, RenderOptions(), "blend scene"}, std::tuple{&full, reducedRegion(), "blend scene @0.5"}}) {
            const Apart apart = levelsApart(*doc, options);
            if (std::getenv("COMPOSITOR_REPORT_U16_CALIBRATION"))
                std::fprintf(stderr, "  %-20s %-17s worst %3d, beyond a level %.4f%%\n", blendModeName(mode), what, apart.worst, apart.beyondOne * 100);
            // A single layer: at most 1% of samples beyond a level. The stacked scene compounds the 8-bit rounding
            // through clipping (8-bit unpremultiplied bases) and an isolated folder, so it is held to 10%: a bound
            // that catches a broken path, not a calibration.
            const double bound = doc == &single ? 0.01 : 0.10;
            if (apart.beyondOne > bound) { std::fprintf(stderr, "  %s, %s: %.2f%% of samples more than a level apart\n", blendModeName(mode), what, apart.beyondOne * 100); failures++; }
        }
    }
    CHECK_EQ(failures, 0);
}

TEST_MAIN()
