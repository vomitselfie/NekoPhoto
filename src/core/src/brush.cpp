// BrushStroke: the curve through the samples, the dab spacing, the provisional tail and the dirty area, at any depth.
// The pixels are a StrokeRasterOf<Ops> (stroke_raster.h), instantiated here for 8 bits, in brush_u16.cpp for 16, in
// brush_f32.cpp for 32 and in brush_modes.cpp for CMYK.
#include "compositor/brush.h"
#include "stroke_raster.h"
#include "compositor/workcounters.h"

namespace compositor {

template class StrokeRasterOf<StrokeOps<SampleType::U8>>;
extern template class StrokeRasterOf<StrokeOps<SampleType::U16>>;
extern template class StrokeRasterOf<StrokeOps<SampleType::F32>>;
extern template class StrokeRasterOf<CmykStrokeOps<SampleType::U8>>;
extern template class StrokeRasterOf<CmykStrokeOps<SampleType::U16>>;

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

BrushStroke::BrushStroke(const Layer& layer, bool mask, BrushSettings settings, const Document& document, const ConvertOptions& options)
    : isMask_(mask), settings_(settings), canvas_(0, 0, document.width, document.height), name_(layer.name), layerTransform_(layer.transform) {
    depth_ = document.sampleType;
    mode_ = colorModeSupportsDepth(document.colorMode, depth_) ? document.colorMode : ColorMode::RGB;
    if (depth_ == SampleType::F32) {
        // The colour pickers hold encoded colour; a 32-bit document holds linear light.
        curve_ = documentTransfer(document);
        const TransferCurve curve = *curve_;
        toNative_ = [curve](const float rgb[3], double out[4]) {
            for (int c = 0; c < 3; c++) out[c] = curve.toLinear(std::clamp(rgb[c], 0.0f, 1.0f));
            out[3] = 0;
        };
    } else if (mode_ != ColorMode::RGB) {
        // The colour (sRGB, as CMYK and Lab documents keep colours) through the document's profile, as Little CMS's
        // float colours: CMYK ink 0..100 with the profile's black generation, or L 0..100 and signed a, b.
        const ColorMode m = mode_;
        const SampleType depth = depth_;
        ColorTransformPtr t = transformBetween(ColorProfile(), document.profile, options, PixelFormat::RGBFloat,
                                                             m == ColorMode::CMYK ? PixelFormat::CMYKFloat : PixelFormat::LabFloat);
        if (!t) { raster8_ = std::make_unique<StrokeRaster<SampleType::U8>>(*this); error_ = "The document's colour profile cannot be used."; return; }
        toNative_ = [t, m, depth](const float rgb[3], double out[4]) {
            const float in[3] = {std::clamp(rgb[0], 0.0f, 1.0f), std::clamp(rgb[1], 0.0f, 1.0f), std::clamp(rgb[2], 0.0f, 1.0f)};
            float v[4] = {0, 0, 0, 0};
            t->apply(in, v, 1);
            if (m == ColorMode::CMYK) {
                for (int c = 0; c < 4; c++) out[c] = 1 - std::clamp(double(v[c]) / 100.0, 0.0, 1.0);   // stored inverted
                return;
            }
            const double one = depth == SampleType::U16 ? double(one16) : 255.0;
            const double offset = depth == SampleType::U16 ? labOffset<SampleType::U16>() : labOffset<SampleType::U8>();
            const double scale = depth == SampleType::U16 ? labScale<SampleType::U16>() : labScale<SampleType::U8>();
            out[0] = std::clamp(double(v[0]) / 100.0, 0.0, 1.0);
            out[1] = std::clamp((double(v[1]) * scale + offset) / one, 0.0, 1.0);
            out[2] = std::clamp((double(v[2]) * scale + offset) / one, 0.0, 1.0);
            out[3] = 0;
        };
    }
    const AnyGray* coverage = document.selection ? &document.selection->coverage : nullptr;
    initRaster(layer, mask, coverage);
}

void BrushStroke::initRaster(const Layer& layer, bool mask, const AnyGray* selection) {
    auto make = [&](auto& raster, const auto* gray) {
        using R = typename std::remove_reference_t<decltype(raster)>::element_type;
        raster = std::make_unique<R>(*this);
        if (selection && *selection && !gray) { error_ = "The selection is not at the document's depth."; return; }
        valid_ = raster->init(layer, mask, gray);
    };
    const bool cmyk = mode_ == ColorMode::CMYK && !mask;
    switch (depth_) {
    case SampleType::F32: make(rasterF_, selection ? selection->f32().get() : nullptr); break;
    case SampleType::U16:
        if (cmyk) make(rasterC16_, selection ? selection->u16().get() : nullptr);
        else make(raster16_, selection ? selection->u16().get() : nullptr);
        break;
    case SampleType::U8:
        if (cmyk) make(rasterC8_, selection ? selection->u8().get() : nullptr);
        else make(raster8_, selection ? selection->u8().get() : nullptr);
        break;
    }
}

BrushStroke::~BrushStroke() = default;

template <class F> decltype(auto) BrushStroke::withRaster(F&& f) {
    if (raster16_) return f(*raster16_);
    if (rasterF_) return f(*rasterF_);
    if (rasterC8_) return f(*rasterC8_);
    if (rasterC16_) return f(*rasterC16_);
    return f(*raster8_);
}

template <class F> decltype(auto) BrushStroke::withRaster(F&& f) const {
    if (raster16_) return f(std::as_const(*raster16_));
    if (rasterF_) return f(std::as_const(*rasterF_));
    if (rasterC8_) return f(std::as_const(*rasterC8_));
    if (rasterC16_) return f(std::as_const(*rasterC16_));
    return f(std::as_const(*raster8_));
}

void BrushStroke::nativeColor(const float rgb[3], double out[4]) const {
    if (toNative_) { toNative_(rgb, out); return; }
    for (int c = 0; c < 3; c++) out[c] = rgb[c];
    out[3] = 0;
}

void BrushStroke::setMaskClone(std::shared_ptr<const GrayImage> sample) { if (raster8_) raster8_->maskClone = std::move(sample); }
void BrushStroke::setMaskClone(std::shared_ptr<const Gray16> sample) { if (raster16_) raster16_->maskClone = std::move(sample); }
void BrushStroke::setMaskClone(std::shared_ptr<const GrayF> sample) { if (rasterF_) rasterF_->maskClone = std::move(sample); }
ImagePtr BrushStroke::previewImage() const { return raster8_ ? raster8_->working : nullptr; }
GrayPtr BrushStroke::previewMask() const { return raster8_ ? raster8_->workingMask : nullptr; }
Image16Ptr BrushStroke::previewImage16() const { return raster16_ ? raster16_->working : rasterC16_ ? rasterC16_->working : nullptr; }
Gray16Ptr BrushStroke::previewMask16() const { return raster16_ ? raster16_->workingMask : nullptr; }
AnyImage BrushStroke::preview() const {
    if (raster8_) return ImagePtr(raster8_->working);
    if (raster16_) return Image16Ptr(raster16_->working);
    if (rasterF_) return ImageFPtr(rasterF_->working);
    if (rasterC8_) return ImageC8Ptr(rasterC8_->working);
    if (rasterC16_) return Image16Ptr(rasterC16_->working);
    return {};
}
AnyGray BrushStroke::previewMaskAny() const {
    if (raster8_) return GrayPtr(raster8_->workingMask);
    if (raster16_) return Gray16Ptr(raster16_->workingMask);
    if (rasterF_) return GrayFPtr(rasterF_->workingMask);
    return {};
}
const Image* BrushStroke::gridBase() const { return raster8_ ? raster8_->base.get() : nullptr; }
Image* BrushStroke::gridWorking() { return raster8_ ? raster8_->working.get() : nullptr; }
const GrayImage* BrushStroke::gridSelection() const { return raster8_ ? raster8_->selection.get() : nullptr; }
const Image16* BrushStroke::gridBase16() const { return raster16_ ? raster16_->base.get() : nullptr; }
Image16* BrushStroke::gridWorking16() { return raster16_ ? raster16_->working.get() : nullptr; }
const Gray16* BrushStroke::gridSelection16() const { return raster16_ ? raster16_->selection.get() : nullptr; }
const ImageF* BrushStroke::gridBaseF() const { return rasterF_ ? rasterF_->base.get() : nullptr; }
ImageF* BrushStroke::gridWorkingF() { return rasterF_ ? rasterF_->working.get() : nullptr; }
const GrayF* BrushStroke::gridSelectionF() const { return rasterF_ ? rasterF_->selection.get() : nullptr; }
GrayImage* BrushStroke::gridCoverage() { return raster8_ ? raster8_->coverage.get() : rasterC8_ ? rasterC8_->coverage.get() : nullptr; }
Gray16* BrushStroke::gridCoverage16() {
    if (raster16_) return raster16_->coverage.get();
    if (rasterC16_) return rasterC16_->coverage.get();
    if (rasterF_) return rasterF_->proxyCoverage();
    return nullptr;
}

void BrushStroke::recomposeCovered(const Rect& gridRect) {
    if (rasterF_) rasterF_->syncCoverage(gridRect.intersection(Rect(0, 0, width_, height_)));
    markDirty(gridRect);
    recompose(gridRect);
}

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
    work::add(work::Counter::BrushDabs);
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
    work::add(work::Counter::BrushRecomposePixels, uint64_t(r.width) * uint64_t(r.height));
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
    // Perceptual and Linear: drawn from their colours baked into a Classic ramp (any depth and mode).
    if (stops.method != GradientMethod::Classic) { fillGradientOver(shape, from, to, stops.baked(), opacity); return; }
    if (!toNative_ || (isMask_ && !rasterF_)) {
        withRaster([&](auto& raster) { raster.fillGradientOver(shape, from, to, stops, opacity); });
        return;
    }
    // Each stop in the document's model, the gradient then running between them there (Photoshop blends a CMYK or
    // Lab document's gradient in its mode, a 32-bit one's in linear light): the first three samples in `native`, the
    // fourth (CMYK's black) in `fourth`'s red. A 32-bit mask takes the gray as coverage, not linearised.
    GradientStops native = stops, fourth = stops;
    auto convert = [&](const float rgb[3], float first[3], float& extra) {
        if (isMask_) { for (int c = 0; c < 3; c++) first[c] = rgb[c]; extra = 0; return; }
        double v[4];
        nativeColor(rgb, v);
        for (int c = 0; c < 3; c++) first[c] = float(v[c]);
        extra = float(v[3]);
    };
    float e = 0;
    convert(stops.start, native.start, e); fourth.start[0] = e;
    convert(stops.end, native.end, e); fourth.end[0] = e;
    for (size_t i = 0; i < stops.colors.size(); i++) { convert(stops.colors[i].rgb, native.colors[i].rgb, e); fourth.colors[i].rgb[0] = e; }
    // The two-stop form lerps its alpha with the colour; the fourth's copy carries the same alpha, so it matches.
    withRaster([&](auto& raster) { raster.fillGradientNative(shape, from, to, native, mode_ == ColorMode::CMYK && !isMask_ ? &fourth : nullptr, opacity); });
}


void BrushStroke::fillColor(double red, double green, double blue) {
    float color[4] = {float(red), float(green), float(blue), 1.0f};
    fillGradientOver(0, {0, 0}, {0, 0}, color, color, 1);
}

void BrushStroke::heal() {
    withRaster([&](auto& raster) { raster.heal(); });
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
