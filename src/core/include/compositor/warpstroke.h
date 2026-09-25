// Smudge and Liquify strokes: they push the active layer's pixels around under
// the brush, working on the layer as the canvas shows it at document size. When
// the stroke ends the result is painted back into the layer along the stroke's
// path (a clone stroke that replaces). A port of Document/SmudgeLiquify.swift.
//
// Liquify keeps a displacement field (where each pixel of the result samples the
// original) and resamples the untouched original through it after every push, so
// a long stroke never blurs; the falloff is Gustafsson's forward warp, which
// shrinks with the drag length as Photoshop's does.
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
    /// The pixels changed since the last call (document pixels: the image is at document size). Also brings
    /// the renderer's reduced copies of the image up to date there.
    Rect takeDirtyRect();
    const std::vector<Point>& points() const { return points_; }
    double diameter() const { return diameter_; }

private:
    int radius() const { return int(std::ceil(diameter_ / 2)); }
    float weight(float u) const;
    void pickUp(Point center);
    void smudge(Point center);
    void push(Point from, Point to);

    /// Liquify's accumulated displacement over the box the stroke has touched (zero beyond it): two floats
    /// per pixel, the offset from a result pixel's centre to where it samples the original.
    struct Field {
        int x0 = 0, y0 = 0, width = 0, height = 0;
        std::vector<float> offsets;
    };
    void growField(int x0, int y0, int x1, int y1);
    void fieldAt(double x, double y, float& dx, float& dy) const;

    void markDirty(int x0, int y0, int x1, int y1);   // half-open pixel box
    int dirtyX0_ = 0, dirtyY0_ = 0, dirtyX1_ = 0, dirtyY1_ = 0;   // empty when x0 >= x1
    std::shared_ptr<Image> image_;
    std::shared_ptr<const Image> original_;
    WarpMode mode_;
    double diameter_, hardness_, strength_;
    int width_, height_;
    std::vector<Point> points_;
    bool hasLast_ = false;
    Point last_;
    std::vector<float> carried_;
    Field field_;
};

} // namespace compositor
