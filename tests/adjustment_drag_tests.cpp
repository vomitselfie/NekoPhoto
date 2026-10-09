// An adjustment layer's slider drag through a RenderCache (render_resume.h): each frame of the drag, drawn from the
// state the cache keeps below the dragged layer, must equal an uncached render of the same document byte for byte. The
// scenes cover the places an adjustment layer can stand (at the top level, in a fused run of table adjustments, in a
// Pass Through or isolated folder, nested folders, a styled folder, clipped to a plain or a styled base) with a mask,
// Blend If and blend modes on it and on the layers above; every depth (8, 16, 32 bits) and colour mode (RGB, CMYK,
// Lab); the whole canvas at 1:1, reduced views (drawn from the mip levels) and zoomed regions. Between ticks the
// document changes some other way (a layer below shown, hidden, faded) with the version moved on, the region and scale
// change, and a pixel layer's edit takes the cache for a while: each frame must still equal the uncached one.
#include "check.h"
#include "compositor/adjustments.h"
#include "compositor/blendif.h"
#include "compositor/colormgmt.h"
#include "compositor/document.h"
#include "compositor/layerstyle.h"
#include "compositor/render.h"
#include "compositor/workcounters.h"

#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

using namespace compositor;
using work::Counter;

namespace {

const int W = 96, H = 64;

std::shared_ptr<Image> photo(int w, int h) {
    auto img = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t* p = img->pixel(x, y);
            p[0] = uint8_t(x * 255 / std::max(1, w - 1));
            p[1] = uint8_t(y * 255 / std::max(1, h - 1));
            p[2] = uint8_t(((x / 7 + y / 5) % 2) ? 200 : 40);
            p[3] = 255;
        }
    return img;
}

std::shared_ptr<Image> noisy(int w, int h, uint32_t seed) {
    auto img = std::make_shared<Image>(w, h);
    std::mt19937 rng(seed);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t* p = img->pixel(x, y);
            const unsigned a = rng() % 256;
            for (int c = 0; c < 3; c++) p[c] = uint8_t((rng() % 256) * a / 255);
            p[3] = uint8_t(a);
        }
    return img;
}

std::shared_ptr<GrayImage> ramp(int w, int h) {
    auto m = std::make_shared<GrayImage>(w, h, 0);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) m->at(x, y) = uint8_t(255 * (x + y) / (w + h - 2));
    return m;
}

LayerMask maskOf(std::shared_ptr<GrayImage> mask) { LayerMask m; m.asset = MaskAsset::make(mask); return m; }

Layer pixels(const std::string& name, std::shared_ptr<Image> image, Point origin = {0, 0}) { return Layer(Asset::make(image, name), origin); }

Layer adjustmentLayer(AdjustmentKind kind) {
    Layer l(std::string("adjustment ") + adjustmentKindName(kind), Size(W, H));
    l.adjustment = AdjustmentSettings::defaults(kind).toLayerAdjustment();
    return l;
}

Layer folder(const std::string& name, bool passThrough) {
    Layer g(name, Size(W, H));
    g.isGroup = true;
    g.passThrough = passThrough;
    return g;
}

LayerStyle dropShadow() {
    LayerStyle s;
    DropShadow d;
    d.distance = 3;
    d.size = 2;
    s.dropShadows.push_back(d);
    return s;
}

/// The settings of tick `i` for the adjustment's kind: each tick a different table or shift.
AdjustmentSettings tick(AdjustmentKind kind, int i) {
    AdjustmentSettings s = AdjustmentSettings::defaults(kind);
    switch (kind) {
    case AdjustmentKind::Levels: s.levels.ranges[0].white = 255 - 23 * i; s.levels.ranges[0].gamma = 1 + 0.15 * i; break;
    case AdjustmentKind::Exposure: s.exposure.exposure = 0.3 * i - 0.5; break;
    case AdjustmentKind::HueSaturation: s.hsv.adjustments[0].hue = 17 * i; s.hsv.adjustments[0].saturation = 9 * i - 20; break;
    case AdjustmentKind::Curves: s.curves.channels[0] = {{0, 0}, {128, double(100 + 20 * i)}, {255, 255}}; break;
    default: break;
    }
    return s;
}

/// A document and the adjustment layer dragged in it; `finish` runs after the mode and depth conversion (Blend If
/// reads the document's colour mode).
struct Scene {
    std::string name;
    Document doc{W, H};
    Uuid dragged;
    AdjustmentKind kind = AdjustmentKind::Levels;
    Uuid below;   // a pixel layer under the drag, changed between ticks
    std::function<void(Document&)> finish;
};

/// The layers every scene starts from: an opaque photo and a translucent Multiply layer under everything else.
Scene start(const std::string& name) {
    Scene s;
    s.name = name;
    s.doc.layers.push_back(pixels("photo", photo(W, H)));
    Layer paint = pixels("paint below", noisy(W, H, 3));
    paint.blendMode = BlendMode::Multiply;
    paint.opacity = 0.8;
    s.below = paint.id;
    s.doc.layers.push_back(paint);
    return s;
}

/// What goes above the dragged layer in every scene: a Screen layer and a masked Normal one.
void above(Scene& s) {
    Layer screen = pixels("screen above", noisy(80, 50, 7), {8, 6});
    screen.blendMode = BlendMode::Screen;
    screen.opacity = 0.7;
    s.doc.layers.push_back(screen);
    Layer top = pixels("masked above", noisy(60, 40, 9), {30, 20});
    top.mask = maskOf(ramp(60, 40));
    s.doc.layers.push_back(top);
}

std::vector<Scene> scenes() {
    std::vector<Scene> all;
    {   // At the top level, a fused table adjustment.
        Scene s = start("top level Levels");
        Layer a = adjustmentLayer(AdjustmentKind::Levels);
        s.dragged = a.id;
        s.doc.layers.push_back(a);
        above(s);
        all.push_back(std::move(s));
    }
    {   // In a fused run: a Curves layer under it and an Exposure layer over it (one pass for the three at 8 bits).
        Scene s = start("fused run");
        Layer curves = adjustmentLayer(AdjustmentKind::Curves);
        curves.adjustment = tick(AdjustmentKind::Curves, 2).toLayerAdjustment();
        s.doc.layers.push_back(curves);
        Layer a = adjustmentLayer(AdjustmentKind::Levels);
        s.dragged = a.id;
        s.doc.layers.push_back(a);
        Layer exposure = adjustmentLayer(AdjustmentKind::Exposure);
        exposure.adjustment = tick(AdjustmentKind::Exposure, 3).toLayerAdjustment();
        s.doc.layers.push_back(exposure);
        above(s);
        all.push_back(std::move(s));
    }
    {   // Not fusible: a masked Hue/Saturation layer at 70% in Color mode.
        Scene s = start("masked Hue/Saturation in Color");
        Layer a = adjustmentLayer(AdjustmentKind::HueSaturation);
        a.mask = maskOf(ramp(W, H));
        a.opacity = 0.7;
        a.blendMode = BlendMode::Color;
        s.dragged = a.id;
        s.kind = AdjustmentKind::HueSaturation;
        s.doc.layers.push_back(a);
        above(s);
        all.push_back(std::move(s));
    }
    {   // Blend If on a masked Levels layer.
        Scene s = start("Blend If");
        Layer a = adjustmentLayer(AdjustmentKind::Levels);
        a.mask = maskOf(ramp(W, H));
        s.dragged = a.id;
        s.doc.layers.push_back(a);
        above(s);
        const Uuid id = a.id;
        s.finish = [id](Document& doc) {
            BlendIf b;
            b.channels[0].underlying = {0, 40, 160, 230};
            b.channels[0].thisLayer = {10, 30, 255, 255};
            setLayerBlendIf(*doc.find(id), b, doc.colorMode);
        };
        all.push_back(std::move(s));
    }
    {   // In a Pass Through folder at 60%, between two of its layers.
        Scene s = start("Pass Through folder");
        Layer g = folder("pass through", true);
        g.opacity = 0.6;
        Layer inner = pixels("inner", noisy(70, 50, 11), {10, 5});
        inner.parentId = g.id;
        Layer a = adjustmentLayer(AdjustmentKind::Levels);
        a.parentId = g.id;
        Layer over = pixels("inner over", noisy(50, 40, 13), {40, 20});
        over.parentId = g.id;
        over.blendMode = BlendMode::Overlay;
        s.dragged = a.id;
        s.doc.layers.push_back(g);
        s.doc.layers.push_back(inner);
        s.doc.layers.push_back(a);
        s.doc.layers.push_back(over);
        above(s);
        all.push_back(std::move(s));
    }
    {   // In an isolated, masked Multiply folder.
        Scene s = start("isolated folder");
        Layer g = folder("isolated", false);
        g.blendMode = BlendMode::Multiply;
        g.opacity = 0.9;
        g.mask = maskOf(ramp(W, H));
        Layer inner = pixels("inner", noisy(70, 50, 17), {10, 5});
        inner.parentId = g.id;
        Layer a = adjustmentLayer(AdjustmentKind::HueSaturation);
        a.parentId = g.id;
        Layer over = pixels("inner over", noisy(50, 40, 19), {40, 20});
        over.parentId = g.id;
        s.dragged = a.id;
        s.kind = AdjustmentKind::HueSaturation;
        s.doc.layers.push_back(g);
        s.doc.layers.push_back(inner);
        s.doc.layers.push_back(a);
        s.doc.layers.push_back(over);
        above(s);
        all.push_back(std::move(s));
    }
    {   // Nested: a fading Pass Through folder inside an isolated one.
        Scene s = start("nested folders");
        Layer outer = folder("outer", false);
        outer.opacity = 0.85;
        Layer first = pixels("outer first", noisy(70, 50, 23), {4, 4});
        first.parentId = outer.id;
        Layer inner = folder("inner", true);
        inner.parentId = outer.id;
        inner.opacity = 0.5;
        Layer innerPaint = pixels("inner paint", noisy(60, 40, 29), {20, 10});
        innerPaint.parentId = inner.id;
        Layer a = adjustmentLayer(AdjustmentKind::Exposure);
        a.parentId = inner.id;
        Layer innerOver = pixels("inner over", noisy(40, 30, 31), {30, 25});
        innerOver.parentId = inner.id;
        innerOver.blendMode = BlendMode::SoftLight;
        Layer last = pixels("outer last", noisy(30, 30, 37), {60, 30});
        last.parentId = outer.id;
        s.dragged = a.id;
        s.kind = AdjustmentKind::Exposure;
        for (Layer* l : {&outer, &first, &inner, &innerPaint, &a, &innerOver, &last}) s.doc.layers.push_back(*l);
        above(s);
        all.push_back(std::move(s));
    }
    {   // In a folder with a drop shadow: its effects are drawn from its contents as it opens.
        Scene s = start("styled folder");
        Layer g = folder("styled", true);
        setLayerStyle(g, dropShadow());
        Layer inner = pixels("inner", noisy(50, 30, 41), {20, 15});
        inner.parentId = g.id;
        Layer a = adjustmentLayer(AdjustmentKind::Levels);
        a.parentId = g.id;
        s.dragged = a.id;
        s.doc.layers.push_back(g);
        s.doc.layers.push_back(inner);
        s.doc.layers.push_back(a);
        above(s);
        all.push_back(std::move(s));
    }
    {   // Clipped to a plain base, between two clipped layers.
        Scene s = start("clipped");
        Layer base = pixels("base", noisy(70, 50, 43), {10, 8});
        Layer under = pixels("clipped under", noisy(W, H, 47));
        under.maskSourceId = base.id;
        Layer a = adjustmentLayer(AdjustmentKind::Levels);
        a.maskSourceId = base.id;
        Layer over = pixels("clipped over", noisy(W, H, 53));
        over.maskSourceId = base.id;
        over.blendMode = BlendMode::Screen;
        s.dragged = a.id;
        for (Layer* l : {&base, &under, &a, &over}) s.doc.layers.push_back(*l);
        above(s);
        all.push_back(std::move(s));
    }
    {   // Clipped to a styled base.
        Scene s = start("clipped to a styled base");
        Layer base = pixels("base", noisy(70, 50, 59), {10, 8});
        setLayerStyle(base, dropShadow());
        Layer a = adjustmentLayer(AdjustmentKind::HueSaturation);
        a.maskSourceId = base.id;
        s.dragged = a.id;
        s.kind = AdjustmentKind::HueSaturation;
        s.doc.layers.push_back(base);
        s.doc.layers.push_back(a);
        above(s);
        all.push_back(std::move(s));
    }
    return all;
}

/// The bytes of a rendered frame, for an exact comparison.
std::vector<uint8_t> bytesOf(const AnyImage& image) {
    if (auto p = image.u8()) return {p->data(), p->data() + p->byteCount()};
    if (auto p = image.u16()) { auto b = reinterpret_cast<const uint8_t*>(p->data()); return {b, b + p->byteCount()}; }
    if (auto p = image.f32()) { auto b = reinterpret_cast<const uint8_t*>(p->data()); return {b, b + p->byteCount()}; }
    if (auto p = image.c8()) { auto b = reinterpret_cast<const uint8_t*>(p->data()); return {b, b + p->byteCount()}; }
    return {};
}

/// The frame at the document's own depth and layout, as the exports and the 16- and 32-bit views draw it.
std::vector<uint8_t> renderAtDepth(const Document& doc, const RenderOptions& o, const Overrides* overrides, RenderCache* cache) {
    if (doc.colorMode != ColorMode::RGB) return bytesOf(renderNative(doc, o, overrides, cache));
    switch (doc.sampleType) {
    case SampleType::U16: { auto out = std::make_shared<Image16>(1, 1); render16(doc, o, *out, overrides, cache); return bytesOf(Image16Ptr(out)); }
    case SampleType::F32: { auto out = std::make_shared<ImageF>(1, 1); renderF(doc, o, *out, overrides, cache); return bytesOf(ImageFPtr(out)); }
    default: { auto out = std::make_shared<Image>(1, 1); render(doc, o, *out, overrides, cache); return bytesOf(ImagePtr(out)); }
    }
}

/// The canvas's frame: 8-bit RGB through the display path.
std::vector<uint8_t> renderForCanvas(const Document& doc, const RenderOptions& o, const Overrides* overrides, RenderCache* cache) {
    auto out = std::make_shared<Image>(1, 1);
    render(doc, o, *out, overrides, cache);
    return bytesOf(ImagePtr(out));
}

struct Variant { const char* name; ColorMode mode; SampleType depth; };
const Variant variants[] = {
    {"RGB 8", ColorMode::RGB, SampleType::U8},   {"RGB 16", ColorMode::RGB, SampleType::U16}, {"RGB 32", ColorMode::RGB, SampleType::F32},
    {"CMYK 8", ColorMode::CMYK, SampleType::U8}, {"CMYK 16", ColorMode::CMYK, SampleType::U16},
    {"Lab 8", ColorMode::Lab, SampleType::U8},   {"Lab 16", ColorMode::Lab, SampleType::U16},
};

struct View { const char* name; Rect region; double scale; };
const View views[] = {
    {"1:1", Rect(), 1},
    {"50%", Rect(), 0.5},
    {"25%", Rect(), 0.25},
    {"region at 300%", Rect(17, 9, 41, 30), 3},
    {"region at 37%", Rect(5, 3, 80, 57), 0.37},
};

using Renderer = std::vector<uint8_t> (*)(const Document&, const RenderOptions&, const Overrides*, RenderCache*);

int frames = 0, cachedFrames = 0;

/// One drag in `scene` through `draw`, every frame compared with the uncached one.
void drag(const Scene& source, const Variant& variant, Renderer draw, const char* path) {
    Scene s = source;
    std::string why;
    // A kind the mode does not offer would be dormant (hidden) there: Curves drags in its place.
    if (!adjustmentOfferedInMode(s.kind, variant.mode)) {
        s.kind = AdjustmentKind::Curves;
        s.doc.find(s.dragged)->adjustment = tick(s.kind, 0).toLayerAdjustment();
    }
    if (variant.mode != ColorMode::RGB && !convertDocumentMode(s.doc, variant.mode, ColorProfile(), ConvertOptions(), &why)) {
        check::fail(__FILE__, __LINE__, s.name + ", " + variant.name + ": " + why);
        return;
    }
    if (variant.depth != SampleType::U8 && !convertSampleType(s.doc, variant.depth, &why)) {
        check::fail(__FILE__, __LINE__, s.name + ", " + variant.name + ": " + why);
        return;
    }
    if (s.finish) s.finish(s.doc);
    Layer* dragged = s.doc.find(s.dragged);
    REQUIRE(dragged && dragged->adjustment && dragged->visible);
    Overrides overrides;
    overrides[s.dragged].adjusting = true;
    RenderCache cache;
    uint64_t version = 1;
    int step = 0;
    auto frame = [&](const View& view, const std::string& what) {
        RenderOptions o;
        o.region = view.region;
        o.scale = view.scale;
        o.version = version;
        const work::Snapshot before = work::snapshot();
        const std::vector<uint8_t> cached = draw(s.doc, o, &overrides, &cache);
        const uint64_t draws = (work::snapshot() - before)[Counter::LayerDraws];
        const work::Snapshot plainBefore = work::snapshot();
        const std::vector<uint8_t> plain = draw(s.doc, o, nullptr, nullptr);
        const uint64_t plainDraws = (work::snapshot() - plainBefore)[Counter::LayerDraws];
        frames++;
        if (draws < plainDraws) cachedFrames++;
        if (cached != plain) {
            size_t at = 0;
            while (at < cached.size() && at < plain.size() && cached[at] == plain[at]) at++;
            check::fail(__FILE__, __LINE__, s.name + ", " + variant.name + ", " + path + ", " + view.name + ", " + what + ": the cached frame differs from the uncached one at byte " +
                                                std::to_string(at) + " of " + std::to_string(plain.size()));
        }
        return std::pair{draws, plainDraws};
    };
    auto setTick = [&](int i) { s.doc.find(s.dragged)->adjustment = tick(s.kind, i).toLayerAdjustment(); };
    for (const View& view : views) {
        // The first frame keeps the state below; the next ones draw from it and must draw fewer layers than a whole frame.
        setTick(0);
        frame(view, "first tick");
        for (int i = 1; i <= 3; i++) {
            setTick(i);
            auto [draws, plainDraws] = frame(view, "tick " + std::to_string(i));
            if (draws >= plainDraws)
                check::fail(__FILE__, __LINE__, s.name + ", " + variant.name + ", " + path + ", " + view.name + ": a tick drew " + std::to_string(draws) + " layers, a whole frame " +
                                                    std::to_string(plainDraws));
        }
        // A layer below fades, hides and shows again (each a change the caller's version records).
        Layer* below = s.doc.find(s.below);
        below->opacity = 0.35 + 0.1 * step;
        version++;
        frame(view, "a layer below faded");
        below->visible = false;
        version++;
        frame(view, "a layer below hidden");
        setTick(4);
        frame(view, "a tick with it hidden");
        below->visible = true;
        version++;
        frame(view, "a layer below shown");
        // A pixel layer's edit (its own override) takes the cache, then the drag goes on.
        Overrides edit;
        edit[s.below].image = ImagePtr(noisy(W, H, 99));
        {
            RenderOptions o;
            o.region = view.region;
            o.scale = view.scale;
            o.version = version;
            draw(s.doc, o, &edit, &cache);
        }
        setTick(5);
        frame(view, "after another layer's edit");
        step++;
    }
}

} // namespace

TEST_CASE(adjustment_drag_frames_equal_uncached_frames) {
    const std::vector<Scene> all = scenes();
    for (const Variant& v : variants)
        for (const Scene& s : all) {
            drag(s, v, renderAtDepth, "at depth");
            if (v.depth != SampleType::U8 || v.mode != ColorMode::RGB) drag(s, v, renderForCanvas, "for the canvas");
        }
    std::fprintf(stderr, "  %d frames compared, %d of them drawn from a kept state\n", frames, cachedFrames);
    CHECK(cachedFrames > frames / 3);
}

TEST_MAIN()
