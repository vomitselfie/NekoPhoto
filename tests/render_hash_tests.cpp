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
// Not covered, on purpose:
// - Knockout: the engine has no knockout groups yet; an isolated (non pass-through) group stands in for it.
// - Color Lookup: needs a LUT file; its interpolation is exercised by its own tests.
#include "check.h"
#include "compositor/adjustments.h"
#include "compositor/blur.h"
#include "compositor/brush.h"
#include "compositor/depth.h"
#include "compositor/document.h"
#include "compositor/filters.h"
#include "compositor/mypaint.h"
#include "compositor/parallel.h"
#include "compositor/render.h"
#include "compositor/tipbrush.h"
#include "compositor/toning.h"

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
        for (Point p : wave(20, 240, 80, 45)) stroke.strokeTo({p, pressure ? 0.2 + 0.8 * std::fabs(std::sin(i++ * 0.13)) : 1.0});
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
        tip.angleJitter = 60;
        tip.sizeJitter = 0.4;
        tip.scatter = 0.5;
        tip.count = 2;
        tip.flowJitter = 0.3;
        tip.roundness = 0.6;
        tip.pressureSize = 1;
        tip.minimumSize = 0.2;
        tip.pressureFlow = 0.5;
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
                    MyPaintInput input;
                    input.document = p;
                    input.pressure = 0.3 + 0.6 * std::fabs(std::sin(i++ * 0.1));
                    input.seconds = 1.0 / 60;
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

} // namespace

TEST_CASE(render_hashes_match_the_baseline_on_the_pool_and_serially) {
    addGoldenScenes();
    addBlendScenes();
    addAdjustmentScenes();
    addFilterScenes();
    addBrushScenes();
    add16BitScenes();
    add16BitEditScenes();

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
    int changed = 0, added = 0, missing = 0;
    for (auto& [name, h] : actual) {
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
        std::fprintf(stderr, "  %d changed, %d new, %d missing; COMPOSITOR_UPDATE_RENDER_HASHES=1 rewrites %s\n", changed, added, missing, path.c_str());
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
