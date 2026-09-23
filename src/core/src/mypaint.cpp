#include "compositor/mypaint.h"
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
std::string knownOnly(const std::string& brushJson) {
    nlohmann::json j = nlohmann::json::parse(brushJson, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("settings") || !j["settings"].is_object()) return brushJson;
    nlohmann::json& settings = j["settings"];
    for (auto it = settings.begin(); it != settings.end();) {
        if (int(mypaint_brush_setting_from_cname(it.key().c_str())) < 0) { it = settings.erase(it); continue; }
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
    const Image* base = grid_.gridBase();
    if (!base) { error_ = "MyPaint brushes paint layer pixels only."; return; }
    engine_ = std::make_unique<Engine>();
    Tiles& t = engine_->tiles;
    mypaint_tiled_surface2_init(&t.parent, requestStart, requestEnd);
    t.parent.threadsafe_tile_requests = TRUE;   // Tiles::tile locks, so libmypaint may render tiles in parallel
    t.base = base;
    t.width = base->width();
    t.height = base->height();
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

void MyPaintStroke::strokeTo(const MyPaintInput& input) {
    if (!isValid() || finished_ || !input.document.isFinite()) return;
    Point p = grid_.documentToGrid().apply(input.document);
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
    mypaint_brush_stroke_to(engine_->brush, surface, float(p.x), float(p.y), float(std::clamp(input.pressure, 0.0, 1.0)),
                            float(std::clamp(input.xtilt, -1.0, 1.0)), float(std::clamp(input.ytilt, -1.0, 1.0)),
                            std::clamp(input.seconds, 0.001, 5.0));
    MyPaintRectangle rects[8];
    MyPaintRectangles changed{8, rects};
    mypaint_tiled_surface2_end_atomic(&t.parent, &changed);
    started_ = true;
    last_ = input.document;

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

void MyPaintStroke::finish() {
    if (!isValid() || finished_ || !started_) return;
    MyPaintInput lift;
    lift.document = last_;
    lift.pressure = 0;
    lift.seconds = 0.01;
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
void MyPaintStroke::strokeTo(const MyPaintInput&) {}
void MyPaintStroke::finish() {}

#endif

} // namespace compositor
