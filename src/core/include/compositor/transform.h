// LayerTransform: where a layer's pixels sit on the document. A port of
// Document/LayerTransform.swift: unrotated bounds in document pixels, rotation
// in degrees clockwise about their center, flips, and the resampling choice.
#pragma once
#include "geometry.h"
#include <array>
#include <string>
#include <vector>

namespace compositor {

enum class Sampling { Nearest, Smooth, High };
const char* samplingName(Sampling s);           // "Nearest", "Smooth", "High quality"
bool parseSampling(const std::string& name, Sampling& out);

struct LayerTransform {
    Point origin;
    Size size;
    double rotation = 0;
    bool flipX = false;
    bool flipY = false;
    Sampling sampling = Sampling::High;

    LayerTransform() = default;
    LayerTransform(Point origin_, Size size_) : origin(origin_), size(size_) {}
    bool operator==(const LayerTransform&) const = default;

    Point center() const { return {origin.x + size.width / 2, origin.y + size.height / 2}; }
    double radians() const { return std::fmod(rotation, 360.0) * M_PI / 180.0; }
    bool isValid() const;
    /// The document point at unit coordinates (0..1 across the unrotated bounds).
    Point point(Point unit) const;
    bool contains(Point p) const;
    double scalePercent(Size pixelSize) const { return size.width / std::max(1.0, pixelSize.width) * 100; }
    LayerTransform scaledToPercent(double percent, Size pixelSize) const;
    /// Whole pixels and whole degrees.
    LayerTransform rounded() const;
    /// Maps a `width` x `height` pixel grid (y down) onto the document (BrushRaster.pixelToDocument).
    Affine pixelToDocument(int width, int height) const;
    Affine unitToDocument() const { return pixelToDocument(1, 1); }
    /// A transform placing the unit square as `map` does (shear dropped), keeping this sampling.
    LayerTransform placing(const Affine& map) const;
    /// This placement carried along as a layer moves from `from` to `to`.
    LayerTransform following(const LayerTransform& from, const LayerTransform& to) const;
    bool samePlacement(const LayerTransform& other) const;
    /// The four document corners in handle order: TL, TR, BR, BL.
    std::array<Point, 4> corners() const;
    /// Axis-aligned document bounds of the rotated rectangle.
    Rect bounds() const;

    static const std::array<Point, 8> handles; // TL, T, TR, R, BR, B, BL, L in unit coordinates
};

/// Dragging a transform: move, resize by handle, or rotate (Document/LayerTransform.swift `TransformDrag`).
struct TransformDrag {
    enum class Mode { Move, Resize, Rotate };
    LayerTransform original;
    Point start;
    Mode mode = Mode::Move;
    int handle = 0;
    LayerTransform updated(Point point, bool lockRatio, bool shift, bool option = false) const;
};

/// Snapping a moved box to guide positions (TransformSnap).
struct SnapResult { double dx = 0, dy = 0; bool snappedX = false, snappedY = false; double x = 0, y = 0; };
SnapResult snapOffset(const Rect& box, const std::vector<double>& xs, const std::vector<double>& ys, double tolerance);

} // namespace compositor
