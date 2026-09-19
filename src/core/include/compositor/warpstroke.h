// Smudge and Liquify strokes: they push the active layer's pixels around under
// the brush, working on the layer as the canvas shows it at document size. When
// the stroke ends the result is painted back into the layer along the stroke's
// path (a clone stroke that replaces). A port of Document/SmudgeLiquify.swift.
#pragma once
#include "image.h"
#include "geometry.h"
#include <memory>
#include <vector>

namespace compositor {

enum class WarpMode { Liquify, Smudge };

class WarpStroke {
public:
    /// `image` is the layer rendered at document size (`width` x `height`).
    WarpStroke(std::shared_ptr<Image> image, WarpMode mode, double diameter, double hardness, double strength);
    void append(Point point);
    std::shared_ptr<const Image> image() const { return image_; }
    const std::vector<Point>& points() const { return points_; }
    double diameter() const { return diameter_; }

private:
    int radius() const { return int(std::ceil(diameter_ / 2)); }
    float weight(float u) const;
    void pickUp(Point center);
    void smudge(Point center);
    void push(Point from, Point to);

    std::shared_ptr<Image> image_;
    WarpMode mode_;
    double diameter_, hardness_, strength_;
    int width_, height_;
    std::vector<Point> points_;
    bool hasLast_ = false;
    Point last_;
    std::vector<float> carried_, scratch_;
};

} // namespace compositor
