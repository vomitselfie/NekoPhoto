// Brush dynamics (docs/brush-engine.md): how a brush's parameters follow the pen. Each mapping reads one input of a
// BrushSample (pressure, speed, tilt, tilt direction, twist, a random draw, or how far along the stroke it is),
// shapes it through a curve over [0, 1], and scales it into the range [offset, offset + depth]. The mappings on
// one target combine by one rule:
//
//   scalar targets (size, flow, opacity, roundness, spacing, scatter, grain depth):
//       value = base x product of (offset + depth x curve(input)), in the order listed, then clamped to the target's range
//   circular targets (angle, grain rotation), in degrees:
//       value = base + sum of (offset + depth x curve(input))
//
// There is no additive term on scalar targets and no product on circular ones: an angle has no meaningful multiple,
// and a sum keeps it continuous across a turn. A random draw on a circular target is centred (-1..1, the curve
// shaping its size and keeping its sign), so jitter spreads both ways.
//
// Everything here is plain arithmetic on doubles: nothing assumes 8-bit pixels.
#pragma once
#include "brushsample.h"
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace compositor {

/// Roll is the barrel's twist when the pen reports one (BrushSample::twistReported) and the stroke's direction when it does
/// not, so a tip that turns with the barrel on a pen that has one follows the stroke on a pen that has not.
enum class DynamicsInput { Pressure, Speed, Tilt, TiltDirection, Twist, Random, StrokeProgress, Roll };
constexpr int dynamicsInputCount = 8;
enum class DynamicsTarget { Size, Flow, Opacity, Angle, Roundness, Spacing, Scatter, GrainDepth, GrainRotation };
constexpr int dynamicsTargetCount = 9;

/// A response curve over [0, 1] -> [0, 1] through points, piecewise linear or smooth. Smooth is a monotone cubic
/// (PCHIP, Fritsch-Carlson): it bends through the points without overshooting between them, so it never leaves the
/// range the points span. No points is the identity.
struct DynamicsCurve {
    enum class Kind { Linear, Smooth };
    Kind kind = Kind::Linear;
    std::vector<std::pair<double, double>> points;   // x ascending

    bool isIdentity() const { return points.size() < 2; }
    double evaluate(double x) const;
    /// Drops points that are not finite, clamps the rest to [0, 1] and sorts them by x (the last of equal x wins).
    void normalize();
};

struct DynamicsMapping {
    DynamicsInput input = DynamicsInput::Pressure;
    DynamicsTarget target = DynamicsTarget::Size;
    DynamicsCurve curve;
    double offset = 0;   // the output where the curve gives 0
    double depth = 1;    // added where the curve gives 1 (negative to fall)
    /// The input's full scale where it has one: Speed, document pixels per second (0: 2000); StrokeProgress, brush
    /// diameters of travel (0: the whole stroke when its length is known, else 25 diameters, Photoshop's Fade).
    double scale = 0;

    double minimum() const { return depth < 0 ? offset + depth : offset; }
    double maximum() const { return depth < 0 ? offset : offset + depth; }
};

using BrushDynamics = std::vector<DynamicsMapping>;

bool isCircular(DynamicsTarget target);
/// The target's legal range (for circular targets, a turn either way).
std::pair<double, double> targetRange(DynamicsTarget target);

/// The mapping's input from `sample` in [0, 1] (Random: `random`, 0..1, or -1..1 on a circular target).
/// `diameter` scales StrokeProgress.
double dynamicsInput(const DynamicsMapping& mapping, const BrushSample& sample, double diameter, double random);
/// One mapping's output for that input.
double dynamicsOutput(const DynamicsMapping& mapping, double input);
/// `base` with every mapping on `target` applied by the combination rule above. `random` is the draw for this target;
/// without `withRandom` the Random mappings are left out (the part of the size that sets the spacing).
double applyDynamics(const BrushDynamics& dynamics, DynamicsTarget target, double base, const BrushSample& sample,
                     double diameter, double random = 0, bool withRandom = true);
bool hasMapping(const BrushDynamics& dynamics, DynamicsTarget target, std::optional<DynamicsInput> input = std::nullopt);
/// A mapping with an identity curve.
DynamicsMapping dynamicsMapping(DynamicsInput input, DynamicsTarget target, double offset, double depth, double scale = 0);
/// The first mapping from `input` to `target`, if any.
const DynamicsMapping* findMapping(const BrushDynamics& dynamics, DynamicsInput input, DynamicsTarget target);

/// The settings tip brushes had before mappings, and presets saved then still hold: what they mean as mappings.
/// Pressure on size scales from `minimumSize` up (with `pressureSize` of 1), pressure on flow from 1 - `pressureFlow`,
/// and each jitter lowers its target by up to its amount (angle: that many degrees either way).
struct LegacyTipDynamics {
    double sizeJitter = 0, flowJitter = 0, angleJitter = 0;
    double pressureSize = 0, minimumSize = 0, pressureFlow = 0;
};
BrushDynamics legacyDynamics(const LegacyTipDynamics& legacy);

/// A tilted pencil's tip, for any tip brush: it flattens as the pen leans (Tilt on roundness, down to `flattest` at
/// full tilt) and turns so its long side points the way the pen leans (TiltDirection on the angle, a depth of -360).
BrushDynamics tiltShapesTip(double flattest);
/// Whether `dynamics` has tiltShapesTip's mappings, and if so its `flattest`.
std::optional<double> tiltShapeOf(const BrushDynamics& dynamics);
/// `dynamics` without them.
void removeTiltShape(BrushDynamics& dynamics);

/// Names as brush.json stores them: "pressure", "tiltDirection", "size", "grainRotation"...
const char* dynamicsInputName(DynamicsInput input);
const char* dynamicsTargetName(DynamicsTarget target);
std::optional<DynamicsInput> dynamicsInputFromName(const std::string& name);
std::optional<DynamicsTarget> dynamicsTargetFromName(const std::string& name);

} // namespace compositor
