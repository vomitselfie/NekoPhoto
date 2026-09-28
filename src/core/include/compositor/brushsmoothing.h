// Stabilisation: the signal-processing stage between the pen's raw samples and the stroke (docs/brush-engine.md,
// "Smoothing"). Raw samples go in; the samples the stroke should follow come out, zero or more per input, before
// BrushSampleTrack derives speed, direction and the rest from them.
//
//   raw sample -> input smoothing -> pressure smoothing -> stroke stabiliser -> BrushSampleTrack -> dynamics -> dabs
//
// Three controls, each its own filter with its own constants, since position, pressure and angles are different
// signals with different noise:
//   - input smoothing: a One Euro filter on the position (low latency: its cutoff rises with speed), and on the tilt
//     and the twist with slower constants of their own; the twist on its unwrapped angle, the tilt as a vector;
//   - pressure smoothing: a one-pole low pass on the pressure and the airbrush wheel, nothing else;
//   - the stroke stabiliser: Photoshop's Smoothing, 0..100, a brush that trails the pen, with Pulled String Mode,
//     Stroke Catch-Up, Catch-Up On Stroke End and Adjust For Zoom.
// With every amount at 0 nothing changes: the samples pass through as they came.
#pragma once
#include "brushsample.h"
#include <vector>

namespace compositor {

struct BrushSmoothing {
    double input = 0;             // input smoothing, 0..100
    double stabilizer = 0;        // the stroke stabiliser (Photoshop's Smoothing), 0..100
    double pressure = 0;          // pressure smoothing, 0..100
    bool pulledString = false;    // the brush moves only when the pen pulls the string taut
    bool strokeCatchUp = true;    // the brush keeps closing on a pen that has paused; off, it moves only as the pen does
    bool catchUpOnEnd = false;    // the release paints on from where the brush trails to where the pen lifted
    bool adjustForZoom = true;    // the stabiliser's reach is measured on the screen, so it is the same at any zoom

    bool active() const { return input > 0 || stabilizer > 0 || pressure > 0; }
    bool operator==(const BrushSmoothing&) const = default;
};

/// The stabiliser's reach in document pixels at `viewScale` (screen points per document pixel): the string's length
/// in Pulled String Mode, and the lag a steady pen settles at without Stroke Catch-Up.
double stabilizerReach(const BrushSmoothing& smoothing, double viewScale);

class BrushStabilizer {
public:
    explicit BrushStabilizer(BrushSmoothing smoothing = {}) : smoothing_(smoothing) {}
    const BrushSmoothing& settings() const { return smoothing_; }
    /// Starts a new stroke.
    void reset();
    /// The next raw sample: appends the samples the stroke should take now (raw; derive them afterwards). The first
    /// sample of a stroke comes out as it went in.
    void add(const BrushSample& raw, std::vector<BrushSample>& out);
    /// Time passes with the pen still (seconds, on the samples' clock): with Stroke Catch-Up the brush closes on it.
    void tick(double time, std::vector<BrushSample>& out);
    /// The pen lifts: with Catch-Up On Stroke End the brush paints on to where it lifted.
    void finish(std::vector<BrushSample>& out);
    /// The pen as the filters last had it (before the stabiliser), and where the brush is: the lag is their distance.
    const BrushSample& pen() const { return pen_; }
    Point brush() const { return brush_; }
    bool started() const { return started_; }

private:
    void stabilize(const BrushSample& from, const BrushSample& to, std::vector<BrushSample>& out);
    void give(const BrushSample& pen, std::vector<BrushSample>& out, bool force);

    BrushSmoothing smoothing_;
    bool started_ = false;
    BrushSample raw_;             // the last raw sample
    BrushSample pen_;             // the last filtered sample
    double speedX_ = 0, speedY_ = 0;   // the One Euro filter's smoothed velocity, document pixels per second
    double twist_ = 0;            // the filtered twist, unwrapped, degrees
    Point brush_;                 // where the stabilised brush is
    Point emitted_;               // the last position given to the stroke
    double emittedTime_ = 0;
    double brushSpeed_ = 0;       // how fast the brush has been moving, document pixels per second
    double clock_ = 0;            // the stabiliser's time
};

} // namespace compositor
