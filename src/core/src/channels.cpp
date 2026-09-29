// Alpha and spot channels, and the colour channels as views (channels.h, docs/channels.md).
#include "compositor/channels.h"
#include "compositor/colormgmt.h"
#include "compositor/depth.h"
#include "compositor/parallel.h"
#include "compositor/render.h"
#include "compositor/resample.h"
#include "compositor/selection.h"
#include <algorithm>
#include <bit>
#include <cmath>

namespace compositor {

namespace {

/// A document-sized gray at `depth` holding `value` (0..1).
AnyGray solidGray(int width, int height, SampleType depth, bool white) {
    if (depth == SampleType::F32) return GrayFPtr(std::make_shared<GrayF>(width, height, white ? 1.0f : 0.0f));
    if (depth == SampleType::U16) return Gray16Ptr(std::make_shared<Gray16>(width, height, white ? uint16_t(one16) : uint16_t(0)));
    return GrayPtr(std::make_shared<GrayImage>(width, height, white ? uint8_t(255) : uint8_t(0)));
}

/// `gray` inverted, at its depth.
AnyGray invertedGray(const AnyGray& gray) {
    if (const GrayFPtr& g = gray.f32()) {
        auto out = std::make_shared<GrayF>(g->width(), g->height());
        const size_t n = size_t(g->width()) * g->height();
        for (size_t i = 0; i < n; i++) out->data()[i] = 1.0f - cleanCoverage(g->data()[i]);
        return GrayFPtr(out);
    }
    if (const Gray16Ptr& g = gray.u16()) {
        auto out = std::make_shared<Gray16>(g->width(), g->height());
        const size_t n = size_t(g->width()) * g->height();
        const uint16_t* s = g->data();
        uint16_t* d = out->data();
        for (size_t i = 0; i < n; i++) d[i] = uint16_t(one16 - std::min<uint32_t>(s[i], one16));
        return Gray16Ptr(out);
    }
    if (const GrayPtr& g = gray.u8()) {
        auto out = std::make_shared<GrayImage>(g->width(), g->height());
        const size_t n = size_t(g->width()) * g->height();
        const uint8_t* s = g->data();
        uint8_t* d = out->data();
        for (size_t i = 0; i < n; i++) d[i] = uint8_t(255 - s[i]);
        return GrayPtr(out);
    }
    return {};
}

/// A gray brought to `depth` and to `width` x `height` (cut or padded with black), shared when it already fits.
AnyGray fitGray(const AnyGray& gray, SampleType depth, int width, int height) {
    AnyGray g = grayAtDepth(gray, depth);
    if (!g || (g.width() == width && g.height() == height)) return g;
    return g.visit([&](const auto& p) -> AnyGray {
        using G = std::remove_cvref_t<decltype(*p)>;
        auto out = std::make_shared<G>(width, height);
        if (p)
            for (int y = 0; y < std::min(height, p->height()); y++)
                std::copy(p->row(y), p->row(y) + std::min(width, p->width()), out->row(y));
        return std::shared_ptr<const G>(out);
    });
}

/// The document's composite at its depth, over white, as one gray: luminosity, or one colour channel (0..2).
AnyGray compositeGray(const Document& document, int channel) {
    auto pick = [&](auto r, auto g, auto b, uint32_t one) -> uint32_t {
        if (channel == 0) return r;
        if (channel == 1) return g;
        if (channel == 2) return b;
        // Photoshop's luminosity (Ctrl+Alt+2): 0.30, 0.59, 0.11.
        return std::min<uint32_t>(one, uint32_t((30 * r + 59 * g + 11 * b + 50) / 100));
    };
    if (document.sampleType == SampleType::U16 || document.sampleType == SampleType::F32) {
        // 32 bits: the composite encoded at exposure 0 (the decision image's tones, docs/bit-depth.md), then as at 16.
        auto flat = document.sampleType == SampleType::F32 ? encodeImage16(*renderFlattenedF(document), encodedTransfer(document)) : renderFlattened16(document);
        auto out = std::make_shared<Gray16>(document.width, document.height);
        for (int y = 0; y < document.height; y++) {
            const uint16_t* s = flat->row(y);
            uint16_t* d = out->row(y);
            for (int x = 0; x < document.width; x++, s += 4) {
                const uint32_t white = one16 - std::min<uint32_t>(s[3], one16);
                d[x] = uint16_t(pick(std::min(one16, s[0] + white), std::min(one16, s[1] + white), std::min(one16, s[2] + white), one16));
            }
        }
        if (document.sampleType == SampleType::F32) return GrayFPtr(widenGrayF(*out));
        return Gray16Ptr(out);
    }
    auto flat = renderFlattened(document);
    auto out = std::make_shared<GrayImage>(document.width, document.height);
    for (int y = 0; y < document.height; y++) {
        const uint8_t* s = flat->row(y);
        uint8_t* d = out->row(y);
        for (int x = 0; x < document.width; x++, s += 4) {
            const uint32_t white = 255u - s[3];
            d[x] = uint8_t(pick(std::min(255u, s[0] + white), std::min(255u, s[1] + white), std::min(255u, s[2] + white), 255u));
        }
    }
    return GrayPtr(out);
}

/// One colour channel of a CMYK or Lab document's composite as a selection: a CMYK channel's ink (transparency is
/// paper, no ink), a Lab channel's value over white (L 100, a and b neutral).
AnyGray modeChannelGray(const Document& document, int channel) {
    const AnyImage flat = renderNative(document);
    const int n = flat.channels(), alpha = n - 1;
    const bool cmyk = document.colorMode == ColorMode::CMYK;
    auto value = [&](uint32_t stored, uint32_t a, uint32_t one) -> uint32_t {
        // Over white: stored + (one - a) for L and for CMYK's inverted ink (paper), the neutral point for a and b.
        const uint32_t paper = cmyk || channel == 0 ? one : (one == 255 ? 128u : 16384u);
        const uint32_t over = std::min<uint32_t>(one, stored + uint32_t((uint64_t(paper) * (one - std::min(a, one)) + one / 2) / one));
        return cmyk ? one - over : over;
    };
    if (flat.u16()) {
        const Image16& img = *flat.u16();
        auto out = std::make_shared<Gray16>(document.width, document.height);
        for (int y = 0; y < document.height; y++) {
            const uint16_t* s = img.row(y);
            uint16_t* d = out->row(y);
            for (int x = 0; x < document.width; x++, s += n) d[x] = uint16_t(value(s[channel], s[alpha], one16));
        }
        return Gray16Ptr(out);
    }
    auto out = std::make_shared<GrayImage>(document.width, document.height);
    auto fill = [&](const auto& img) {
        for (int y = 0; y < document.height; y++) {
            const uint8_t* s = img.row(y);
            uint8_t* d = out->row(y);
            for (int x = 0; x < document.width; x++, s += n) d[x] = uint8_t(value(s[channel], s[alpha], 255));
        }
    };
    if (flat.c8()) fill(*flat.c8());
    else if (flat.u8()) fill(*flat.u8());
    return GrayPtr(out);
}

/// Where `after`'s grid sits on `before`'s, in whole pixels, when the two are the same scale and turn.
bool gridOffset(const LayerTransform& before, int beforeWidth, int beforeHeight, const LayerTransform& after, int afterWidth, int afterHeight, int& dx, int& dy) {
    if (before.rotation != after.rotation || before.flipX != after.flipX || before.flipY != after.flipY) return false;
    if (before.rotation != 0 || before.flipX || before.flipY) {
        // A turned or flipped layer: only an edit on the very same grid.
        if (!(before == after) || beforeWidth != afterWidth || beforeHeight != afterHeight) return false;
        dx = dy = 0;
        return true;
    }
    const double sx = before.size.width / std::max(1, beforeWidth), sy = before.size.height / std::max(1, beforeHeight);
    const double ax = after.size.width / std::max(1, afterWidth), ay = after.size.height / std::max(1, afterHeight);
    if (std::fabs(sx - ax) > 1e-9 || std::fabs(sy - ay) > 1e-9 || sx <= 0 || sy <= 0) return false;
    const double fx = (after.origin.x - before.origin.x) / sx, fy = (after.origin.y - before.origin.y) / sy;
    if (std::fabs(fx - std::round(fx)) > 1e-6 || std::fabs(fy - std::round(fy)) > 1e-6) return false;
    dx = int(std::lround(fx));
    dy = int(std::lround(fy));
    return true;
}

template <class Img>
std::shared_ptr<Img> keepChannels(const Img& before, const Img* after, int dx, int dy, unsigned channels, uint32_t one) {
    // 4 samples a pixel (RGB, Lab) or 5 (CMYK); alpha last.
    int n = 4;
    std::shared_ptr<Img> out;
    if constexpr (requires { before.channels(); }) { n = before.channels(); out = std::make_shared<Img>(before.width(), before.height(), n); }
    else out = std::make_shared<Img>(before.width(), before.height());
    if (after) { if constexpr (requires { after->channels(); }) if (after->channels() != n) after = nullptr; }
    const int alpha = n - 1;
    parallelRows(0, before.height(), [&](int y0, int y1) {
    for (int y = y0; y < y1; y++) {
        const auto* b = before.row(y);
        auto* o = out->row(y);
        const int ay = y - dy;
        const bool rowIn = after && ay >= 0 && ay < after->height();
        for (int x = 0; x < before.width(); x++, b += n, o += n) {
            const uint32_t A = b[alpha];
            o[alpha] = b[alpha];
            const int ax = x - dx;
            const bool in = rowIn && ax >= 0 && ax < after->width();
            for (int c = 0; c < alpha; c++) {
                o[c] = b[c];
                if (!(channels >> c & 1) || A == 0 || !in) continue;
                const auto* a = after->pixel(ax, ay);
                const uint32_t aa = a[alpha];
                if (aa == A) { o[c] = a[c]; continue; }   // the usual case: the edit kept the alpha, its colour is exact
                if (aa == 0) continue;
                // The edit's straight colour at the old alpha.
                const uint64_t straight = std::min<uint64_t>(one, (uint64_t(a[c]) * one + aa / 2) / aa);
                o[c] = static_cast<std::remove_cvref_t<decltype(o[c])>>(std::min<uint64_t>(A, (straight * A + one / 2) / one));
            }
        }
    }
    }, 32);
    return out;
}

/// keepChannels at 32 bits: float colour, unbounded; the edit's straight colour at the old alpha where it changed alpha.
std::shared_ptr<ImageF> keepChannelsF(const ImageF& before, const ImageF* after, int dx, int dy, unsigned channels) {
    auto out = std::make_shared<ImageF>(before);
    parallelRows(0, before.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            const int ay = y - dy;
            if (!after || ay < 0 || ay >= after->height()) continue;
            float* o = out->row(y);
            for (int x = 0; x < before.width(); x++, o += 4) {
                const int ax = x - dx;
                const float A = o[3];
                if (ax < 0 || ax >= after->width() || !(A > 0)) continue;
                const float* a = after->pixel(ax, ay);
                for (int c = 0; c < 3; c++) {
                    if (!(channels >> c & 1)) continue;
                    if (a[3] == A) o[c] = a[c];
                    else if (a[3] > 0) o[c] = a[c] / a[3] * A;
                }
            }
        }
    }, 32);
    return out;
}

template <class G>
std::shared_ptr<G> cropGrayPadded(const G& g, int x0, int y0, int width, int height) {
    auto out = std::make_shared<G>(width, height);
    for (int y = 0; y < height; y++) {
        const int sy = y + y0;
        if (sy < 0 || sy >= g.height()) continue;
        const int xa = std::max(0, -x0), xb = std::min(width, g.width() - x0);
        if (xb > xa) std::copy(g.row(sy) + xa + x0, g.row(sy) + xb + x0, out->row(y) + xa);
    }
    return out;
}

template <class G>
std::shared_ptr<G> flippedGray(const G& g, bool horizontal) {
    auto out = std::make_shared<G>(g.width(), g.height());
    for (int y = 0; y < g.height(); y++)
        for (int x = 0; x < g.width(); x++)
            out->at(x, y) = g.at(horizontal ? g.width() - 1 - x : x, horizontal ? y : g.height() - 1 - y);
    return out;
}

} // namespace

// ---- The document's channels -----------------------------------------------------------------------------

const Channel* findChannel(const Document& document, const Uuid& id) {
    for (const Channel& c : document.channels) if (c.id == id) return &c;
    return nullptr;
}

Channel* findChannel(Document& document, const Uuid& id) {
    for (Channel& c : document.channels) if (c.id == id) return &c;
    return nullptr;
}

int channelIndex(const Document& document, const Uuid& id) {
    for (size_t i = 0; i < document.channels.size(); i++) if (document.channels[i].id == id) return int(i);
    return -1;
}

std::string nextChannelName(const Document& document, const std::string& prefix) {
    for (int n = 1;; n++) {
        const std::string name = prefix + " " + std::to_string(n);
        if (std::none_of(document.channels.begin(), document.channels.end(), [&](const Channel& c) { return c.name == name; })) return name;
    }
}

std::string channelAddProblem(const Document& document) {
    if (int(document.channels.size()) >= Document::maxChannels)
        return "A document holds at most " + std::to_string(Document::maxChannels) + " alpha and spot channels (56 channels with the colour channels).";
    const long long pixels = (long long)document.width * document.height;
    const long long project = document.projectPixelBudgetAt();
    if (pixels > project || document.maskPixels() > project - pixels) return "The masks and channels would exceed the document's memory budget.";
    return {};
}

Channel makeAlphaChannel(const Document& document, std::string name, const AnyGray& coverage) {
    Channel c;
    c.id = makeUuid();
    c.name = std::move(name);
    c.kind = ChannelKind::Alpha;
    c.image = coverage ? fitGray(coverage, document.sampleType, document.width, document.height) : solidGray(document.width, document.height, document.sampleType, false);
    return c;
}

AnyGray channelCoverage(const Channel& channel, SampleType depth) {
    AnyGray g = grayAtDepth(channel.image, depth);
    return channel.selectedAreas ? invertedGray(g) : g;
}

void saveSelectionInto(Channel& channel, const std::optional<Selection>& selection, SelectionMode mode, SampleType depth, int width, int height) {
    std::optional<Selection> current;
    current.emplace();
    current->coverage = fitGray(channelCoverage(channel, depth), depth, width, height);
    const AnyGray shape = selection && selection->coverage ? fitGray(selection->coverage, depth, width, height) : solidGray(width, height, depth, false);
    std::optional<Selection> combined = combineSelection(current, shape, mode, true, depth);
    AnyGray result = combined && combined->coverage ? fitGray(combined->coverage, depth, width, height) : solidGray(width, height, depth, false);
    channel.image = channel.selectedAreas ? invertedGray(result) : result;
}

void setSelectedAreas(Channel& channel, bool selectedAreas) {
    if (channel.selectedAreas == selectedAreas) return;
    channel.selectedAreas = selectedAreas;
    channel.image = invertedGray(channel.image);
}

AnyGray selectionSourceCoverage(const Document& document, const SelectionSource& source, std::string* error) {
    auto fail = [&](const char* why) { if (error) *error = why; return AnyGray(); };
    switch (source.kind) {
    case SelectionSource::AlphaChannel: {
        const Channel* channel = findChannel(document, source.id);
        if (!channel) return fail("There is no channel with that id.");
        return fitGray(channelCoverage(*channel, document.sampleType), document.sampleType, document.width, document.height);
    }
    case SelectionSource::Composite: return compositeGray(document, 3);
    case SelectionSource::Red: return document.colorMode == ColorMode::RGB ? compositeGray(document, 0) : modeChannelGray(document, 0);
    case SelectionSource::Green: return document.colorMode == ColorMode::RGB ? compositeGray(document, 1) : modeChannelGray(document, 1);
    case SelectionSource::Blue: return document.colorMode == ColorMode::RGB ? compositeGray(document, 2) : modeChannelGray(document, 2);
    case SelectionSource::Black:
        if (document.colorMode != ColorMode::CMYK) return fail("Only CMYK documents have a Black channel.");
        return modeChannelGray(document, 3);
    case SelectionSource::Transparency: {
        const Layer* layer = document.find(source.id);
        if (!layer || layer->isGroup) return fail("There is no pixel layer with that id.");
        if (document.sampleType == SampleType::F32) return GrayFPtr(coverageFromLayerF(document, *layer));
        if (document.sampleType == SampleType::U16) return Gray16Ptr(coverageFromLayer16(document, *layer));
        return GrayPtr(coverageFromLayer(document, *layer));
    }
    case SelectionSource::LayerMask: {
        const Layer* layer = document.find(source.id);
        if (!layer || !layer->mask || !layer->mask->asset.image) return fail("That layer has no mask.");
        // The mask as it sits on the canvas; beyond a placed mask, the colour most of its edge is.
        const uint8_t background = layer->mask->placement && layer->mask->asset.thumbnail ? LayerMask::background(*layer->mask->asset.thumbnail) : 0;
        if (const GrayFPtr& m = layer->mask->asset.image.f32()) {
            auto out = std::make_shared<GrayF>(document.width, document.height, background / 255.0f);
            sampleMaskCoverage(*m, layer->maskTransform(), document.rect(), 1, background / 255.0f, *out, false);
            return grayAtDepth(GrayFPtr(out), document.sampleType);
        }
        if (const Gray16Ptr& m = layer->mask->asset.image.u16()) {
            auto out = std::make_shared<Gray16>(document.width, document.height, widen8(background));
            sampleMaskCoverage(*m, layer->maskTransform(), document.rect(), 1, widen8(background), *out, false);
            return grayAtDepth(Gray16Ptr(out), document.sampleType);
        }
        auto out = std::make_shared<GrayImage>(document.width, document.height, background);
        sampleMaskCoverage(*layer->mask->asset.image.u8(), layer->maskTransform(), document.rect(), 1, background, *out, false);
        return grayAtDepth(GrayPtr(out), document.sampleType);
    }
    }
    return fail("Unknown selection source.");
}

std::optional<Selection> loadSelectionFrom(const Document& document, const SelectionSource& source, bool invert, SelectionMode mode, bool antialiased, std::string* error) {
    AnyGray coverage = selectionSourceCoverage(document, source, error);
    if (!coverage) return std::nullopt;
    if (invert) coverage = invertedGray(coverage);
    std::optional<Selection> result = combineSelection(document.selection, coverage, mode, antialiased, document.sampleType);
    if (!result) {
        // Subtracting from or intersecting with no selection selects nothing.
        result.emplace();
        result->coverage = solidGray(document.width, document.height, document.sampleType, false);
        result->antialiased = antialiased;
    }
    return result;
}

SelectionMode thumbnailClickMode(bool shift, bool alt) {
    if (shift && alt) return SelectionMode::Intersect;
    if (shift) return SelectionMode::Add;
    if (alt) return SelectionMode::Subtract;
    return SelectionMode::Replace;
}

// ---- The canvas under the channels -------------------------------------------------------------------------

void cropChannels(Document& document, int x, int y, int width, int height) {
    for (Channel& c : document.channels)
        c.image = c.image.visit([&](const auto& p) -> AnyGray {
            if (!p) return AnyGray();
            return std::shared_ptr<const std::remove_cvref_t<decltype(*p)>>(cropGrayPadded(*p, x, y, width, height));
        });
}

void flipChannels(Document& document, bool horizontal) {
    for (Channel& c : document.channels)
        c.image = c.image.visit([&](const auto& p) -> AnyGray {
            if (!p) return AnyGray();
            return std::shared_ptr<const std::remove_cvref_t<decltype(*p)>>(flippedGray(*p, horizontal));
        });
}

void resampleChannels(Document& document, int fromWidth, int fromHeight, Sampling sampling) {
    const int w = document.width, h = document.height;
    const double sx = double(fromWidth) / w, sy = double(fromHeight) / h;
    const ResampleFilter filter = filterFor(sampling);
    for (Channel& c : document.channels) {
        if (c.image.width() == w && c.image.height() == h) continue;
        if (const GrayFPtr& g = c.image.f32()) {
            if (sampling == Sampling::Nearest) {
                auto out = std::make_shared<GrayF>(w, h);
                for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) out->at(x, y) = g->at(std::min(g->width() - 1, int((x + 0.5) * sx)), std::min(g->height() - 1, int((y + 0.5) * sy)));
                c.image = GrayFPtr(out);
            } else c.image = GrayFPtr(resampleAxisAligned(*g, w, h, 0.5 * sx, sx, 0.5 * sy, sy, filter, 0.0f));
        } else if (const Gray16Ptr& g = c.image.u16()) {
            if (sampling == Sampling::Nearest) {
                auto out = std::make_shared<Gray16>(w, h);
                for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) out->at(x, y) = g->at(std::min(g->width() - 1, int((x + 0.5) * sx)), std::min(g->height() - 1, int((y + 0.5) * sy)));
                c.image = Gray16Ptr(out);
            } else c.image = Gray16Ptr(resampleAxisAligned(*g, w, h, 0.5 * sx, sx, 0.5 * sy, sy, filter, 0));
        } else if (const GrayPtr& g = c.image.u8()) {
            if (sampling == Sampling::Nearest) {
                auto out = std::make_shared<GrayImage>(w, h);
                for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) out->at(x, y) = g->at(std::min(g->width() - 1, int((x + 0.5) * sx)), std::min(g->height() - 1, int((y + 0.5) * sy)));
                c.image = GrayPtr(out);
            } else c.image = GrayPtr(resampleAxisAligned(*g, w, h, 0.5 * sx, sx, 0.5 * sy, sy, filter, 0));
        }
        c.psdCarry.reset();
    }
}

// ---- Single-channel editing ----------------------------------------------------------------------------------

AnyImage keepColorChannels(const AnyImage& before, const AnyImage& after, int dx, int dy, unsigned channels) {
    if (!before) return {};
    if (const ImageFPtr& b = before.f32()) {
        const ImageFPtr a = after ? after.f32() : nullptr;
        return ImageFPtr(keepChannelsF(*b, a.get(), dx, dy, channels));
    }
    if (const Image16Ptr& b = before.u16()) {
        const Image16Ptr a = after ? imageAtDepth(after, SampleType::U16).u16() : nullptr;
        return Image16Ptr(keepChannels(*b, a.get(), dx, dy, channels, one16));
    }
    if (const ImagePtr& b = before.u8()) {
        const ImagePtr a = after ? imageAtDepth(after, SampleType::U8).u8() : nullptr;
        return ImagePtr(keepChannels(*b, a.get(), dx, dy, channels, 255u));
    }
    if (const ImageC8Ptr& b = before.c8()) {
        const AnyImage a = after ? imageAtFormat(after, SampleType::U8, ColorMode::CMYK) : AnyImage();
        return ImageC8Ptr(keepChannels(*b, a.c8().get(), dx, dy, channels, 255u));
    }
    return before;
}

AnyImage keepColorChannels(const AnyImage& before, const LayerTransform& beforeTransform, const AnyImage& after, const LayerTransform& afterTransform,
                           unsigned channels) {
    int dx = 0, dy = 0;
    if (!before || !after || !gridOffset(beforeTransform, before.width(), before.height(), afterTransform, after.width(), after.height(), dx, dy)) return {};
    return keepColorChannels(before, after, dx, dy, channels);
}

bool restrictToColorChannels(const Document& before, Document& after, unsigned channels) {
    const unsigned all = colorChannelsAllFor(after.colorMode);
    if ((channels & all) == all) return false;
    // Edits of the canvas or of the layer structure (Image Size, Crop, merges) work on whole layers, as in Photoshop.
    if (before.width != after.width || before.height != after.height || before.layers.size() != after.layers.size()) return false;
    for (size_t i = 0; i < after.layers.size(); i++) if (before.layers[i].id != after.layers[i].id) return false;
    bool changed = false;
    for (Layer& layer : after.layers) {
        const Layer* old = before.find(layer.id);
        if (!old || old->isGroup || layer.isGroup) continue;
        // Applying or adding a mask, rasterizing: not a pixel edit of the layer's colour.
        if (bool(old->mask) != bool(layer.mask)) continue;
        const AnyImage was = old->asset ? old->asset->image : AnyImage();
        const AnyImage now = layer.asset ? layer.asset->image : AnyImage();
        if (was == now) continue;
        if (!was) {
            // A blank layer painted in some channels only: nothing shows (it has no alpha to carry the colour).
            layer.asset = old->asset;
            layer.transform = old->transform;
            layer.mask = old->mask;
            changed = true;
            continue;
        }
        int dx = 0, dy = 0;
        if (now && !gridOffset(old->transform, was.width(), was.height(), layer.transform, now.width(), now.height(), dx, dy)) continue;
        const bool sameGrid = dx == 0 && dy == 0 && now && now.width() == was.width() && now.height() == was.height();
        const AnyImage kept = keepColorChannels(was, now, dx, dy, channels);
        layer.asset = Asset::makeAny(kept, layer.asset ? layer.asset->name : old->asset->name);
        if (!sameGrid) {
            // Back on the old grid; the mask with it, as the colour edit did not mean to move it.
            layer.transform = old->transform;
            layer.mask = old->mask;
        }
        changed = true;
    }
    return changed;
}

const char* colorChannelName(ColorMode mode, int channel) {
    static const char* rgb[] = {"Red", "Green", "Blue"};
    static const char* cmyk[] = {"Cyan", "Magenta", "Yellow", "Black"};
    static const char* lab[] = {"Lightness", "a", "b"};
    if (channel < 0 || channel >= colorModeColorChannels(mode)) return "";
    return mode == ColorMode::CMYK ? cmyk[channel] : mode == ColorMode::Lab ? lab[channel] : rgb[channel];
}

// ---- The view ---------------------------------------------------------------------------------------------------

namespace {

/// The colour channels of a CMYK or Lab frame (`view.native`, the same size as `out`) as the Channels panel shows them.
void applyModeChannels(Image& out, const ChannelView& view) {
    const AnyImage& native = view.native;
    const int n = native.channels(), alpha = n - 1;
    const unsigned color = view.color & colorChannelsAllFor(view.mode);
    const int single = std::popcount(color) == 1 ? std::countr_zero(color) : -1;
    const bool deep = bool(native.u16());
    const float one = deep ? float(one16) : 255.f;
    auto row = [&](int y) -> const void* {
        if (native.u16()) return native.u16()->row(y);
        if (native.c8()) return native.c8()->row(y);
        return native.u8()->row(y);
    };
    // Straight 0..1 samples of one pixel.
    auto straight = [&](const void* r, int x, float* v) -> float {
        float s[5];
        for (int c = 0; c < n; c++) s[c] = deep ? float(static_cast<const uint16_t*>(r)[x * n + c]) : float(static_cast<const uint8_t*>(r)[x * n + c]);
        const float a = s[alpha];
        for (int c = 0; c < alpha; c++) v[c] = a > 0 ? std::min(1.f, s[c] / a) : (view.mode == ColorMode::CMYK || c == 0 ? 1.f : 0.5f);
        return a / one;
    };
    // Several Lab channels: the colour with the hidden ones neutral, through Lab D50 to sRGB.
    ColorTransformPtr lab;
    if (view.mode == ColorMode::Lab && single < 0) lab = transformBetween(ColorProfile(), srgbProfile(), ConvertOptions(), PixelFormat::LabFloat, PixelFormat::RGBFloat);
    static const float tints[4][3] = {{0, 1, 1}, {1, 0, 1}, {1, 1, 0}, {0, 0, 0}};   // cyan, magenta, yellow, black ink
    const int w = std::min(out.width(), native.width()), h = std::min(out.height(), native.height());
    for (int y = 0; y < h; y++) {
        const void* r = row(y);
        uint8_t* p = out.row(y);
        for (int x = 0; x < w; x++, p += 4) {
            float v[4];
            const float a = straight(r, x, v);
            float rgb[3] = {1, 1, 1};
            if (single >= 0) rgb[0] = rgb[1] = rgb[2] = v[single];
            else if (view.mode == ColorMode::CMYK) {
                for (int c = 0; c < 4; c++) {
                    if (!(color >> c & 1)) continue;
                    const float ink = 1.f - v[c];
                    for (int k = 0; k < 3; k++) rgb[k] *= 1.f - ink * (1.f - tints[c][k]);
                }
            } else if (lab) {
                const float l = color & 1 ? v[0] * 100.f : 50.f;
                const float scaleAB = deep ? 32768.f / 128.f : 255.f;
                const float offset = deep ? 16384.f / 32768.f : 128.f / 255.f;
                const float in[3] = {l, color & 2 ? (v[1] - offset) * scaleAB : 0.f, color & 4 ? (v[2] - offset) * scaleAB : 0.f};
                lab->apply(in, rgb, 1);
            }
            for (int k = 0; k < 3; k++) p[k] = uint8_t(std::lround(std::clamp(rgb[k], 0.f, 1.f) * a * 255.f));
            p[3] = uint8_t(std::lround(a * 255.f));
        }
    }
}

} // namespace

void applyChannelView(Image& out, const Rect& region, double scale, const ChannelView& view) {
    if (view.isDefault() || out.isEmpty() || scale <= 0) return;
    if (view.mode != ColorMode::RGB) {
        // CMYK and Lab: the colour channels come from the frame at the document's layout; the gray and the overlays
        // are as in RGB, below.
        const unsigned all = colorChannelsAllFor(view.mode);
        if (view.color != 0 && (view.color & all) != all && view.native) applyModeChannels(out, view);
        ChannelView rest = view;
        rest.mode = ColorMode::RGB;
        rest.native = AnyImage();
        if (view.color != 0) rest.color = colorChannelsAll;
        if (!rest.isDefault()) applyChannelView(out, region, scale, rest);
        return;
    }
    const int w = out.width(), h = out.height();
    // The document pixel under each output column and row.
    std::vector<int> xs(static_cast<size_t>(w)), ys(static_cast<size_t>(h));
    for (int x = 0; x < w; x++) xs[size_t(x)] = int(std::floor(region.x + (x + 0.5) / scale));
    for (int y = 0; y < h; y++) ys[size_t(y)] = int(std::floor(region.y + (y + 0.5) / scale));
    // A channel's value at a document pixel, 0..1 (black beyond it).
    auto sample = [](const AnyGray& g, int x, int y) -> float {
        if (x < 0 || y < 0 || x >= g.width() || y >= g.height()) return 0.f;
        if (const Gray16Ptr& p = g.u16()) return float(std::min<uint32_t>(p->at(x, y), one16)) / float(one16);
        if (const GrayFPtr& p = g.f32()) return cleanCoverage(p->at(x, y));
        return float(g.u8()->at(x, y)) / 255.f;
    };
    const unsigned color = view.color & colorChannelsAll;
    const int single = std::popcount(color) == 1 ? std::countr_zero(color) : -1;
    for (int y = 0; y < h; y++) {
        uint8_t* p = out.row(y);
        for (int x = 0; x < w; x++, p += 4) {
            if (color == 0) {
                float v = view.gray ? sample(view.gray, xs[size_t(x)], ys[size_t(y)]) : 0.f;
                if (view.grayInverted) v = 1.f - v;
                const uint8_t g = uint8_t(std::lround(v * 255.f));
                p[0] = p[1] = p[2] = g;
                p[3] = 255;
            } else if (single >= 0) {
                p[0] = p[1] = p[2] = p[single];
            } else if (color != colorChannelsAll) {
                for (int c = 0; c < 3; c++) if (!(color >> c & 1)) p[c] = 0;
            }
            for (const ChannelOverlay& o : view.overlays) {
                const float v = sample(o.image, xs[size_t(x)], ys[size_t(y)]);
                const float a = float(std::clamp(o.opacity, 0.0, 1.0)) * (1.f - v);
                if (a <= 0) continue;
                for (int c = 0; c < 3; c++) p[c] = uint8_t(std::lround(p[c] * (1.f - a) + float(std::clamp(o.color[size_t(c)], 0.0, 1.0)) * 255.f * a));
                p[3] = uint8_t(std::lround(p[3] * (1.f - a) + 255.f * a));
            }
        }
    }
}

std::shared_ptr<GrayImage> channelThumbnail(const AnyGray& image, int maxSide) {
    if (const GrayFPtr& g = image.f32()) return makeGrayThumbnail(*g, maxSide);
    if (const Gray16Ptr& g = image.u16()) return makeGrayThumbnail(*g, maxSide);
    if (const GrayPtr& g = image.u8()) return makeGrayThumbnail(*g, maxSide);
    return nullptr;
}

} // namespace compositor
