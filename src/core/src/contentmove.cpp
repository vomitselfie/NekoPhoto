// Content-Aware Move: fill the source, place the patch with its tone adapted, resynthesise the seam.
#include "nekophoto/contentmove.h"
#include "nekophoto/depth.h"
#include "nekophoto/heal.h"
#include "nekophoto/inpaint.h"
#include "nekophoto/morphology.h"
#include "nekophoto/parallel.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace nekophoto {

bool contentAwareMove(Image& image, const GrayImage& selection, int dx, int dy, const ContentMoveOptions& options, const GrayImage* visible) {
    const int w = image.width(), h = image.height();
    if (selection.width() != w || selection.height() != h || (dx == 0 && dy == 0)) return false;
    if (visible && (visible->width() != w || visible->height() != h)) visible = nullptr;
    if (nonzeroBounds(selection).isEmpty()) return false;
    // Where the patch lands: the selection shifted.
    GrayImage landing(w, h);
    bool lands = false;
    for (int y = 0; y < h; y++) {
        const int sy = y - dy;
        if (sy < 0 || sy >= h) continue;
        for (int x = std::max(0, dx); x < std::min(w, w + dx); x++) {
            const uint8_t c = selection.at(x - dx, sy);
            landing.at(x, y) = c;
            lands |= c != 0;
        }
    }
    if (!lands) return false;
    const Image original = image;
    const int adaptation = std::clamp(options.adaptation, 0, 4);
    InpaintOptions fill;
    fill.seed = options.seed;
    fill.colorMode = options.colorMode;
    if (!options.extend) {
        // The hole a little wider than the selection, so no fringe of the moved object stays behind.
        auto hole = growSelection(selection, 2);
        if (!contentFill(image, *hole, fill, visible)) return false;
    }
    // The patch as it was, shifted to where it lands.
    Image moved(w, h);
    parallelRows(0, h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            const int sy = y - dy;
            if (sy < 0 || sy >= h) continue;
            const int xa = std::max(0, dx), xb = std::min(w, w + dx);
            if (xb > xa) std::memcpy(moved.pixel(xa, y), original.pixel(xa - dx, sy), size_t(xb - xa) * 4);
        }
    });
    // Its tone adapted to the new place, as the Healing Brush does: fully at its edge, so it meets its new
    // surroundings, fading over a feather to as much as the adaptation asks in its middle.
    const PixelBounds lb = nonzeroBounds(landing);
    const int patchSize = std::max(lb.x1 - lb.x0, lb.y1 - lb.y0);
    const int unit = std::max(2, patchSize / 48);
    const int rim = (1 + adaptation) * unit;
    const float feather = float(2 * rim);
    Image toned = image;
    healFrom(toned, moved, landing, 1.0f, visible);
    const std::vector<float> inside = squaredDistanceTransform(landing, false);
    const float adapt = float(adaptation) / 4.0f;
    parallelRows(0, h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++)
            for (int x = 0; x < w; x++) {
                const int c = landing.at(x, y);
                if (!c) continue;
                const float edge = std::clamp(1.0f - std::sqrt(inside[size_t(y) * w + x]) / feather, 0.0f, 1.0f);
                const float toward = adapt + (1.0f - adapt) * edge;
                const uint8_t* m = moved.pixel(x, y);
                const uint8_t* t = toned.pixel(x, y);
                uint8_t* p = image.pixel(x, y);
                for (int k = 0; k < 4; k++) {
                    const float patch = m[k] + (t[k] - m[k]) * toward;
                    p[k] = uint8_t(std::clamp(p[k] + (patch - p[k]) * (c / 255.0f) + 0.5f, 0.0f, 255.0f));
                }
                // Premultiplied colour never exceeds alpha.
                for (int k = 0; k < 3; k++) p[k] = std::min(p[k], p[3]);
            }
    });
    // The seam: a thin rim across the patch's edge resynthesised, copying only from outside the patch (the new
    // surroundings), so what the patch brought along (its old background) does not spread along the seam. It is
    // wider as the adaptation loosens.
    const int seam = std::max(1, rim / 3);
    auto outer = growSelection(landing, seam);
    auto inner = growSelection(landing, -seam);
    GrayImage band(w, h), sources(w, h);
    bool any = false;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const bool in = inner->at(x, y) >= 128;
            if (outer->at(x, y) >= 128 && !in) { band.at(x, y) = 255; any = true; }
            sources.at(x, y) = (in || landing.at(x, y) >= 128 || (visible && visible->at(x, y) < 128)) ? 0 : 255;
        }
    if (any) {
        fill.seed = options.seed + 7;
        Image seamed = image;
        if (contentFill(seamed, band, fill, &sources)) image = std::move(seamed);
    }
    return true;
}

namespace {
template <SampleType S> bool moveInks(ImageT<S>& image, const GrayOf<S>& selection, int dx, int dy, const ContentMoveOptions& options, const GrayOf<S>* visible);
} // namespace

bool contentAwareMove(Image16& image, const Gray16& selection16, int dx, int dy, const ContentMoveOptions& options, const Gray16* visible16) {
    if (image.channels() == 5) return moveInks<SampleType::U16>(image, selection16, dx, dy, options, visible16);
    // contentAwareMove's steps at 16 bits. The decisions (where the patch lands, the rims, the nearest-neighbour
    // fields) are made on 8-bit masks and the image rounded to 8 bits; the pixels moved, filled and blended are the
    // 16-bit ones. The tone adaptation is the Healing Brush's smooth offset, worked out at 8 bits and added to the
    // 16-bit patch, so the patch keeps its own detail at 16 bits.
    const int w = image.width(), h = image.height();
    if (selection16.width() != w || selection16.height() != h || (dx == 0 && dy == 0)) return false;
    if (visible16 && (visible16->width() != w || visible16->height() != h)) visible16 = nullptr;
    if (nonzeroBounds(selection16).isEmpty()) return false;
    GrayImage selection(w, h);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { const uint16_t v = selection16.at(x, y); selection.at(x, y) = v ? std::max<uint8_t>(1, narrow16(v)) : 0; }
    std::shared_ptr<GrayImage> visible8 = visible16 ? narrowGray(*visible16) : nullptr;
    const GrayImage* visible = visible8.get();
    GrayImage landing(w, h);
    Gray16 landing16(w, h);
    bool lands = false;
    for (int y = 0; y < h; y++) {
        const int sy = y - dy;
        if (sy < 0 || sy >= h) continue;
        for (int x = std::max(0, dx); x < std::min(w, w + dx); x++) {
            landing.at(x, y) = selection.at(x - dx, sy);
            landing16.at(x, y) = selection16.at(x - dx, sy);
            lands |= landing.at(x, y) != 0;
        }
    }
    if (!lands) return false;
    const Image16 original = image;
    const int adaptation = std::clamp(options.adaptation, 0, 4);
    InpaintOptions fill;
    fill.seed = options.seed;
    fill.colorMode = options.colorMode;
    if (!options.extend) {
        auto hole = growSelection(selection, 2);
        if (!contentFill16(image, *hole, fill, visible)) return false;
    }
    Image16 moved(w, h);
    parallelRows(0, h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            const int sy = y - dy;
            if (sy < 0 || sy >= h) continue;
            const int xa = std::max(0, dx), xb = std::min(w, w + dx);
            if (xb > xa) std::memcpy(moved.pixel(xa, y), original.pixel(xa - dx, sy), size_t(xb - xa) * 4 * sizeof(uint16_t));
        }
    });
    const PixelBounds lb = nonzeroBounds(landing);
    const int patchSize = std::max(lb.x1 - lb.x0, lb.y1 - lb.y0);
    const int unit = std::max(2, patchSize / 48);
    const int rim = (1 + adaptation) * unit;
    const float feather = float(2 * rim);
    // The heal's offset at 8 bits.
    auto moved8 = narrowImage(moved);
    auto toned8 = narrowImage(image);
    healFrom(*toned8, *moved8, landing, 1.0f, visible);
    const std::vector<float> inside = squaredDistanceTransform(landing, false);
    const float adapt = float(adaptation) / 4.0f;
    parallelRows(0, h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++)
            for (int x = 0; x < w; x++) {
                const float c = float(std::min<uint32_t>(landing16.at(x, y), one16)) / float(one16);
                if (c <= 0) continue;
                const float edge = std::clamp(1.0f - std::sqrt(inside[size_t(y) * w + x]) / feather, 0.0f, 1.0f);
                const float toward = adapt + (1.0f - adapt) * edge;
                const uint16_t* m = moved.pixel(x, y);
                const uint8_t* m8 = moved8->pixel(x, y);
                const uint8_t* t8 = toned8->pixel(x, y);
                uint16_t* p = image.pixel(x, y);
                for (int k = 0; k < 4; k++) {
                    const float toned = std::clamp(float(m[k]) + float(int(widen8(t8[k])) - int(widen8(m8[k]))), 0.0f, 32768.0f);
                    const float patch = m[k] + (toned - m[k]) * toward;
                    p[k] = uint16_t(std::clamp(p[k] + (patch - p[k]) * c + 0.5f, 0.0f, 32768.0f));
                }
                for (int k = 0; k < 3; k++) p[k] = std::min(p[k], p[3]);
            }
    });
    const int seam = std::max(1, rim / 3);
    auto outer = growSelection(landing, seam);
    auto inner = growSelection(landing, -seam);
    GrayImage band(w, h), sources(w, h);
    bool any = false;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const bool in = inner->at(x, y) >= 128;
            if (outer->at(x, y) >= 128 && !in) { band.at(x, y) = 255; any = true; }
            sources.at(x, y) = (in || landing.at(x, y) >= 128 || (visible && visible->at(x, y) < 128)) ? 0 : 255;
        }
    if (any) {
        fill.seed = options.seed + 7;
        Image16 seamed = image;
        if (contentFill16(seamed, band, fill, &sources)) image = std::move(seamed);
    }
    return true;
}

// ---- CMYK: five samples (docs/color-modes.md) ------------------------------------------------------------------
// The same steps on the inks as stored and alpha, at the image's own depth. The decisions (the hole, where the patch
// lands, the rims and the seam's sources) are 8-bit masks; the fill copies inks (inpaint.h), the patch's tone is the
// Healing Brush's CMYK membrane on every ink (heal.h), and nothing passes through RGB.

namespace {

/// An 8-bit decision mask of `mask`: anything nonzero stays nonzero.
template <SampleType S>
GrayImage decisionMask(const GrayOf<S>& mask) {
    if constexpr (S == SampleType::U8) return mask;
    else {
        GrayImage out(mask.width(), mask.height());
        for (int y = 0; y < mask.height(); y++)
            for (int x = 0; x < mask.width(); x++) { const uint16_t v = mask.at(x, y); out.at(x, y) = v ? std::max<uint8_t>(1, narrow16(v)) : 0; }
        return out;
    }
}

template <SampleType S>
bool fillInks(ImageT<S>& image, const GrayImage& hole, const InpaintOptions& options, const GrayImage* visible) {
    if constexpr (S == SampleType::U8) return contentFill(image, hole, options, visible);
    else return contentFill16(image, hole, options, visible);
}

template <SampleType S>
bool moveInks(ImageT<S>& image, const GrayOf<S>& selectionS, int dx, int dy, const ContentMoveOptions& options, const GrayOf<S>* visibleS) {
    using Sample = SampleOf<S>;
    constexpr int N = 5;
    const double one = double(SampleTraits<S>::one);
    const int w = image.width(), h = image.height();
    if (image.channels() != N || selectionS.width() != w || selectionS.height() != h || (dx == 0 && dy == 0)) return false;
    if (visibleS && (visibleS->width() != w || visibleS->height() != h)) visibleS = nullptr;
    if (nonzeroBounds(selectionS).isEmpty()) return false;
    const GrayImage selection = decisionMask<S>(selectionS);
    std::shared_ptr<GrayImage> visible8;
    if (visibleS) {
        if constexpr (S == SampleType::U8) visible8 = std::make_shared<GrayImage>(*visibleS);
        else visible8 = narrowGray(*visibleS);
    }
    const GrayImage* visible = visible8.get();
    // Where the patch lands, as decided (8-bit) and with the selection's own coverage.
    GrayImage landing(w, h);
    GrayOf<S> landingS(w, h);
    bool lands = false;
    for (int y = 0; y < h; y++) {
        const int sy = y - dy;
        if (sy < 0 || sy >= h) continue;
        for (int x = std::max(0, dx); x < std::min(w, w + dx); x++) {
            landing.at(x, y) = selection.at(x - dx, sy);
            landingS.at(x, y) = selectionS.at(x - dx, sy);
            lands |= landing.at(x, y) != 0;
        }
    }
    if (!lands) return false;
    const ImageT<S> original = image;
    const int adaptation = std::clamp(options.adaptation, 0, 4);
    InpaintOptions fill;
    fill.seed = options.seed;
    if (!options.extend) {
        auto hole = growSelection(selection, 2);
        if (!fillInks<S>(image, *hole, fill, visible)) return false;
    }
    ImageT<S> moved(w, h, N);
    parallelRows(0, h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            const int sy = y - dy;
            if (sy < 0 || sy >= h) continue;
            const int xa = std::max(0, dx), xb = std::min(w, w + dx);
            if (xb > xa) std::memcpy(moved.pixel(xa, y), original.pixel(xa - dx, sy), size_t(xb - xa) * N * sizeof(Sample));
        }
    });
    const PixelBounds lb = nonzeroBounds(landing);
    const int patchSize = std::max(lb.x1 - lb.x0, lb.y1 - lb.y0);
    const int unit = std::max(2, patchSize / 48);
    const int rim = (1 + adaptation) * unit;
    const double feather = double(2 * rim);
    // The patch toned to its new place on every ink, K included.
    ImageT<S> toned = image;
    healFrom(toned, moved, landingS, 1.0f, visibleS);
    const std::vector<float> inside = squaredDistanceTransform(landing, false);
    const double adapt = double(adaptation) / 4.0;
    parallelRows(0, h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++)
            for (int x = 0; x < w; x++) {
                const double c = std::min(double(landingS.at(x, y)), one) / one;
                if (c <= 0) continue;
                const double edge = std::clamp(1.0 - std::sqrt(double(inside[size_t(y) * w + x])) / feather, 0.0, 1.0);
                const double toward = adapt + (1.0 - adapt) * edge;
                const Sample* m = moved.pixel(x, y);
                const Sample* t = toned.pixel(x, y);
                Sample* p = image.pixel(x, y);
                for (int k = 0; k < N; k++) {
                    const double patch = m[k] + (double(t[k]) - m[k]) * toward;
                    p[k] = Sample(std::lround(std::clamp(p[k] + (patch - p[k]) * c, 0.0, one)));
                }
                // Premultiplied inks never exceed alpha.
                for (int k = 0; k < N - 1; k++) p[k] = std::min(p[k], p[N - 1]);
            }
    });
    const int seam = std::max(1, rim / 3);
    auto outer = growSelection(landing, seam);
    auto inner = growSelection(landing, -seam);
    GrayImage band(w, h), sources(w, h);
    bool any = false;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const bool in = inner->at(x, y) >= 128;
            if (outer->at(x, y) >= 128 && !in) { band.at(x, y) = 255; any = true; }
            sources.at(x, y) = (in || landing.at(x, y) >= 128 || (visible && visible->at(x, y) < 128)) ? 0 : 255;
        }
    if (any) {
        fill.seed = options.seed + 7;
        ImageT<S> seamed = image;
        if (fillInks<S>(seamed, band, fill, &sources)) image = std::move(seamed);
    }
    return true;
}

} // namespace

bool contentAwareMove(ImageC8& image, const GrayImage& selection, int dx, int dy, const ContentMoveOptions& options, const GrayImage* visible) {
    return moveInks<SampleType::U8>(image, selection, dx, dy, options, visible);
}

} // namespace nekophoto
