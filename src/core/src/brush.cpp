// BrushStroke: the curve through the samples, the dab spacing, the provisional tail and the dirty area, at any depth.
// The pixels are a StrokeRaster<S> (stroke_raster.h), instantiated here for 8 bits and in brush_u16.cpp for 16.
#include "compositor/brush.h"
#include "stroke_raster.h"

namespace compositor {

template class StrokeRaster<SampleType::U8>;
extern template class StrokeRaster<SampleType::U16>;

double brushFalloff(double u) {
    const double k = 2.5;
    return std::max(0.0, (std::exp(-k * u * u) - std::exp(-k)) / (1 - std::exp(-k)));
}

BrushStroke::BrushStroke(const Layer& layer, bool mask, BrushSettings settings, Size canvas, const GrayImage* selection)
    : isMask_(mask), settings_(settings), canvas_(0, 0, canvas.width, canvas.height), name_(layer.name), layerTransform_(layer.transform) {
    raster8_ = std::make_unique<StrokeRaster<SampleType::U8>>(*this);
    valid_ = raster8_->init(layer, mask, selection);
}

BrushStroke::BrushStroke(const Layer& layer, bool mask, BrushSettings settings, Size canvas, SampleType depth, const Gray16* selection)
    : isMask_(mask), settings_(settings), canvas_(0, 0, canvas.width, canvas.height), name_(layer.name), layerTransform_(layer.transform) {
    if (depth != SampleType::U16) {
        raster8_ = std::make_unique<StrokeRaster<SampleType::U8>>(*this);
        error_ = "A stroke at this depth needs the 8-bit constructor.";
        return;
    }
    depth_ = depth;
    raster16_ = std::make_unique<StrokeRaster<SampleType::U16>>(*this);
    valid_ = raster16_->init(layer, mask, selection);
}

BrushStroke::~BrushStroke() = default;

template <class F> decltype(auto) BrushStroke::withRaster(F&& f) {
    if (raster16_) return f(*raster16_);
    return f(*raster8_);
}

template <class F> decltype(auto) BrushStroke::withRaster(F&& f) const {
    if (raster16_) return f(std::as_const(*raster16_));
    return f(std::as_const(*raster8_));
}

void BrushStroke::setMaskClone(std::shared_ptr<const GrayImage> sample) { if (raster8_) raster8_->maskClone = std::move(sample); }
void BrushStroke::setMaskClone(std::shared_ptr<const Gray16> sample) { if (raster16_) raster16_->maskClone = std::move(sample); }
ImagePtr BrushStroke::previewImage() const { return raster8_ ? raster8_->working : nullptr; }
GrayPtr BrushStroke::previewMask() const { return raster8_ ? raster8_->workingMask : nullptr; }
Image16Ptr BrushStroke::previewImage16() const { return raster16_ ? raster16_->working : nullptr; }
Gray16Ptr BrushStroke::previewMask16() const { return raster16_ ? raster16_->workingMask : nullptr; }
const Image* BrushStroke::gridBase() const { return raster8_ ? raster8_->base.get() : nullptr; }
Image* BrushStroke::gridWorking() { return raster8_ ? raster8_->working.get() : nullptr; }
const GrayImage* BrushStroke::gridSelection() const { return raster8_ ? raster8_->selection.get() : nullptr; }
const Image16* BrushStroke::gridBase16() const { return raster16_ ? raster16_->base.get() : nullptr; }
Image16* BrushStroke::gridWorking16() { return raster16_ ? raster16_->working.get() : nullptr; }
const Gray16* BrushStroke::gridSelection16() const { return raster16_ ? raster16_->selection.get() : nullptr; }
GrayImage* BrushStroke::gridCoverage() { return raster8_ ? raster8_->coverage.get() : nullptr; }
Gray16* BrushStroke::gridCoverage16() { return raster16_ ? raster16_->coverage.get() : nullptr; }

void BrushStroke::markDirty(const Rect& gridRect) {
    Rect r = gridRect.intersection(Rect(0, 0, width_, height_));
    if (r.isEmpty()) return;
    dirtyGrid_ = dirtyGrid_.unionWith(r);
    touchedGrid_ = touchedGrid_.unionWith(r);
    touched_ = true;
}

void BrushStroke::refreshLevels(const Rect& grid) const {
    // The renderer draws zoomed-out layers from reduced copies cached by image; the working pixels change in
    // place, so the copies are brought up to date where they changed (else strokes vanish when zoomed out).
    const int x0 = int(std::floor(grid.minX())), y0 = int(std::floor(grid.minY())), x1 = int(std::ceil(grid.maxX())), y1 = int(std::ceil(grid.maxY()));
    withRaster([&](const auto& raster) { raster.refreshLevels(x0, y0, x1, y1); });
}

Rect BrushStroke::takeDirtyRect() {
    if (dirtyGrid_.isEmpty()) return {};
    refreshLevels(dirtyGrid_);
    Rect doc = pixelToDocument_.mapBounds(dirtyGrid_).insetBy(-1, -1).intersection(canvas_);
    dirtyGrid_ = {};
    return doc;
}

void BrushStroke::restoreTail() {
    withRaster([&](auto& raster) { raster.restoreTail(tailRect_); });
    previous_ = tailPrevious_;
    distanceToNext_ = tailDistance_;
    hasTail_ = false;
    markDirty(tailRect_);
}

void BrushStroke::append(Point point) {
    if (!valid_ || !point.isFinite() || std::fabs(point.x) > 1e7 || std::fabs(point.y) > 1e7) return;
    if (!samples_.empty() && samples_.back() == point) return;
    // Undo the provisional tail.
    if (hasTail_) restoreTail();
    samples_.push_back(point);
    if (samples_.size() > 4) samples_.erase(samples_.begin());
    size_t n = samples_.size();
    if (n == 1) walk(point);
    else if (n >= 3) curve(samples_[n - 3], samples_[n - 2], samples_[n >= 4 ? n - 4 : 0], samples_[n - 1]);
    if (n >= 2) {
        // Draw a straight tail to the cursor, remembering what it covers so it can be taken back.
        Point start = samples_[n - 2];
        double reach = settings_.diameter / 2 + 2;
        Rect box = Rect::fromPoints(start, point).insetBy(-reach, -reach).intersection(canvas_);
        Rect affected = box.isEmpty() ? Rect() : documentToPixel_.mapBounds(box).integral().intersection(Rect(0, 0, width_, height_));
        tailPrevious_ = previous_;
        tailDistance_ = distanceToNext_;
        if (!affected.isEmpty()) {
            tailRect_ = affected;
            withRaster([&](auto& raster) { raster.saveTail(affected); });
            hasTail_ = true;
        }
        walk(point);
        previous_ = tailPrevious_;
        distanceToNext_ = tailDistance_;
    }
    if (!dirtyGrid_.isEmpty() && !deferRecompose_) recompose(dirtyGrid_);
}

void BrushStroke::appendAll(const std::vector<Point>& documentPoints) {
    deferRecompose_ = true;
    for (const Point& p : documentPoints) append(p);
    deferRecompose_ = false;
    if (!dirtyGrid_.isEmpty()) recompose(dirtyGrid_);
}

void BrushStroke::flush() {
    if (!valid_) return;
    if (hasTail_) restoreTail();
    size_t n = samples_.size();
    if (n >= 2) {
        curve(samples_[n - 2], samples_[n - 1], samples_[n >= 3 ? n - 3 : 0], samples_[n - 1]);
        samples_ = {samples_[n - 1]};
    }
    if (!dirtyGrid_.isEmpty()) recompose(dirtyGrid_);
}

void BrushStroke::curve(Point start, Point end, Point before, Point after) {
    auto knot = [](double t, Point a, Point b) { return t + std::max(0.0001, std::sqrt(std::hypot(b.x - a.x, b.y - a.y))); };
    auto mix = [](Point a, Point b, double ta, double tb, double t) {
        double wa = (tb - t) / (tb - ta), wb = (t - ta) / (tb - ta);
        return Point{a.x * wa + b.x * wb, a.y * wa + b.y * wb};
    };
    double t0 = 0, t1 = knot(t0, before, start), t2 = knot(t1, start, end), t3 = knot(t2, end, after);
    int pieces = std::max(1, int(std::ceil(std::hypot(end.x - start.x, end.y - start.y) / 2)));
    for (int index = 1; index <= pieces; index++) {
        double t = t1 + (t2 - t1) * index / pieces;
        Point a1 = mix(before, start, t0, t1, t), a2 = mix(start, end, t1, t2, t), a3 = mix(end, after, t2, t3, t);
        Point b1 = mix(a1, a2, t0, t2, t), b2 = mix(a2, a3, t1, t3, t);
        walk(index == pieces ? end : mix(b1, b2, t1, t2, t));
    }
}

void BrushStroke::walk(Point point) {
    // Hard tips merge by max, so dabs only need to be close enough that the scallop between two circles stays
    // under a tenth of a pixel (depth ~ s^2 / 8r); soft tips build up by screen and keep the dense spacing.
    const double radius = settings_.diameter / 2;
    double spacing = settings_.hardness >= 1 ? std::clamp(std::sqrt(0.8 * radius), 0.25, settings_.diameter * 0.05) : std::max(0.25, settings_.diameter * 0.025);
    if (previous_) {
        double dx = point.x - previous_->x, dy = point.y - previous_->y;
        double length = std::hypot(dx, dy);
        if (length > 0) {
            double d = distanceToNext_;
            while (d <= length) {
                dab({previous_->x + dx * d / length, previous_->y + dy * d / length});
                d += spacing;
            }
            distanceToNext_ = d - length;
        }
    } else {
        dab(point);
        distanceToNext_ = spacing;
    }
    previous_ = point;
}

void BrushStroke::dab(Point center) {
    withRaster([&](auto& raster) { raster.dab(center); });
}

std::shared_ptr<TiledSource> tiledProcessedDocument(Document document, std::function<void(Image&)> process, int margin) {
    const int w = document.width, h = document.height;
    margin = std::max(0, margin);
    auto shared = std::make_shared<Document>(std::move(document));
    return std::make_shared<TiledSource>(w, h, [shared, process = std::move(process), margin, w, h](int x, int y, int tw, int th, Image& out) {
        // The tile and the margin around it, inside the canvas (past its edges the whole image has nothing either).
        const int px0 = std::max(0, x - margin), py0 = std::max(0, y - margin);
        const int px1 = std::min(w, x + tw + margin), py1 = std::min(h, y + th + margin);
        RenderOptions options;
        options.region = Rect(px0, py0, px1 - px0, py1 - py0);
        Image padded;
        render(*shared, options, padded);
        process(padded);
        for (int row = 0; row < th; row++) std::memcpy(out.row(row), padded.pixel(x - px0, y + row - py0), size_t(tw) * 4);
    });
}

void BrushStroke::recompose(const Rect& gridRect) {
    if (painted_) return;   // another engine owns the working pixels
    Rect r = gridRect.intersection(Rect(0, 0, width_, height_));
    if (r.isEmpty()) return;
    withRaster([&](auto& raster) { raster.recompose(r); });
}

bool BrushStroke::liftSelection() {
    return withRaster([&](auto& raster) { return raster.liftSelection(); });
}

void BrushStroke::moveLifted(Point offset, bool duplicate) {
    withRaster([&](auto& raster) { raster.moveLifted(offset, duplicate); });
}

void BrushStroke::fillGradientOver(int shape, Point from, Point to, const float startColor[4], const float endColor[4], double opacity) {
    GradientStops stops;
    for (int c = 0; c < 4; c++) { stops.start[c] = startColor[c]; stops.end[c] = endColor[c]; }
    fillGradientOver(shape, from, to, stops, opacity);
}

void BrushStroke::fillGradientOver(int shape, Point from, Point to, const GradientStops& stops, double opacity) {
    if (!valid_) return;
    withRaster([&](auto& raster) { raster.fillGradientOver(shape, from, to, stops, opacity); });
}

void BrushStroke::fillColor(double red, double green, double blue) {
    float color[4] = {float(red), float(green), float(blue), 1.0f};
    fillGradientOver(0, {0, 0}, {0, 0}, color, color, 1);
}

void BrushStroke::heal() {
    withRaster([&](auto& raster) { raster.heal(); });
}

void BrushStroke::previewHeal() {
    if (!valid_ || !settings_.healing || isMask_) return;
    flush();
    heal();
}

BrushStroke::Commit BrushStroke::commit() {
    Commit result;
    result.transform = layerTransform_;
    if (!valid_) return result;
    flush();
    if (settings_.healing) heal();
    withRaster([&](auto& raster) { raster.commit(result); });
    return result;
}

} // namespace compositor
