// MyPaint brushes: the presets of the mypaint-brushes collection (.myb, JSON), painted by libmypaint's
// engine onto a layer's pixels. libmypaint draws into 64-pixel tiles of 15-bit premultiplied RGBA that are
// filled from the layer on first touch, so smudging and blending read the real paint; after every event the
// changed area is written back to a BrushStroke's working image through the selection. The grid, the preview
// and the commit stay the BrushStroke's, so a MyPaint stroke is undone, previewed and placed like any other.
// Optional at build time (COMPOSITOR_HAVE_MYPAINT).
#pragma once
#include "brush.h"
#include <memory>
#include <string>

namespace compositor {

/// Whether this build can paint MyPaint brushes.
bool myPaintSupported();

/// What a preset asks for by default: its radius (in pixels) and whether it is an eraser, read from the .myb.
struct MyPaintPresetInfo { double radius = 2; bool eraser = false; bool valid = false; };
MyPaintPresetInfo myPaintPresetInfo(const std::string& brushJson);

/// One pointer event: a document point with the pen's pressure (0..1; 0.5 for a mouse), tilt (-1..1) and the
/// seconds since the previous event.
struct MyPaintInput {
    Point document;
    double pressure = 0.5, xtilt = 0, ytilt = 0;
    double seconds = 0;
};

class MyPaintStroke {
public:
    /// Paints onto `grid`, a stroke begun on a layer's pixels (not a mask), with the preset `brushJson`. The
    /// colour, the size (`settings.diameter` in document pixels), the opacity (multiplying the preset's) and
    /// erasing come from `settings`; the rest of the preset's behaviour is its own. `grid` keeps the preview,
    /// the selection and the commit, and must outlive this object.
    MyPaintStroke(BrushStroke& grid, const std::string& brushJson, const BrushSettings& settings);
    ~MyPaintStroke();
    MyPaintStroke(const MyPaintStroke&) = delete;
    MyPaintStroke& operator=(const MyPaintStroke&) = delete;

    bool isValid() const;
    const std::string& error() const { return error_; }
    void strokeTo(const MyPaintInput& input);
    /// Lifts the pen: a last event at zero pressure, which some presets use to taper. Call before the grid's
    /// commit.
    void finish();

private:
    struct Engine;
    BrushStroke& grid_;
    std::unique_ptr<Engine> engine_;
    std::string error_;
    Point last_;
    bool started_ = false, finished_ = false;
};

} // namespace compositor
