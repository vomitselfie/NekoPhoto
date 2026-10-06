#include "compositor/mypaint.h"
#include "compositor/colormgmt.h"
#include "compositor/parallel.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>

#ifdef COMPOSITOR_HAVE_MYPAINT
extern "C" {
#include <mypaint-brush.h>
#include <mypaint-brush-settings.h>
#include <mypaint-tiled-surface.h>
}
#include <cstring>
#include <mutex>
#include <vector>
#endif

namespace compositor {

MyPaintPresetInfo myPaintPresetInfo(const std::string& brushJson) {
    MyPaintPresetInfo info;
    nlohmann::json j = nlohmann::json::parse(brushJson, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("settings") || !j["settings"].is_object()) return info;
    const nlohmann::json& settings = j["settings"];
    auto base = [&](const char* name, double fallback) {
        auto it = settings.find(name);
        if (it == settings.end() || !it->is_object()) return fallback;
        auto value = it->find("base_value");
        return value != it->end() && value->is_number() ? value->get<double>() : fallback;
    };
    info.radius = std::exp(base("radius_logarithmic", std::log(2.0)));
    info.eraser = base("eraser", 0) > 0.5;
    info.valid = true;
    return info;
}

#ifdef COMPOSITOR_HAVE_MYPAINT

namespace {

constexpr int tileSize = MYPAINT_TILE_SIZE;
constexpr uint32_t one = 1u << 15;   // libmypaint's fixed-point 1.0

/// The tile store libmypaint paints into. It must start with the MyPaintTiledSurface2 it extends: libmypaint
/// hands the callbacks that pointer.
struct Tiles {
    MyPaintTiledSurface2 parent;
    const Image* base = nullptr;
    // A 32-bit layer: its linear floats encoded through the document's curve into libmypaint's 15 bits as a tile is
    // first touched (the documented round trip, docs/bit-depth.md), and decoded back only where a dab changed a sample.
    const ImageF* baseF = nullptr;
    const TransferCurve* curve = nullptr;
    const Image16* base16 = nullptr;   // a 16-bit layer: its 0..32768 samples are libmypaint's own fixed point
    int width = 0, height = 0, columns = 0, rows = 0;
    std::vector<std::unique_ptr<uint16_t[]>> tiles;
    std::mutex lock;

    uint16_t* tile(int tx, int ty) {
        if (tx < 0 || ty < 0 || tx >= columns || ty >= rows) return nullptr;
        std::lock_guard<std::mutex> guard(lock);
        std::unique_ptr<uint16_t[]>& slot = tiles[size_t(ty) * size_t(columns) + size_t(tx)];
        if (!slot) {
            // First touch: the layer's own pixels, so smudging and blending read what is really there.
            slot.reset(new uint16_t[size_t(tileSize) * tileSize * 4]());
            if (baseF) {
                for (int y = 0; y < tileSize; y++) {
                    const int py = ty * tileSize + y;
                    if (py >= height) break;
                    uint16_t* dst = slot.get() + size_t(y) * tileSize * 4;
                    for (int x = 0; x < tileSize; x++) {
                        const int px = tx * tileSize + x;
                        if (px >= width) break;
                        encodeF(baseF->pixel(px, py), dst + x * 4);
                    }
                }
                return slot.get();
            }
            if (base16) {
                // The same range as libmypaint's: copied as they are.
                for (int y = 0; y < tileSize; y++) {
                    const int py = ty * tileSize + y;
                    if (py >= height) break;
                    const int px = tx * tileSize, n = std::min(tileSize, width - px);
                    if (n > 0) std::memcpy(slot.get() + size_t(y) * tileSize * 4, base16->pixel(px, py), size_t(n) * 4 * sizeof(uint16_t));
                }
                return slot.get();
            }
            for (int y = 0; y < tileSize; y++) {
                const int py = ty * tileSize + y;
                if (py >= height) break;
                const uint8_t* src = base->row(py);
                uint16_t* dst = slot.get() + size_t(y) * tileSize * 4;
                for (int x = 0; x < tileSize; x++) {
                    const int px = tx * tileSize + x;
                    if (px >= width) break;
                    for (int c = 0; c < 4; c++) dst[x * 4 + c] = uint16_t((src[px * 4 + c] * one + 127) / 255);
                }
            }
        }
        return slot.get();
    }

    /// A premultiplied linear float pixel as libmypaint's premultiplied 15-bit encoded colour.
    void encodeF(const float* p, uint16_t* out) const {
        const float a = std::clamp(p[3], 0.0f, 1.0f);
        if (!(a > 0)) { out[0] = out[1] = out[2] = out[3] = 0; return; }
        for (int c = 0; c < 3; c++) {
            const float straight = std::clamp(p[c] / a, 0.0f, 1.0f);
            out[c] = uint16_t(std::lround(curve->fromLinearExact(straight) * a * float(one)));
        }
        out[3] = uint16_t(std::lround(a * float(one)));
    }
    /// And back: premultiplied 15-bit encoded to premultiplied linear float.
    void decodeF(const uint16_t* in, float* out) const {
        const float a = std::min<uint32_t>(in[3], one) / float(one);
        if (!(a > 0)) { out[0] = out[1] = out[2] = out[3] = 0; return; }
        for (int c = 0; c < 3; c++) {
            const float straight = std::clamp(std::min<uint32_t>(in[c], one) / float(one) / a, 0.0f, 1.0f);
            out[c] = curve->toLinear(straight) * a;
        }
        out[3] = a;
    }

    /// A tile already painted, or null; for reading back once libmypaint is done (no requests in flight).
    const uint16_t* painted(int tx, int ty) const { return tiles[size_t(ty) * size_t(columns) + size_t(tx)].get(); }
};

void requestStart(MyPaintTiledSurface2* surface, MyPaintTileRequest* request) {
    auto* self = reinterpret_cast<Tiles*>(surface);
    uint16_t* buffer = self->tile(request->tx, request->ty);
    // Off the grid: a scratch tile that is thrown away, since nothing there can be kept.
    request->context = buffer ? nullptr : new uint16_t[size_t(tileSize) * tileSize * 4]();
    request->buffer = buffer ? buffer : static_cast<uint16_t*>(request->context);
}

void requestEnd(MyPaintTiledSurface2*, MyPaintTileRequest* request) {
    delete[] static_cast<uint16_t*>(request->context);
    request->context = nullptr;
}

/// The preset without the settings and inputs this libmypaint does not know (MyPaint 2 presets name a few that
/// 1.6 lacks, such as the surface-map inputs); libmypaint would print a warning for each and ignore them.
///
/// Legal boundaries (docs/legal-boundaries.md, "Brushes") are applied here too, so every preset paints inside them:
/// - opacity never follows the pen's speed (no velocity-dependent deposition): the speed inputs, and the custom input
///   that can carry them, are dropped from the opacity settings;
/// - smudging keeps ONE carried colour (libmypaint's running average of canvas samples, which the brush colour never
///   enters): the smudge-bucket setting, which selects among several stores, is dropped;
/// - colours mix linearly: the spectral paint mode is dropped.
std::string knownOnly(const std::string& brushJson) {
    nlohmann::json j = nlohmann::json::parse(brushJson, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("settings") || !j["settings"].is_object()) return brushJson;
    nlohmann::json& settings = j["settings"];
    static const char* const dropped[] = {"smudge_bucket", "paint_mode"};
    static const char* const deposition[] = {"opaque", "opaque_multiply", "opaque_linearize"};
    for (auto it = settings.begin(); it != settings.end();) {
        if (int(mypaint_brush_setting_from_cname(it.key().c_str())) < 0) { it = settings.erase(it); continue; }
        if (std::find_if(std::begin(dropped), std::end(dropped), [&](const char* d) { return it.key() == d; }) != std::end(dropped)) { it = settings.erase(it); continue; }
        if (std::find_if(std::begin(deposition), std::end(deposition), [&](const char* d) { return it.key() == d; }) != std::end(deposition)
            && it->is_object() && it->contains("inputs") && (*it)["inputs"].is_object())
            for (const char* input : {"speed1", "speed2", "custom"}) (*it)["inputs"].erase(input);
        if (it->is_object() && it->contains("inputs") && (*it)["inputs"].is_object()) {
            nlohmann::json& inputs = (*it)["inputs"];
            for (auto in = inputs.begin(); in != inputs.end();)
                in = int(mypaint_brush_input_from_cname(in.key().c_str())) < 0 ? inputs.erase(in) : std::next(in);
        }
        ++it;
    }
    return j.dump();
}

void rgbToHsv(double r, double g, double b, double& h, double& s, double& v) {
    const double mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
    v = mx;
    s = mx > 0 ? d / mx : 0;
    if (d <= 0) { h = 0; return; }
    if (mx == r) h = std::fmod((g - b) / d, 6.0);
    else if (mx == g) h = (b - r) / d + 2;
    else h = (r - g) / d + 4;
    h /= 6;
    if (h < 0) h += 1;
}

} // namespace

struct MyPaintStroke::Engine {
    Tiles tiles;
    MyPaintBrush* brush = nullptr;
    ~Engine() {
        if (brush) mypaint_brush_unref(brush);
        mypaint_tiled_surface2_destroy(&tiles.parent);
    }
};

bool myPaintSupported() { return true; }

MyPaintStroke::MyPaintStroke(BrushStroke& grid, const std::string& brushJson, const BrushSettings& settings) : grid_(grid) {
    if (!grid_.isValid()) { error_ = grid_.error(); return; }
    if (grid_.colorMode() != ColorMode::RGB) { error_ = "MyPaint brushes paint RGB documents only."; return; }
    const Image* base = grid_.gridBase();
    const Image16* base16 = grid_.gridBase16();
    const ImageF* baseF = grid_.gridBaseF();
    if (!base && !base16 && !baseF) { error_ = "MyPaint brushes paint layer pixels only."; return; }
    if (baseF && !grid_.transferCurve()) { error_ = "MyPaint brushes paint layer pixels only."; return; }
    engine_ = std::make_unique<Engine>();
    Tiles& t = engine_->tiles;
    mypaint_tiled_surface2_init(&t.parent, requestStart, requestEnd);
    t.parent.threadsafe_tile_requests = TRUE;   // Tiles::tile locks, so libmypaint may render tiles in parallel
    t.base = base;
    t.base16 = base16;
    t.baseF = baseF;
    t.curve = grid_.transferCurve();
    t.width = base ? base->width() : base16 ? base16->width() : baseF->width();
    t.height = base ? base->height() : base16 ? base16->height() : baseF->height();
    t.columns = (t.width + tileSize - 1) / tileSize;
    t.rows = (t.height + tileSize - 1) / tileSize;
    t.tiles.resize(size_t(t.columns) * size_t(t.rows));

    engine_->brush = mypaint_brush_new();
    if (!mypaint_brush_from_string(engine_->brush, knownOnly(brushJson).c_str())) { error_ = "The brush preset could not be read."; engine_.reset(); return; }
    // Grid pixels per document pixel: the preset works in the pixels it paints.
    Point unit = grid_.documentToGrid().applyVector({1, 0});
    double gridScale = std::hypot(unit.x, unit.y);
    if (!(gridScale > 0) || !std::isfinite(gridScale)) gridScale = 1;
    MyPaintBrush* brush = engine_->brush;
    mypaint_brush_set_base_value(brush, MYPAINT_BRUSH_SETTING_RADIUS_LOGARITHMIC, float(std::log(std::max(0.2, settings.diameter / 2 * gridScale))));
    mypaint_brush_set_base_value(brush, MYPAINT_BRUSH_SETTING_OPAQUE, float(mypaint_brush_get_base_value(brush, MYPAINT_BRUSH_SETTING_OPAQUE) * settings.opacity));
    double h, s, v;
    rgbToHsv(settings.red, settings.green, settings.blue, h, s, v);
    mypaint_brush_set_base_value(brush, MYPAINT_BRUSH_SETTING_COLOR_H, float(h));
    mypaint_brush_set_base_value(brush, MYPAINT_BRUSH_SETTING_COLOR_S, float(s));
    mypaint_brush_set_base_value(brush, MYPAINT_BRUSH_SETTING_COLOR_V, float(v));
    if (settings.erasing) mypaint_brush_set_base_value(brush, MYPAINT_BRUSH_SETTING_ERASER, 1.0f);
    mypaint_brush_reset(brush);
    mypaint_brush_new_stroke(brush);
}

MyPaintStroke::~MyPaintStroke() = default;

bool MyPaintStroke::isValid() const { return engine_ != nullptr && error_.empty(); }

void MyPaintStroke::strokeTo(const BrushSample& input) {
    if (!isValid() || finished_ || !input.position.isFinite()) return;
    Point p = grid_.documentToGrid().apply(input.position);
    if (std::fabs(p.x) > 1e7 || std::fabs(p.y) > 1e7) return;
    Tiles& t = engine_->tiles;
    // libmypaint's first-generation entry point, as GIMP uses it. The newer mypaint_brush_stroke_to_2 (view
    // zoom and the spectral "paint mode" some MyPaint 2 presets set) reads uninitialised memory in 1.6: the
    // same stroke comes out differently depending on what the heap held (MALLOC_PERTURB_ shows it), which this
    // one does not. Those presets paint here with ordinary RGB mixing.
    MyPaintSurface* surface = mypaint_surface2_to_surface(&t.parent.parent);
    mypaint_tiled_surface2_begin_atomic(&t.parent);
    if (!started_) {
        // Put the pen down where the stroke starts: the brush's position state there, then an event without
        // pressure a second after the last (as GIMP does), so no line is drawn in from the origin and the
        // slow-tracking settings start settled.
        for (MyPaintBrushState state : {MYPAINT_BRUSH_STATE_X, MYPAINT_BRUSH_STATE_ACTUAL_X}) mypaint_brush_set_state(engine_->brush, state, float(p.x));
        for (MyPaintBrushState state : {MYPAINT_BRUSH_STATE_Y, MYPAINT_BRUSH_STATE_ACTUAL_Y}) mypaint_brush_set_state(engine_->brush, state, float(p.y));
        mypaint_brush_stroke_to(engine_->brush, surface, float(p.x), float(p.y), 0.0f, 0.0f, 0.0f, 1.0);
    }
    const double seconds = std::clamp(input.dt > 0 ? input.dt : 1.0 / 120, 0.001, 5.0);
    // A click leaves a mark, as in Photoshop and Krita: a preset that places dabs only by distance travelled
    // would paint nothing until the pen moved, so the press gets exactly one dab from a dab rate lent for it.
    const float rate = mypaint_brush_get_base_value(engine_->brush, MYPAINT_BRUSH_SETTING_DABS_PER_SECOND);
    const bool lend = !started_ && rate <= 0;
    // One and a half dabs' worth: one is placed, and a rate of exactly one per event rounds libmypaint's time
    // bookkeeping just below zero ("Time is running backwards").
    if (lend) mypaint_brush_set_base_value(engine_->brush, MYPAINT_BRUSH_SETTING_DABS_PER_SECOND, float(1.5 / seconds));
    mypaint_brush_stroke_to(engine_->brush, surface, float(p.x), float(p.y), float(std::clamp(input.pressure, 0.0, 1.0)),
                            float(std::clamp(input.tiltX / 60, -1.0, 1.0)), float(std::clamp(input.tiltY / 60, -1.0, 1.0)), seconds);
    if (lend) mypaint_brush_set_base_value(engine_->brush, MYPAINT_BRUSH_SETTING_DABS_PER_SECOND, rate);
    MyPaintRectangle rects[8];
    MyPaintRectangles changed{8, rects};
    mypaint_tiled_surface2_end_atomic(&t.parent, &changed);
    started_ = true;
    last_ = input;

    if (t.baseF) {
        // A 32-bit layer: a pixel whose four samples a dab left as they were encoded keeps its exact floats; one it
        // changed is decoded, then taken through the selection in float.
        ImageF* working = grid_.gridWorkingF();
        const ImageF* baseF = t.baseF;
        const GrayF* selection = grid_.gridSelectionF();
        for (int i = 0; i < changed.num_rectangles; i++) {
            const MyPaintRectangle& r = rects[i];
            const int x0 = std::max(0, r.x), y0 = std::max(0, r.y), x1 = std::min(t.width, r.x + r.width), y1 = std::min(t.height, r.y + r.height);
            if (x0 >= x1 || y0 >= y1) continue;
            parallelRows(y0, y1, [&](int ya, int yb) {
                for (int y = ya; y < yb; y++) {
                    const float* b = baseF->row(y);
                    float* out = working->row(y);
                    const float* sel = selection ? selection->row(y) : nullptr;
                    for (int x = x0; x < x1; x++) {
                        const uint16_t* tile = t.painted(x / tileSize, y / tileSize);
                        if (!tile) continue;
                        const uint16_t* px = tile + (size_t(y % tileSize) * tileSize + size_t(x % tileSize)) * 4;
                        uint16_t before[4];
                        t.encodeF(b + x * 4, before);
                        if (std::equal(before, before + 4, px)) { std::memcpy(out + x * 4, b + x * 4, 4 * sizeof(float)); continue; }
                        float painted[4];
                        t.decodeF(px, painted);
                        const float k = sel ? std::clamp(sel[x], 0.0f, 1.0f) : 1.0f;
                        for (int c = 0; c < 4; c++) out[x * 4 + c] = b[x * 4 + c] + (painted[c] - b[x * 4 + c]) * k;
                    }
                }
            }, 16);
            grid_.markPainted(Rect(x0, y0, x1 - x0, y1 - y0));
        }
        return;
    }
    if (t.base16) {
        // A 16-bit layer takes the tiles' samples as they are, through the selection.
        Image16* working = grid_.gridWorking16();
        const Image16* base16 = t.base16;
        const Gray16* selection = grid_.gridSelection16();
        for (int i = 0; i < changed.num_rectangles; i++) {
            const MyPaintRectangle& r = rects[i];
            const int x0 = std::max(0, r.x), y0 = std::max(0, r.y), x1 = std::min(t.width, r.x + r.width), y1 = std::min(t.height, r.y + r.height);
            if (x0 >= x1 || y0 >= y1) continue;
            parallelRows(y0, y1, [&](int ya, int yb) {
                for (int y = ya; y < yb; y++) {
                    const uint16_t* b = base16->row(y);
                    uint16_t* out = working->row(y);
                    const uint16_t* sel = selection ? selection->row(y) : nullptr;
                    for (int x = x0; x < x1; x++) {
                        const uint16_t* tile = t.painted(x / tileSize, y / tileSize);
                        if (!tile) continue;
                        const uint16_t* px = tile + (size_t(y % tileSize) * tileSize + size_t(x % tileSize)) * 4;
                        for (int c = 0; c < 4; c++) {
                            const int painted = int(std::min<uint32_t>(px[c], one)), under = b[x * 4 + c];
                            if (!sel) { out[x * 4 + c] = uint16_t(painted); continue; }
                            const int delta = painted - under, k = int(std::min<uint32_t>(sel[x], one));
                            out[x * 4 + c] = uint16_t(under + (delta * k + (delta >= 0 ? int(one / 2) : -int(one / 2))) / int(one));
                        }
                    }
                }
            }, 16);
            grid_.markPainted(Rect(x0, y0, x1 - x0, y1 - y0));
        }
        return;
    }
    // The changed area back to 8 bits, through the selection: out = base + (paint - base) * selected.
    Image* working = grid_.gridWorking();
    const Image* base = grid_.gridBase();
    const GrayImage* selection = grid_.gridSelection();
    for (int i = 0; i < changed.num_rectangles; i++) {
        const MyPaintRectangle& r = rects[i];
        const int x0 = std::max(0, r.x), y0 = std::max(0, r.y), x1 = std::min(t.width, r.x + r.width), y1 = std::min(t.height, r.y + r.height);
        if (x0 >= x1 || y0 >= y1) continue;
        parallelRows(y0, y1, [&](int ya, int yb) {
            for (int y = ya; y < yb; y++) {
                const uint8_t* b = base->row(y);
                uint8_t* out = working->row(y);
                const uint8_t* sel = selection ? selection->row(y) : nullptr;
                for (int x = x0; x < x1; x++) {
                    const uint16_t* tile = t.painted(x / tileSize, y / tileSize);
                    if (!tile) continue;   // inside the reported box, but never touched
                    const uint16_t* px = tile + (size_t(y % tileSize) * tileSize + size_t(x % tileSize)) * 4;
                    for (int c = 0; c < 4; c++) {
                        const int painted = int((uint32_t(std::min<uint32_t>(px[c], one)) * 255 + one / 2) >> 15);
                        out[x * 4 + c] = sel ? uint8_t(b[x * 4 + c] + ((painted - b[x * 4 + c]) * sel[x] + (painted >= b[x * 4 + c] ? 127 : -127)) / 255) : uint8_t(painted);
                    }
                }
            }
        }, 16);
        grid_.markPainted(Rect(x0, y0, x1 - x0, y1 - y0));
    }
}

bool MyPaintStroke::settled() const {
    if (!isValid() || !started_ || finished_) return true;
    const Point p = grid_.documentToGrid().apply(last_.position);
    const float x = mypaint_brush_get_state(engine_->brush, MYPAINT_BRUSH_STATE_X);
    const float y = mypaint_brush_get_state(engine_->brush, MYPAINT_BRUSH_STATE_Y);
    return std::hypot(double(x) - p.x, double(y) - p.y) < 0.5;
}

void warmMyPaint(const std::string& brushJson) {
    auto image = std::make_shared<Image>(8, 8);
    Layer layer(Asset::make(image, "warm"), Point(0, 0));
    BrushSettings settings;
    settings.diameter = 4;
    BrushStroke grid(layer, false, settings, Size(8, 8));
    MyPaintStroke stroke(grid, brushJson, settings);
    BrushSample input = mouseSample({4, 4}, 0);
    input.dt = 1.0 / 120;
    stroke.strokeTo(input);
    stroke.finish();
}

void MyPaintStroke::finish() {
    if (!isValid() || finished_ || !started_) return;
    BrushSample lift = mouseSample(last_.position, last_.time + 0.01);
    lift.pressure = 0;
    lift.dt = 0.01;
    strokeTo(lift);
    finished_ = true;
}


#else

struct MyPaintStroke::Engine {};

bool myPaintSupported() { return false; }

MyPaintStroke::MyPaintStroke(BrushStroke& grid, const std::string&, const BrushSettings&) : grid_(grid) {
    error_ = "This build has no MyPaint brush engine (libmypaint).";
}
MyPaintStroke::~MyPaintStroke() = default;
bool MyPaintStroke::isValid() const { return false; }
void MyPaintStroke::strokeTo(const BrushSample&) {}
bool MyPaintStroke::settled() const { return true; }
void warmMyPaint(const std::string&) {}
void MyPaintStroke::finish() {}

#endif

} // namespace compositor
