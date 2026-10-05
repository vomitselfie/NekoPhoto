// One pointer event of a brush stroke, as every brush engine takes it (docs/brush-engine.md). The raw fields are what
// the device reported, kept as they came: pressure is never folded into size here, and a mouse reports neutral
// values. The derived fields (speed, direction, tilt as magnitude and azimuth, distance and so on) are filled from
// the samples before, by a BrushSampleTrack, so a recorded stroke replays to the same values every time.
#pragma once
#include "geometry.h"
#include <optional>
#include <string>
#include <vector>

namespace compositor {

struct BrushSample {
    // ---- Raw input ----
    Point position;                 // document pixels
    double time = 0;                // seconds, from any origin; only differences matter
    double pressure = 0.5;          // 0..1; a mouse reports 0.5
    double tiltX = 0, tiltY = 0;    // degrees from upright, -90..90 each way (Qt reports about +-60)
    double twist = 0;               // barrel rotation in degrees, as reported (-180..180)
    double tangentialPressure = 0;  // the airbrush wheel, -1..1
    bool stylus = false;            // a pen: its pressure, tilt and twist are real; false for a mouse
    bool eraser = false;            // the pen's eraser end
    bool twistReported = false;     // the pen reports its barrel's twist (not every pen can): the Roll input reads it
    /// Screen points per document pixel where the stroke was drawn: the canvas's zoom (1 at 100%, 2 at 200%). The
    /// view's rotation or flip would not change it, since they keep lengths. 1 in a replay that does not say.
    double viewScale = 1;

    // ---- Derived by BrushSampleTrack ----
    double dt = 0;                  // seconds since the previous sample (1/120 when the times do not say)
    double speed = 0;               // document pixels per second, lightly smoothed
    double screenSpeed = 0;         // screen points per second (the document step times viewScale), smoothed the same
    double acceleration = 0;        // of that speed, per second
    double direction = 0;           // radians of travel, unwrapped: continuous across a turn
    double tiltMagnitude = 0;       // 0 upright .. 1 at 60 degrees or more
    double tiltAzimuth = 0;         // radians the pen leans towards, unwrapped
    double twistAngle = 0;          // the twist in radians, unwrapped
    double distance = 0;            // document pixels travelled since the stroke began
    double progress = -1;           // 0..1 along a stroke whose whole length is known (a replay); -1 while painting

    // ---- Set by the brush engine for its own dabs, not by the track ----
    double strokeRandom = 0;        // one draw per stroke, 0..1 (DynamicsInput::StrokeRandom)
    double initialDirection = 0;    // radians the stroke set off in (DynamicsInput::InitialDirection)
};

/// A mouse event: neutral pressure, no tilt or twist.
BrushSample mouseSample(Point position, double time);

/// `angle` moved by whole turns to lie within half a turn of `previous` (radians).
double unwrapAngle(double previous, double angle);

/// Fills the derived fields of each sample from the ones before it.
class BrushSampleTrack {
public:
    /// The sample with its derived fields filled; the first of a stroke has no speed and dt 1/120.
    BrushSample add(BrushSample sample);
    void reset() { last_.reset(); }
    const std::optional<BrushSample>& last() const { return last_; }

private:
    std::optional<BrushSample> last_;
};

/// Derives a whole stroke at once, progress included.
void deriveStroke(std::vector<BrushSample>& samples);

/// The sample a fraction `t` of the way from `a` to `b`: every continuous field in between, the angles along their
/// unwrapped values (so a twist from 170 to -170 degrees passes through 180, not 0); the flags from `b`.
BrushSample interpolate(const BrushSample& a, const BrushSample& b, double t);

/// A recorded stroke, as brush fixtures and replays store it: {"samples": [{t, x, y, pressure, tiltX, tiltY, twist,
/// tangentialPressure, viewScale}...], "stylus": bool} or a bare array of samples. Missing fields take their defaults; a stroke
/// that is not a stylus's reads with a mouse's neutral values whatever it holds.
struct RecordedStroke {
    std::string name;
    std::vector<BrushSample> samples;   // raw; deriveStroke fills the rest
    bool stylus = true;
};
std::optional<RecordedStroke> recordedStrokeFromJson(const std::string& text, std::string* error = nullptr);
std::string recordedStrokeToJson(const RecordedStroke& stroke);

} // namespace compositor
