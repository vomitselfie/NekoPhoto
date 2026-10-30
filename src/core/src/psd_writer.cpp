#include "nekophoto/psd_writer.h"
#include "nekophoto/colormgmt.h"
#include "psd_channels.h"
#include "nekophoto/smartfilter.h"
#include "psd/psd_descriptor.hpp"
#include "nekophoto/adjustments.h"
#include "nekophoto/colormgmt.h"
#include "nekophoto/depth.h"
#include "nekophoto/parallel.h"
#include "nekophoto/render.h"
#include "nekophoto/vectorlayer.h"
#include "nekophoto/vectormask.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <zlib.h>
#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
#include "psd_write.h"

namespace nekophoto {

using namespace psd_write;

namespace {

class Writer {
public:
    Writer(const Document& document, const PsdExportOptions& options, bool encode, PsdExportSummary& summary)
        : doc_(document), options_(options), encode_(encode), deep_(document.sampleType == SampleType::U16), float_(document.sampleType == SampleType::F32),
          curve_(float_ ? encodedTransfer(document) : TransferCurve::srgb()), native_(document.colorMode != ColorMode::RGB),
          colours_(colorModeColorChannels(document.colorMode)), summary_(summary) {
        for (const Layer& l : doc_.layers) byId_[l.id] = &l;
    }

    std::vector<Record> records() {
        emit(std::nullopt);
        // Every record gets a unique 'lyid', keeping Photoshop's own where it is still unique.
        uint32_t next = 1;
        for (const Record& r : records_) next = std::max(next, r.layerId + 1);
        std::set<uint32_t> used;
        for (Record& r : records_) {
            if (r.layerId == 0 || !used.insert(r.layerId).second) { r.layerId = next++; used.insert(r.layerId); }
        }
        return std::move(records_);
    }

private:
    const Document& doc_;
    const PsdExportOptions& options_;
    const bool encode_;
    /// A 16-bit document: 16-bit channels, masks at the same depth.
    const bool deep_;
    /// A 32-bit document: float channels and masks, 8-bit rasters (text, fills) linearised through its curve.
    const bool float_;
    const TransferCurve curve_;
    /// A CMYK or Lab document (P7 step D): its layers' own planes, `colours_` of them (4 for CMYK, 3 for Lab).
    const bool native_;
    const int colours_;
    PsdExportSummary& summary_;
    std::map<Uuid, const Layer*> byId_;
    std::vector<Record> records_;

    std::vector<const Layer*> children(const std::optional<Uuid>& parent) const {
        std::vector<const Layer*> out;
        for (const Layer& l : doc_.layers) if (l.parentId == parent) out.push_back(&l);
        return out;
    }

    void emptyChannels(Record& r, bool alpha = true) const {
        if (!encode_) return;
        for (int id : {-1, 0, 1, 2}) if (id != -1 || alpha) r.channels.push_back({id, encodeChannel({}, 0, 0, false, options_.large)});
        if (native_ && colours_ == 4) r.channels.push_back({3, encodeChannel({}, 0, 0, false, options_.large)});
    }

    void setPixels(Record& r, const Image16& image, int left, int top) const {
        if (native_) { setPixelsMode(r, image, left, top); return; }
        r.left = left; r.top = top; r.right = left + image.width(); r.bottom = top + image.height();
        if (!encode_) return;
        auto planes = straightPlanes16(image);
        r.channels.push_back({-1, encodeChannel16(planes[3], image.width(), image.height(), options_.compress)});
        for (int c = 0; c < 3; c++) r.channels.push_back({c, encodeChannel16(planes[size_t(c)], image.width(), image.height(), options_.compress)});
    }

    void setPixels(Record& r, const ImageF& image, int left, int top) const {
        r.left = left; r.top = top; r.right = left + image.width(); r.bottom = top + image.height();
        if (!encode_) return;
        auto planes = straightPlanesF(image);
        r.channels.push_back({-1, encodeChannel32(planes[3], image.width(), image.height(), options_.compress)});
        for (int c = 0; c < 3; c++) r.channels.push_back({c, encodeChannel32(planes[size_t(c)], image.width(), image.height(), options_.compress)});
    }

    /// A CMYK or Lab layer's pixels at the document's layout: alpha, then each colour plane as stored.
    template <class Img>
    void setPixelsMode(Record& r, const Img& image, int left, int top) const {
        r.left = left; r.top = top; r.right = left + image.width(); r.bottom = top + image.height();
        if (!encode_) return;
        const int n = colours_ + 1;
        const auto planes = straightPlanesMode(image, n, doc_.colorMode == ColorMode::Lab);
        auto encode = [&](const std::vector<uint16_t>& plane) {
            if constexpr (sizeof(*image.data()) == 2) return encodeChannel16(plane, image.width(), image.height(), options_.compress);
            else {
                std::vector<uint8_t> bytes(plane.begin(), plane.end());
                return encodeChannel(bytes, image.width(), image.height(), options_.compress, options_.large);
            }
        };
        r.channels.push_back({-1, encode(planes[size_t(n - 1)])});
        for (int c = 0; c < colours_; c++) r.channels.push_back({c, encode(planes[size_t(c)])});
    }
    void setPixels(Record& r, const ImageC8& image, int left, int top) const {
        if (deep_ && encode_) { setPixelsAny(r, imageAtFormat(ImageC8Ptr(std::make_shared<ImageC8>(image)), SampleType::U16, ColorMode::CMYK), left, top); return; }
        setPixelsMode(r, image, left, top);
    }
    /// Pixels of any layout: a CMYK or Lab document's go through setPixelsMode, converted to its layout and depth first
    /// when they are held in another (an RGB raster placed into it).
    void setPixelsAny(Record& r, const AnyImage& image, int left, int top) const {
        if (!image) return;
        if (!native_) {
            if (image.f32()) setPixels(r, *image.f32(), left, top);
            else if (image.u16()) setPixels(r, *image.u16(), left, top);
            else if (image.u8()) setPixels(r, *image.u8(), left, top);
            return;
        }
        AnyImage at = image.channels() == colours_ + 1 ? image : convertImage(image, ColorMode::RGB, ColorProfile(), doc_.colorMode, doc_.profile);
        at = at ? imageAtFormat(at, doc_.sampleType, doc_.colorMode) : AnyImage();
        if (!at) { r.left = left; r.top = top; r.right = left + image.width(); r.bottom = top + image.height(); return; }
        if (at.u16()) setPixelsMode(r, *at.u16(), left, top);
        else if (at.c8()) setPixelsMode(r, *at.c8(), left, top);
        else if (at.u8()) setPixelsMode(r, *at.u8(), left, top);
    }

    void setPixels(Record& r, const Image& image, int left, int top) const {
        if (native_) { setPixelsAny(r, ImagePtr(std::make_shared<Image>(image)), left, top); return; }
        if (float_ && encode_) { setPixels(r, *lineariseImage(image, curve_), left, top); return; }
        if (deep_ && encode_) { setPixels(r, *widenImage(image), left, top); return; }
        r.left = left; r.top = top; r.right = left + image.width(); r.bottom = top + image.height();
        if (!encode_) return;
        auto planes = straightPlanes(image);
        r.channels.push_back({-1, encodeChannel(planes[3], image.width(), image.height(), options_.compress, options_.large)});
        for (int c = 0; c < 3; c++) r.channels.push_back({c, encodeChannel(planes[size_t(c)], image.width(), image.height(), options_.compress, options_.large)});
    }

    /// The layer's mask over `rect` (document pixels): a copy when it lies on the layer's own pixel grid,
    /// else sampled the way the renderer samples it.
    void setMask(Record& r, const Layer& layer, const Rect& rect, bool onGrid) const {
        if (float_) { setMaskF(r, layer, rect, onGrid); return; }
        if (deep_) { setMask16(r, layer, rect, onGrid); return; }
        if (!layer.mask || !layer.mask->asset.image.u8() || rect.isEmpty()) return;
        const GrayImage& mask = *layer.mask->asset.image.u8();
        r.mask = true;
        r.maskLeft = int(rect.x); r.maskTop = int(rect.y); r.maskRight = int(rect.x + rect.width); r.maskBottom = int(rect.y + rect.height);
        r.maskFlags = layer.mask->enabled ? 0 : 2;
        const int w = int(rect.width), h = int(rect.height);
        std::vector<uint8_t> plane(size_t(w) * h);
        if (onGrid && !layer.mask->placement && mask.width() == w && mask.height() == h) {
            r.maskDefault = 255;
            if (encode_) for (int y = 0; y < h; y++) std::memcpy(plane.data() + size_t(y) * w, mask.row(y), size_t(w));
        } else {
            r.maskDefault = 0;
            if (encode_) {
                GrayImage sampled(w, h, 0);
                sampleMaskCoverage(layer.mask->asset.image.u8(), layer.mask->placement ? *layer.mask->placement : layer.transform, rect, 1.0, 0, sampled, false);
                for (int y = 0; y < h; y++) std::memcpy(plane.data() + size_t(y) * w, sampled.row(y), size_t(w));
            }
        }
        if (encode_) r.channels.push_back({-2, encodeChannel(plane, w, h, options_.compress, options_.large)});
        summary_.masks++;
    }

    /// setMask for a 32-bit document: the mask's own floats.
    void setMaskF(Record& r, const Layer& layer, const Rect& rect, bool onGrid) const {
        if (!layer.mask || !layer.mask->asset.image || rect.isEmpty()) return;
        GrayFPtr mask = grayAtDepth(layer.mask->asset.image, SampleType::F32).f32();
        if (!mask) return;
        r.mask = true;
        r.maskLeft = int(rect.x); r.maskTop = int(rect.y); r.maskRight = int(rect.x + rect.width); r.maskBottom = int(rect.y + rect.height);
        r.maskFlags = layer.mask->enabled ? 0 : 2;
        const int w = int(rect.width), h = int(rect.height);
        const bool copy = onGrid && !layer.mask->placement && mask->width() == w && mask->height() == h;
        r.maskDefault = copy ? 255 : 0;
        if (encode_) {
            GrayF sampled(w, h, 0.0f);
            if (!copy) sampleMaskCoverage(mask, layer.mask->placement ? *layer.mask->placement : layer.transform, rect, 1.0, 0.0f, sampled, false);
            const GrayF& from = copy ? *mask : sampled;
            std::vector<float> plane(from.data(), from.data() + size_t(w) * h);
            r.channels.push_back({-2, encodeChannel32(plane, w, h, options_.compress)});
        }
        summary_.masks++;
    }

    /// setMask for a 16-bit document: the mask's own 16 bits.
    void setMask16(Record& r, const Layer& layer, const Rect& rect, bool onGrid) const {
        if (!layer.mask || !layer.mask->asset.image || rect.isEmpty()) return;
        Gray16Ptr mask = layer.mask->asset.image.u16() ? layer.mask->asset.image.u16() : Gray16Ptr(widenGray(*layer.mask->asset.image.u8()));
        r.mask = true;
        r.maskLeft = int(rect.x); r.maskTop = int(rect.y); r.maskRight = int(rect.x + rect.width); r.maskBottom = int(rect.y + rect.height);
        r.maskFlags = layer.mask->enabled ? 0 : 2;
        const int w = int(rect.width), h = int(rect.height);
        std::vector<uint16_t> plane(size_t(w) * h);
        const bool copy = onGrid && !layer.mask->placement && mask->width() == w && mask->height() == h;
        r.maskDefault = copy ? 255 : 0;
        if (encode_) {
            Gray16 sampled(w, h, 0);
            if (!copy) sampleMaskCoverage(mask, layer.mask->placement ? *layer.mask->placement : layer.transform, rect, 1.0, 0, sampled, false);
            const Gray16& from = copy ? *mask : sampled;
            for (size_t i = 0; i < plane.size(); i++) plane[i] = to65535(from.data()[i]);
            r.channels.push_back({-2, encodeChannel16(plane, w, h, options_.compress)});
        }
        summary_.masks++;
    }

    /// What the layer carries from its PSD, onto the finished record: the blocks still true of it, Blend
    /// If, Fill, its id, and the mask section as stored while the mask is unchanged. `sameContent` is false
    /// when the record's pixels are not the layer's own (written clipped or resampled). `freshText` when a new type
    /// layer is written from the text model, so a dropped 'TySh' is replaced rather than lost.
    void applyCarry(Record& r, const Layer& l, bool sameContent = true, bool freshText = false) const {
        if (!l.psdCarry) return;
        const PsdLayerCarry& c = *l.psdCarry;
        const LayerTransform& t = l.transform;
        const bool placementKept = c.placement.origin == t.origin && c.placement.size == t.size && c.placement.rotation == t.rotation
            && c.placement.flipX == t.flipX && c.placement.flipY == t.flipY;
        const uint64_t contentNow = psdContentHash(l.asset ? l.asset->image : AnyImage());
        const bool contentKept = sameContent && placementKept && c.contentHash == contentNow;
        // A 16-bit layer as read: its channels go back as they were stored, since 0..65535 does not survive the trip
        // through 0..32768 (CarriedPlane); any edit changes the fingerprint and they are written anew. PSD to PSD only
        // (a PSB's RLE rows count in 32 bits).
        // A smart object's carried raster goes back with its own bounds, as long as the instance has not moved.
        const bool smartRaster = c.planesRect && l.smartObject && l.isLiveSmartObject();
        // Carried planes go back only when they hold every colour channel of the record being written (a file in another
        // colour model, such as Grayscale opened as RGB, carries fewer).
        const bool planesCover = [&] {
            for (int id = 0; id < colours_; id++)
                if (std::none_of(c.planes.begin(), c.planes.end(), [&](const PsdLayerCarry::CarriedPlane& p) { return p.id == id; })) return false;
            return true;
        }();
        // An 8-bit RGB smart object carries planes only for its Smart Filters' mask applied on import: Photoshop's
        // unmasked raster goes back, as Photoshop stores it (applySmartFilterMask).
        const bool eightBitSmart = !deep_ && !float_ && !native_ && smartRaster && l.asset && l.asset->image.u8();
        // So does a shape's drawing as Photoshop stored it at 8 bits (vectorlayer.h, shapeShowsDrawnPixels).
        const bool drawn8 = c.drawnShape && !deep_ && !float_ && !native_ && l.asset && l.asset->image.u8();
        if ((deep_ || float_ || native_ || eightBitSmart || drawn8) && !options_.large && (sameContent || smartRaster) && placementKept && planesCover
            && c.planesHash == contentNow && l.asset
            && (eightBitSmart || drawn8 || (float_ ? bool(l.asset->image.f32()) : (l.asset->image.u16() || (native_ && l.asset->image.sampleType() == SampleType::U8))))) {
            r.carriedPixels = true;
            if (smartRaster) { r.left = (*c.planesRect)[0]; r.top = (*c.planesRect)[1]; r.right = (*c.planesRect)[2]; r.bottom = (*c.planesRect)[3]; }
            std::vector<std::pair<int, std::vector<uint8_t>>> channels;
            for (const auto& plane : c.planes) channels.push_back({plane.id, plane.data});
            for (auto& ch : r.channels) if (ch.first < -1) channels.push_back(std::move(ch));
            r.channels = std::move(channels);
        }
        std::set<std::string> dropped;
        for (const PsdBlock& block : c.blocks) {
            const auto binding = PsdLayerCarry::binding(block.key);
            if ((binding == PsdLayerCarry::Binding::Content && !contentKept) || (binding == PsdLayerCarry::Binding::Placement && !placementKept)) { dropped.insert(block.key); continue; }
            if ((r.adjustmentKey && block.key == r.adjustmentKey) || r.replaces.count(block.key)) continue;
            if (!placementKept && (block.key == "vmsk" || block.key == "vsms")) {
                // The path follows the layer to where it now is.
                const int cw = doc_.psdCarry && doc_.psdCarry->width > 0 ? doc_.psdCarry->width : doc_.width;
                const int ch = doc_.psdCarry && doc_.psdCarry->height > 0 ? doc_.psdCarry->height : doc_.height;
                const int w0 = std::max(1, int(std::lround(c.placement.size.width))), h0 = std::max(1, int(std::lround(c.placement.size.height)));
                auto moved = mapVectorMask(block.data, cw, ch, [&](Point p) { return mapLayerPoint(p, c.placement, w0, h0, t, l.pixelWidth(), l.pixelHeight()); });
                if (moved) { r.carried.push_back({block.key, std::move(*moved)}); continue; }
                dropped.insert(block.key);
                continue;
            }
            r.carried.push_back(block);
        }
        auto droppedAny = [&](std::initializer_list<const char*> keys) { for (const char* k : keys) if (dropped.count(k)) return true; return false; };
        if (!freshText && droppedAny({"TySh", "tySh"})) summary_.notes.push_back("Layer \"" + l.name + "\": its pixels changed here, so it is written as pixels, not editable text.");
        if (droppedAny({"SoLd", "SoLE", "PlLd", "plLd"})) summary_.notes.push_back("Layer \"" + l.name + "\": its pixels changed here, so it is written as pixels, not a smart object.");
        if (droppedAny({"GdFl", "PtFl", "SoCo"})) summary_.notes.push_back("Layer \"" + l.name + "\": its pixels changed here, so it is written as pixels, not a fill layer.");
        if (droppedAny({"vmsk", "vsms"})) summary_.warnings.push_back("Layer \"" + l.name + "\": its vector mask could not be moved with it, so it is left out.");
        if (droppedAny({"brit", "blwh", "vibA", "phfl", "mixr", "clrL", "nvrt", "post", "thrs", "selc", "blnc", "CgEd"}))
            summary_.warnings.push_back("Layer \"" + l.name + "\": it was painted on here, so the Photoshop adjustment it held is left out.");
        r.blendingRanges = c.blendingRanges;
        r.layerId = c.layerId;
        if (c.blendAs == int(l.blendMode) && c.blendKey.size() == 4 && (!l.isGroup || (c.blendKey == "pass") == l.passThrough)) r.blend = c.blendKey;
        // Transparency lock (bit 0) always; "pixel data irrelevant" (bit 4) while what makes it so is kept.
        r.flags = uint8_t((c.flags & 0x01) | (contentKept ? c.flags & 0x10 : 0));
        if (r.section == 1 && c.closedFolder) r.section = 2;
        const uint64_t maskHash = l.mask ? psdMaskHash(l.mask->asset.image, l.mask->enabled) : 0;
        // The stored mask channels are PSD's (16-bit row counts) at the file's depth: as they are into a PSD of that depth only.
        if (!options_.large && !c.maskData.empty() && c.maskDepth == (float_ ? 32 : deep_ ? 16 : 8) && placementKept && maskHash == c.maskHash && !dropped.count("vmsk") && !dropped.count("vsms")) {
            r.rawMask = c.maskData;
            r.channels.erase(std::remove_if(r.channels.begin(), r.channels.end(), [](auto& ch) { return ch.first == -2 || ch.first == -3; }), r.channels.end());
            if (encode_) for (auto& ch : c.maskChannels) r.channels.push_back(ch);
        }
    }

    std::set<std::string> placedIds_;

public:
    /// Smart Filter cache records written anew (placed id, record body), for the document's 'FEid' block.
    std::vector<std::pair<std::string, std::vector<uint8_t>>> filterRecords_;

private:

    /// A filtered instance whose pixels here have its Smart Filters' mask applied (applySmartFilterMask) goes out as
    /// Photoshop stores one: the filters' result before the mask, which Photoshop applies itself. Photoshop's own raster
    /// when it went back as read; else the filters drawn here without the mask; a stack not drawn here keeps the masked
    /// pixels, with a note.
    void unmaskFilteredPixels(Record& r, const Layer& l, const std::array<double, 8>& quad) {
        const SmartObjectInstance& so = *l.smartObject;
        if (r.carriedPixels || !doc_.psdCarry || !smartFilterMaskApplies(doc_.psdCarry->globals, so, doc_)) return;
        auto source = doc_.smartObjects.find(so.sourceId);
        std::optional<AnyPlacedRaster> drawn;
        if (!so.locked() && source != doc_.smartObjects.end() && source->second->image)
            drawn = drawSmartObjectRaster(doc_.psdCarry->globals, so, *source->second, quad, SmartObjectTarget(SampleType::U8), SmartObjectDraw::FilteredUnmasked);
        if (!drawn || !drawn->image.u8()) {
            summary_.notes.push_back("Layer \"" + l.name + "\": its Smart Filters are written with their mask applied, so Photoshop applies the mask twice.");
            return;
        }
        // The colour and transparency channels anew; the mask's (-2, -3) stay, after them.
        std::vector<std::pair<int, std::vector<uint8_t>>> masks;
        for (auto& ch : r.channels) if (ch.first < -1) masks.push_back(std::move(ch));
        r.channels.clear();
        setPixels(r, *drawn->image.u8(), drawn->x, drawn->y);
        for (auto& ch : masks) r.channels.push_back(std::move(ch));
    }

    /// A smart object still placed by the layer: its Photoshop blocks, the quad following the layer's transform.
    void applySmartObject(Record& r, const Layer& l, int imageWidth, int imageHeight) {
        if (!l.smartObject) return;
        const SmartObjectInstance& so = *l.smartObject;
        if (!l.isLiveSmartObject()) {
            summary_.notes.push_back("Layer \"" + l.name + "\": its pixels changed here, so it is written as pixels, not a smart object.");
            return;
        }
        const int w = imageWidth, h = imageHeight;
        std::array<double, 8> quad{};
        if (!so.locked() && smartObjectPixelsArePlacement(so)) {
            const double corners[4][2] = {{0, 0}, {double(w), 0}, {double(w), double(h)}, {0, double(h)}};
            for (int i = 0; i < 4; i++) { const Point p = mapThroughTransform(l.transform, w, h, corners[i][0], corners[i][1]); quad[size_t(i * 2)] = p.x; quad[size_t(i * 2 + 1)] = p.y; }
        } else quad = moveQuad(so.quad, so.placedTransform, so.placedWidth, so.placedHeight, l.transform, w, h);
        // Moved: against the placement the blocks hold (a redrawn instance's own quad is already the new one).
        std::array<double, 8> stored = so.quad;
        for (const PsdBlock& b : so.psdBlocks)
            if (auto p = parsePsdPlacement(b.key, b.data)) { stored = p->quad; break; }
        bool moved = false;
        for (size_t i = 0; i < 8; i++) moved |= std::abs(quad[i] - stored[i]) > 1e-4;
        const bool filtered = smartObjectFiltered(so);
        auto source = doc_.smartObjects.find(so.sourceId);
        if (filtered && !so.locked() && source != doc_.smartObjects.end() && source->second->image && (moved || !source->second->psdElement)) {
            // Drawn here, and moved or given new contents: Photoshop's cache for it (the unfiltered pixels over the
            // canvas, and the shared mask) is written anew.
            auto cache = doc_.psdCarry ? findSmartFilterCache(doc_.psdCarry->globals, so.placedId) : std::nullopt;
            // The cache is 8-bit whatever the source's depth, in the document's channels.
            std::optional<std::vector<uint8_t>> record;
            if (cache) {
                uint8_t outside = 255;
                for (const PsdBlock& b : so.psdBlocks)
                    if (auto stack = (b.key == "SoLd" || b.key == "SoLE") ? parseSmartFilterStack(b.key, b.data) : std::nullopt) { outside = stack->maskDefault; break; }
                const PixelRect canvas{0, 0, doc_.width, doc_.height};
                if (doc_.colorMode != ColorMode::RGB) {
                    auto drawn = drawSmartObjectRaster({}, so, *source->second, quad, SmartObjectTarget(SampleType::U8, doc_.colorMode, &doc_.profile),
                                                       SmartObjectDraw::Unfiltered);
                    if (drawn && drawn->image)
                        record = authorSmartFilterRecord(so.placedId, canvas, drawn->image, drawn->x, drawn->y, cache->mask.get(), cache->maskBounds, outside);
                } else {
                    const AnyImage eight = smartObjectSourceImage(*source->second, SampleType::U8);
                    if (auto unfiltered = eight.u8() ? placedSmartObjectRaster(so, *eight.u8(), quad) : std::nullopt)
                        record = authorSmartFilterRecord(so.placedId, canvas, *unfiltered, cache->mask.get(), cache->maskBounds, outside);
                }
            }
            if (!record || !placedIds_.insert(so.placedId).second) {
                summary_.warnings.push_back("Layer \"" + l.name + "\": its Smart Filters' cache could not be rewritten, so it is written as pixels.");
                return;
            }
            filterRecords_.push_back({so.placedId, std::move(*record)});
            for (const PsdBlock& b : so.psdBlocks) {
                auto patched = patchPsdPlacement(b.key, b.data, quad);
                if (patched) r.carried.push_back({b.key, std::move(*patched)});
            }
            unmaskFilteredPixels(r, l, quad);
            summary_.smartObjects++;
            return;
        }
        if (moved && filtered) {
            // Smart Filters keep a document-space cache Photoshop checks against the placement.
            summary_.warnings.push_back("Layer \"" + l.name + "\": a smart object with Smart Filters moved here, so it is written as pixels.");
            return;
        }
        // A duplicate needs its own instance id (Photoshop aliases layers that share one).
        std::string placed;
        if (!so.placedId.empty() && !placedIds_.insert(so.placedId).second) {
            if (filtered) {
                summary_.warnings.push_back("Layer \"" + l.name + "\": a copy of a smart object with Smart Filters is written as pixels.");
                return;
            }
            placed = makeUuid();
            std::transform(placed.begin(), placed.end(), placed.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        }
        for (const PsdBlock& b : so.psdBlocks) {
            if (!moved && placed.empty()) { r.carried.push_back(b); continue; }
            auto patched = patchPsdPlacement(b.key, b.data, quad, placed);
            if (patched) r.carried.push_back({b.key, std::move(*patched)});
        }
        if (filtered) unmaskFilteredPixels(r, l, quad);
        summary_.smartObjects++;
    }

    Record base(const Layer& l) const {
        Record r;
        r.name = l.name;
        r.blend = blendKey(l.blendMode);
        r.opacity = uint8_t(std::clamp(std::lround(l.opacity * 255), 0L, 255L));
        r.fill = uint8_t(std::clamp(std::lround(l.fill * 255), 0L, 255L));
        r.hidden = !l.visible;
        return r;
    }

    /// Whether PSD's clipping (to the nearest unclipped layer below, in the same folder) says what ours does:
    /// every sibling between the base and this layer is clipped to the same base.
    bool clippingFits(const Layer& layer) const {
        if (!layer.maskSourceId) return false;
        auto siblings = children(layer.parentId);
        auto at = std::find(siblings.begin(), siblings.end(), &layer);
        while (at != siblings.begin()) {
            --at;
            const Layer& below = **at;
            if (below.id == *layer.maskSourceId) return !below.isGroup && !below.adjustment;
            if (below.isGroup || below.maskSourceId != layer.maskSourceId) return false;
        }
        return false;
    }

    /// The document rendered with only the given layers showing (and the folders that hold them), at 1:1.
    std::shared_ptr<Image> renderOnly(const std::vector<Uuid>& showing, bool keepClips, std::optional<Uuid> neutralise = std::nullopt, bool folderMasks = true) const {
        Document copy = doc_;
        std::map<Uuid, bool> keep;
        for (const Uuid& id : showing) {
            keep[id] = true;
            for (auto p = byId_.at(id)->parentId; p; p = byId_.at(*p)->parentId) keep[*p] = true;
        }
        for (Layer& l : copy.layers) {
            if (!keep.count(l.id)) l.visible = false;
            if (!keepClips) l.maskSourceId.reset();
            if (neutralise && l.id == *neutralise) { l.opacity = 1; l.fill = 1; l.blendMode = BlendMode::Normal; }
            if (!folderMasks && l.isGroup) l.mask.reset();
        }
        return renderFlattened(copy);
    }

    /// renderOnly at the document's layout (CMYK, Lab).
    AnyImage renderOnlyNative(const std::vector<Uuid>& showing, bool keepClips, std::optional<Uuid> neutralise = std::nullopt, bool folderMasks = true) const {
        Document copy = doc_;
        std::map<Uuid, bool> keep;
        for (const Uuid& id : showing) {
            keep[id] = true;
            for (auto p = byId_.at(id)->parentId; p; p = byId_.at(*p)->parentId) keep[*p] = true;
        }
        for (Layer& l : copy.layers) {
            if (!keep.count(l.id)) l.visible = false;
            if (!keepClips) l.maskSourceId.reset();
            if (neutralise && l.id == *neutralise) { l.opacity = 1; l.fill = 1; l.blendMode = BlendMode::Normal; }
            if (!folderMasks && l.isGroup) l.mask.reset();
        }
        return renderNative(copy);
    }

    /// One layer of a CMYK or Lab document resampled into `bounds` (document pixels): drawn alone at its layout.
    AnyImage layerInto(const Layer& layer, const Rect& bounds) const {
        Document single(doc_.width, doc_.height);
        single.sampleType = doc_.sampleType;
        single.colorMode = doc_.colorMode;
        single.profile = doc_.profile;
        Layer copy = layer;
        copy.parentId.reset(); copy.visible = true; copy.opacity = 1; copy.fill = 1; copy.blendMode = BlendMode::Normal; copy.mask.reset(); copy.maskSourceId.reset();
        copy.psdCarry.reset();
        single.layers = {copy};
        RenderOptions options;
        options.region = bounds;
        return renderNative(single, options);
    }

    /// Every layer drawn before `layer` in the stack, and the layer: what an adjustment applies to.
    std::vector<Uuid> throughLayer(const Layer& layer) const {
        std::vector<Uuid> out;
        for (const Layer* l : renderLayers(doc_.layers)) { out.push_back(l->id); if (l->id == layer.id) break; }
        return out;
    }

    void emit(const std::optional<Uuid>& parent) {
        for (const Layer* layerPtr : children(parent)) {
            const Layer& l = *layerPtr;
            if (l.isGroup) { emitFolder(l); continue; }
            if (l.adjustment) { emitAdjustment(l); continue; }
            emitPixels(l);
        }
    }

    void emitFolder(const Layer& l) {
        Record end;
        end.name = "</Layer group>";
        end.section = 3;
        emptyChannels(end);
        if (l.psdCarry) { end.carried = l.psdCarry->endBlocks; end.blendingRanges = l.psdCarry->endRanges; }
        records_.push_back(std::move(end));
        emit(l.id);
        Record folder = base(l);
        folder.section = 1;
        folder.blend = l.passThrough ? "pass" : blendKey(l.blendMode);
        emptyChannels(folder);
        setMask(folder, l, doc_.rect(), false);
        applyCarry(folder, l);
        // The artboard: the file's own block while it still says what the folder is, else one written anew.
        bool artboardKept = false;
        std::erase_if(folder.carried, [&](const PsdBlock& b) {
            if (b.key != "artb" && b.key != "artd" && b.key != "abdd") return false;
            const bool same = l.artboard && parseArtboardBlock(b.data) == l.artboard;
            artboardKept |= same;
            return !same;
        });
        if (l.artboard && !artboardKept) folder.carried.push_back({"artb", artboardBlock(*l.artboard)});
        records_.push_back(std::move(folder));
        summary_.folders++;
    }

    /// Photoshop's own adjustment block for settings that mean the same there, or none.
    bool nativeAdjustment(const AdjustmentSettings& s, Record& r, const Layer& l) const {
        Out o;
        switch (s.kind) {
        case AdjustmentKind::Levels: {
            o.u16(2);
            for (int i = 0; i < 29; i++) {
                LevelsRange range = i < levelsChannelCount ? s.levels.ranges[size_t(i)] : LevelsRange{};
                o.u16(unsigned(std::lround(std::clamp(range.black, 0.0, 253.0))));
                o.u16(unsigned(std::lround(std::clamp(range.white, 2.0, 255.0))));
                o.u16(unsigned(std::lround(std::clamp(range.outputBlack, 0.0, 255.0))));
                o.u16(unsigned(std::lround(std::clamp(range.outputWhite, 0.0, 255.0))));
                o.u16(unsigned(std::lround(std::clamp(range.gamma, 0.1, 9.99) * 100)));
            }
            r.adjustmentKey = "levl";
            break;
        }
        case AdjustmentKind::Curves: {
            o.u8(0);
            o.u16(1);
            uint32_t mask = 0;
            // The composite and three channels always; CMYK's black when it is set.
            const bool black = !(s.curves.channels[4] == std::vector<CurvePoint>{{0, 0}, {255, 255}});
            const int count = black ? 5 : 4;
            for (int c = 0; c < count; c++) if (s.curves.channels[size_t(c)].size() >= 2) mask |= 1u << c;
            o.u32(mask);
            for (int c = 0; c < count; c++) {
                const auto& points = s.curves.channels[size_t(c)];
                if (points.size() < 2) continue;
                o.u16(unsigned(points.size()));
                for (const CurvePoint& p : points) { o.u16(unsigned(std::lround(std::clamp(p.y, 0.0, 255.0)))); o.u16(unsigned(std::lround(std::clamp(p.x, 0.0, 255.0)))); }
            }
            r.adjustmentKey = "curv";
            break;
        }
        case AdjustmentKind::Exposure:
            o.u16(1);
            o.f32(float(s.exposure.exposure)); o.f32(float(s.exposure.offset)); o.f32(float(s.exposure.gamma));
            o.u16(0);   // Photoshop's block is 16 bytes: two zero bytes after the gamma
            r.adjustmentKey = "expA";
            // Read from a Grayscale file, it was drawn through Grayscale's tone curve; an RGB file has no place for that.
            if (s.exposure.normalized().toneGamma != 2.2)
                summary_.warnings.push_back("Layer \"" + l.name + "\": its Exposure came from a Grayscale file; in this RGB file it looks different (Photoshop draws RGB Exposure through a 2.2 power).");
            break;
        case AdjustmentKind::HueSaturation: {
            // Hue, lightness, colorize and the six ranges (their bands, linear falloffs and all) draw as in
            // Photoshop. Saturation does only on Photoshop's own curve: the plain scale differs once any range
            // moves it. An inverted range has no hue2 counterpart.
            bool scaledSaturation = false;
            if (!s.hsv.colorize) for (const auto& [range, a] : s.hsv.adjustments) if (a.saturation != 0) scaledSaturation = true;
            if ((scaledSaturation && !s.hsv.photoshopSaturation) || s.hsv.invertRange) return false;
            auto value = [&](int range) { auto it = s.hsv.adjustments.find(range); return it == s.hsv.adjustments.end() ? RangeAdjustment{} : it->second; };
            o.u16(2);
            o.u8(s.hsv.colorize ? 1 : 0); o.u8(0);
            const RangeAdjustment master = value(0);
            if (s.hsv.colorize) { o.i16(int(std::lround(master.hue))); o.i16(int(std::lround(master.saturation))); o.i16(int(std::lround(master.lightness))); o.i16(0); o.i16(0); o.i16(0); }
            else { o.i16(0); o.i16(25); o.i16(0); o.i16(int(std::lround(master.hue))); o.i16(int(std::lround(master.saturation))); o.i16(int(std::lround(master.lightness))); }
            for (int range = 1; range <= 6; range++) {
                auto band = s.hsv.bands.find(range);
                const HueBand b = band == s.hsv.bands.end() ? HueBand::defaultBand(range) : band->second;
                for (double v : {b.falloffStart, b.rangeStart, b.rangeEnd, b.falloffEnd}) o.i16(int(std::lround(v)));
                const RangeAdjustment a = s.hsv.colorize ? RangeAdjustment{} : value(range);
                o.i16(int(std::lround(a.hue))); o.i16(int(std::lround(a.saturation))); o.i16(int(std::lround(a.lightness)));
            }
            r.adjustmentKey = "hue2";
            break;
        }
        default:
            return otherAdjustment(s, r, l);
        }
        r.adjustmentData = std::move(o.b);
        return true;
    }

    /// Photoshop's other adjustment layers: the file's own block while the settings are what it said (byte for byte),
    /// otherwise one written anew in Photoshop's layout.
    bool otherAdjustment(const AdjustmentSettings& s, Record& r, const Layer& l) const {
        static const std::map<AdjustmentKind, const char*> keys{{AdjustmentKind::Invert, "nvrt"}, {AdjustmentKind::BrightnessContrast, "brit"},
            {AdjustmentKind::Posterize, "post"}, {AdjustmentKind::Threshold, "thrs"}, {AdjustmentKind::BlackWhite, "blwh"}, {AdjustmentKind::ColorBalance, "blnc"},
            {AdjustmentKind::Vibrance, "vibA"}, {AdjustmentKind::PhotoFilter, "phfl"}, {AdjustmentKind::ChannelMixer, "mixr"}, {AdjustmentKind::SelectiveColor, "selc"},
            {AdjustmentKind::ColorLookup, "clrL"}, {AdjustmentKind::GradientMap, "grdm"}};
        auto key = keys.find(s.kind);
        if (key == keys.end()) return false;
        // Unchanged since the file was read: its carried block goes back as it was. (A Gradient Map, which holds only
        // the ramp's ends and its method here, goes back only so; edited, it is written as pixels.)
        if (l.psdCarry && !l.psdCarry->adjustmentJson.empty() && l.psdCarry->adjustmentJson == s.toJson()) {
            for (const PsdBlock& b : l.psdCarry->blocks) if (b.key == key->second) return true;
        }
        Out o;
        auto descriptor = [&](const patchy::psd::DescriptorObject& d) {
            patchy::psd::BigEndianWriter w;
            w.write_u32(16);
            patchy::psd::write_descriptor(w, d);
            o.bytes(w.bytes());
        };
        using DV = patchy::psd::DescriptorValue;
        auto add = [](patchy::psd::DescriptorObject& d, const char* k, bool longForm, DV v) { d.key_order.push_back({k, longForm}); d.values[k] = std::move(v); };
        auto integer = [](int v) { DV x; x.type = DV::Type::Integer; x.integer_value = v; return x; };
        auto boolean = [](bool v) { DV x; x.type = DV::Type::Bool; x.bool_value = v; return x; };
        auto dbl = [](double v) { DV x; x.type = DV::Type::Double; x.double_value = v; return x; };
        switch (s.kind) {
        case AdjustmentKind::Invert: break;   // no data
        case AdjustmentKind::Posterize: o.u16(unsigned(std::clamp(s.posterize.levels, 2, 255))); o.u16(0); break;
        case AdjustmentKind::Threshold: o.u16(unsigned(std::clamp(s.threshold.level, 1, 255))); o.u16(0); break;
        case AdjustmentKind::ColorBalance:
            for (const auto& range : s.colorBalance.ranges) for (double v : range) o.i16(int(std::lround(std::clamp(v, -100.0, 100.0))));
            o.u8(s.colorBalance.preserveLuminosity ? 1 : 0); o.u8(0);
            break;
        case AdjustmentKind::BrightnessContrast: {
            const BrightnessContrastSettings b = s.brightnessContrast.normalized();
            // Legacy: the values in 'brit'. Modern: an all-zero 'brit' beside Photoshop 2026's 'CgEd' descriptor (Patchy's shape).
            if (b.legacy) { o.i16(b.brightness); o.i16(b.contrast); o.u16(127); o.u8(0); o.u8(0); }
            else {
                o.u16(0); o.u16(0); o.u16(0); o.u8(0); o.u8(0);
                patchy::psd::DescriptorObject d;
                d.class_id = "null";
                add(d, "Vrsn", false, integer(1)); add(d, "Brgh", false, integer(b.brightness)); add(d, "Cntr", false, integer(b.contrast));
                add(d, "means", true, integer(127)); add(d, "Lab ", false, boolean(false)); add(d, "useLegacy", true, boolean(false)); add(d, "Auto", false, boolean(false));
                patchy::psd::BigEndianWriter w;
                w.write_u32(16);
                patchy::psd::write_descriptor(w, d);
                r.carried.push_back({"CgEd", w.bytes()});
            }
            r.replaces.insert("CgEd");
            break;
        }
        case AdjustmentKind::ChannelMixer: {
            o.u16(1); o.u16(s.channelMixer.monochrome ? 1 : 0);
            // Four output records of red, green, blue, (CMYK's fourth), constant; monochrome's grey in the first.
            for (int out = 0; out < 4; out++) {
                const auto& row = s.channelMixer.monochrome ? (out == 0 ? s.channelMixer.rows[3] : std::array<double, 4>{}) : (out < 3 ? s.channelMixer.rows[size_t(out)] : std::array<double, 4>{});
                for (int k = 0; k < 3; k++) o.i16(int(std::lround(std::clamp(row[size_t(k)], -200.0, 200.0))));
                o.i16(0);
                o.i16(int(std::lround(std::clamp(row[3], -200.0, 200.0))));
            }
            break;
        }
        case AdjustmentKind::SelectiveColor:
            o.u16(1); o.u16(s.selectiveColor.absolute ? 1 : 0);
            for (int k = 0; k < 4; k++) o.i16(0);   // the reserved first record
            for (const auto& range : s.selectiveColor.ranges) for (double v : range) o.i16(int(std::lround(std::clamp(v, -100.0, 100.0))));
            break;
        case AdjustmentKind::PhotoFilter: {
            // Version 2 with an RGB colour (version 3's XYZ scale is not documented).
            const AdjustmentColor c = s.photoFilter.color.clamped();
            o.u16(2); o.u16(0);
            for (double v : {c.red, c.green, c.blue}) o.u16(unsigned(std::lround(v * 65535)));
            o.u16(0);
            o.u32(uint32_t(std::lround(std::clamp(s.photoFilter.density, 0.0, 100.0))));
            o.u8(s.photoFilter.preserveLuminosity ? 1 : 0);
            break;
        }
        case AdjustmentKind::Vibrance: {
            patchy::psd::DescriptorObject d;
            d.class_id = "null";
            add(d, "vibrance", true, integer(int(std::lround(s.vibrance.vibrance))));
            add(d, "Strt", false, integer(int(std::lround(s.vibrance.saturation))));
            descriptor(d);
            break;
        }
        case AdjustmentKind::BlackWhite: {
            patchy::psd::DescriptorObject d;
            d.class_id = "null";
            const char* names[6] = {"Rd  ", "Yllw", "Grn ", "Cyn ", "Bl  ", "Mgnt"};
            for (size_t i = 0; i < 6; i++) add(d, names[i], false, integer(int(std::lround(s.blackWhite.weights[i]))));
            add(d, "useTint", true, boolean(s.blackWhite.tint));
            DV colour; colour.type = DV::Type::Object; colour.object_value = std::make_shared<patchy::psd::DescriptorObject>();
            colour.object_value->class_id = "RGBC";
            const AdjustmentColor t = s.blackWhite.tintColor.clamped();
            add(*colour.object_value, "Rd  ", false, dbl(t.red * 255)); add(*colour.object_value, "Grn ", false, dbl(t.green * 255)); add(*colour.object_value, "Bl  ", false, dbl(t.blue * 255));
            add(d, "tintColor", true, colour);
            add(d, "bwPresetKind", true, integer(1));
            descriptor(d);
            break;
        }
        case AdjustmentKind::ColorLookup: {
            // Photoshop's descriptor with the LUT file embedded (or the profile), after a version.
            const ColorLookupSettings& c = s.colorLookup;
            if (c.format.empty() || !colorLookupReadable(c)) return false;
            o.u16(1);
            patchy::psd::DescriptorObject d;
            d.class_id = "null";
            auto text = [](const std::string& v) { DV x; x.type = DV::Type::String; x.string_value = v; return x; };
            auto raw = [](std::vector<uint8_t> v) { DV x; x.type = DV::Type::Raw; x.raw_value = std::move(v); return x; };
            auto enumeration = [](const char* type, const char* value) {
                DV x; x.type = DV::Type::Enum; x.enum_type = type; x.enum_type_long_form = true; x.enum_value = value; x.enum_value_long_form = true; return x;
            };
            add(d, "Vrsn", false, integer(1));
            const bool icc = c.format == "icc";
            add(d, "lookupType", true, enumeration("colorLookupType", icc ? "abstractProfile" : "3DLUT"));
            add(d, "Nm  ", false, text(c.name));
            add(d, "Dthr", false, boolean(c.dither));
            if (icc) {
                auto bytes = fromBase64(c.data);
                if (!bytes) return false;
                add(d, "profile", true, raw(*bytes));
            } else {
                add(d, "LUTFormat", true, enumeration("LUTFormatType", c.format == "3dl" ? "LUTFormat3DL" : "LUTFormatCUBE"));
                add(d, "dataOrder", true, enumeration("colorDataOrder", "rgbOrder"));
                add(d, "tableOrder", true, enumeration("colorTableOrder", "bgrOrder"));
                add(d, "LUT3DFileData", true, raw(std::vector<uint8_t>(c.data.begin(), c.data.end())));
                add(d, "LUT3DFileName", true, text(c.name));
            }
            descriptor(d);
            break;
        }
        default: return false;
        }
        r.adjustmentKey = key->second;
        r.adjustmentData = std::move(o.b);
        return true;
    }

    void emitAdjustment(const Layer& l) {
        AdjustmentSettings settings;
        const bool parsed = AdjustmentSettings::parse(l.adjustment->json, settings);
        Record r = base(l);
        if (parsed && nativeAdjustment(settings, r, l)) {
            r.clipping = l.maskSourceId && clippingFits(l);
            emptyChannels(r);
            setMask(r, l, doc_.rect(), false);
            summary_.adjustments++;
            if (r.clipping) summary_.clipped++;
            applyCarry(r, l);
            records_.push_back(std::move(r));
            return;
        }
        // No Photoshop equivalent: the adjusted look of everything beneath, as a pixel layer in its place.
        summary_.warnings.push_back("Adjustment \"" + l.name + "\" (" + adjustmentKindName(l.adjustment->kind) + ") has no Photoshop equivalent; its result is written as a pixel layer that covers the layers beneath it (they stay in the file).");
        Record baked;
        baked.name = l.name;
        baked.hidden = !l.visible;
        if (!l.visible) emptyChannels(baked);   // it showed nothing: an empty hidden layer keeps its place
        else if (encode_ && native_) setPixelsAny(baked, renderOnlyNative(throughLayer(l), true), 0, 0);
        else if (encode_) setPixels(baked, *renderOnly(throughLayer(l), true), 0, 0);
        else { baked.right = doc_.width; baked.bottom = doc_.height; }
        records_.push_back(std::move(baked));
        summary_.layers++;
    }

    void emitPixels(const Layer& l) {
        Record r = base(l);
        const bool hasPixels = l.asset && l.asset->image && l.asset->image.width() > 0 && l.asset->image.height() > 0;
        std::optional<PsdTextMetrics> textMetrics;
        if (l.isLiveText() && options_.textMetrics) textMetrics = options_.textMetrics(*l.text);
        if (l.isLiveText() && !textMetrics) summary_.notes.push_back("Text \"" + l.name + "\" is written as pixels; it stays editable text in the NekoPhoto project.");
        else if (l.isLiveShape()) summary_.notes.push_back("Shape \"" + l.name + "\" is written as pixels; it stays an editable shape in the NekoPhoto project.");
        const bool clipped = l.maskSourceId.has_value();
        const bool clipFits = clipped && clippingFits(l);
        if (!hasPixels) {
            emptyChannels(r);
            setMask(r, l, doc_.rect(), false);
            r.clipping = clipFits;
            summary_.layers++;
            if (r.clipping) summary_.clipped++;
            applyCarry(r, l);
            records_.push_back(std::move(r));
            return;
        }
        const int imageWidth = l.asset->image.width(), imageHeight = l.asset->image.height();
        const LayerTransform& t = l.transform;
        const bool onGrid = t.rotation == 0 && !t.flipX && !t.flipY && t.origin.x == std::round(t.origin.x) && t.origin.y == std::round(t.origin.y)
            && t.size.width == imageWidth && t.size.height == imageHeight;
        if (clipped && !clipFits && native_) {
            // A CMYK or Lab layer clipped to one not right beneath it: written as it shows, the clip's alpha applied.
            summary_.warnings.push_back("Layer \"" + l.name + "\" is clipped to a layer that is not right beneath it, which PSD cannot say; it is written as it shows, unclipped.");
            const Rect bounds = t.bounds().integral().intersection(doc_.rect());
            r.clipping = false;
            if (encode_ && !bounds.isEmpty()) {
                const AnyImage own = renderOnlyNative({l.id}, false, l.id, false), source = renderOnlyNative({*l.maskSourceId}, true, std::nullopt, false);
                const int n = colours_ + 1, bx = int(bounds.x), by = int(bounds.y), bw = int(bounds.width), bh = int(bounds.height);
                auto clip = [&](const auto& o, const auto& c, auto out) {
                    for (int y = 0; y < bh; y++) {
                        const auto* s = o.pixel(bx, by + y);
                        const auto* a = c.pixel(bx, by + y);
                        auto* d = out->row(y);
                        const uint32_t one = sizeof(*d) == 2 ? one16 : 255u;
                        for (int x = 0; x < bw; x++, s += n, a += n, d += n)
                            for (int k = 0; k < n; k++) d[k] = static_cast<std::remove_cvref_t<decltype(d[k])>>((uint32_t(s[k]) * a[n - 1] + one / 2) / one);
                    }
                    setPixelsAny(r, AnyImage(std::shared_ptr<const std::remove_cvref_t<decltype(*out)>>(out)), bx, by);
                };
                if (own.u16()) clip(*own.u16(), *source.u16(), std::make_shared<Image16>(bw, bh, n));
                else if (own.c8()) clip(*own.c8(), *source.c8(), std::make_shared<ImageC8>(bw, bh, n));
                else if (own.u8()) clip(*own.u8(), *source.u8(), std::make_shared<Image>(bw, bh));
            } else { r.left = int(bounds.x); r.top = int(bounds.y); r.right = int(bounds.x + bounds.width); r.bottom = int(bounds.y + bounds.height); }
            applyCarry(r, l, false);
            records_.push_back(std::move(r));
            summary_.layers++;
            return;
        }
        if (clipped && !clipFits) {
            // PSD clips only to the layer right beneath: this one is written as it shows, clip and mask applied.
            summary_.warnings.push_back("Layer \"" + l.name + "\" is clipped to a layer that is not right beneath it, which PSD cannot say; it is written as it shows, unclipped.");
            const Rect bounds = t.bounds().integral().intersection(doc_.rect());
            r.clipping = false;
            if (encode_ && !bounds.isEmpty()) {
                // Without the folders' masks: the layer stays in its folder, which applies them in Photoshop.
                auto own = renderOnly({l.id}, false, l.id, false);
                auto source = renderOnly({*l.maskSourceId}, true, std::nullopt, false);
                Image out(int(bounds.width), int(bounds.height));
                for (int y = 0; y < out.height(); y++) {
                    const uint8_t* s = own->pixel(int(bounds.x), int(bounds.y) + y);
                    const uint8_t* c = source->pixel(int(bounds.x), int(bounds.y) + y);
                    uint8_t* d = out.row(y);
                    for (int x = 0; x < out.width(); x++, s += 4, c += 4, d += 4)
                        for (int k = 0; k < 4; k++) d[k] = uint8_t((s[k] * c[3] + 127) / 255);
                }
                setPixels(r, out, int(bounds.x), int(bounds.y));
            } else { r.left = int(bounds.x); r.top = int(bounds.y); r.right = int(bounds.x + bounds.width); r.bottom = int(bounds.y + bounds.height); }
            applyCarry(r, l, false);
            records_.push_back(std::move(r));
            summary_.layers++;
            return;
        }
        r.clipping = clipFits;
        if (onGrid) {
            // A shape drawn here is its fill, which the path cuts: written as Photoshop writes a shape, its drawing (the
            // fill cut and antialiased, the stroke over it), which readers show and which opens here as drawn pixels.
            AnyImage pixels = l.asset->image;
            if (encode_ && isVectorShapeLayer(l) && !shapeShowsDrawnPixels(l) && !softVectorMask(l.psdCarry->maskData))
                if (AnyImage drawn = shapeDrawing(doc_, l); drawn && drawn.width() == imageWidth && drawn.height() == imageHeight) pixels = drawn;
            if (native_) setPixelsAny(r, pixels, int(t.origin.x), int(t.origin.y));
            else if (pixels.u16()) setPixels(r, *pixels.u16(), int(t.origin.x), int(t.origin.y));
            else if (pixels.f32()) setPixels(r, *pixels.f32(), int(t.origin.x), int(t.origin.y));
            else setPixels(r, *pixels.u8(), int(t.origin.x), int(t.origin.y));
            setMask(r, l, Rect(t.origin.x, t.origin.y, imageWidth, imageHeight), true);
        } else {
            // Live text is redrawn by Photoshop from its type data, so resampling its preview pixels is not worth a note.
            if (!textMetrics) summary_.notes.push_back("Layer \"" + l.name + "\" is scaled, rotated or flipped; it is written resampled into place.");
            const Rect bounds = t.bounds().integral();
            if (encode_ && !bounds.isEmpty()) {
                LayerTransform target(Point(bounds.x, bounds.y), Size(bounds.width, bounds.height));
                if (native_) setPixelsAny(r, layerInto(l, bounds), int(bounds.x), int(bounds.y));
                else if (l.asset->image.u16()) setPixels(r, *resampleLayer(l.asset->image.u16(), t, target, int(bounds.width), int(bounds.height)), int(bounds.x), int(bounds.y));
                else if (l.asset->image.f32()) setPixels(r, *resampleLayer(l.asset->image.f32(), t, target, int(bounds.width), int(bounds.height)), int(bounds.x), int(bounds.y));
                else setPixels(r, *resampleLayer(l.asset->image.u8(), t, target, int(bounds.width), int(bounds.height)), int(bounds.x), int(bounds.y));
            } else { r.left = int(bounds.x); r.top = int(bounds.y); r.right = int(bounds.x + bounds.width); r.bottom = int(bounds.y + bounds.height); }
            setMask(r, l, bounds, false);
        }
        summary_.layers++;
        if (r.clipping) summary_.clipped++;
        applyCarry(r, l, onGrid, textMetrics.has_value());
        applySmartObject(r, l, imageWidth, imageHeight);
        // Photoshop's own type layer, still true of the pixels, says more than ours can (several styles, warps).
        if (textMetrics && std::any_of(r.carried.begin(), r.carried.end(), [](const PsdBlock& b) { return b.key == "TySh"; })) { textMetrics.reset(); summary_.texts++; }
        if (textMetrics) {
            const Rect bounds(r.left, r.top, r.right - r.left, r.bottom - r.top);
            if (auto block = photoshopTypeBlock(*l.text, *textMetrics, t, imageWidth, imageHeight, bounds)) {
                r.carried.push_back({"TySh", std::move(*block)});
                summary_.texts++;
            } else summary_.notes.push_back("Text \"" + l.name + "\" is flipped, which Photoshop text cannot be; it is written as pixels.");
        }
        records_.push_back(std::move(r));
    }
};

} // namespace

PsdExportSummary planPsdExport(const Document& document, const PsdExportOptions& options) {
    PsdExportSummary summary;
    Writer(document, options, false, summary).records();
    return summary;
}

std::vector<uint8_t> encodePsd(const Document& document, const PsdExportOptions& options, PsdExportSummary* summaryOut, std::string* error) {
    const bool large = options.large;
    const int maxSide = large ? psbMaxSide : psdMaxSide;
    if (document.width > maxSide || document.height > maxSide || document.width < 1 || document.height < 1) {
        if (error) *error = "This document is larger than PSD allows (" + std::to_string(psdMaxSide) + " pixels a side); export it as PSB.";
        return {};
    }
    PsdExportSummary summary;
    Writer writer(document, options, true, summary);
    std::vector<Record> records = writer.records();

    Out f;
    f.str("8BPS"); f.u16(large ? 2 : 1); for (int i = 0; i < 6; i++) f.u8(0);
    // The colour channels (3, or 4 for CMYK), the merged image's transparency, then the alpha and spot channels.
    f.u16(unsigned(colorModeColorChannels(document.colorMode)) + 1 + unsigned(document.channels.size()));
    f.u32(uint32_t(document.height)); f.u32(uint32_t(document.width));
    const bool deep = document.sampleType == SampleType::U16;
    // A 32-bit document: float channels in 'Lr32', the merged image in float, resource 1039 the profile its values
    // encode to (Photoshop's 32-bit files carry the working space's own profile; the values are linear in it).
    const bool floating = document.sampleType == SampleType::F32;
    const ColorProfile fileProfile = floating ? encodedProfileOf(document) : document.profile;
    // The colour mode: RGB 3, CMYK 4, Lab 9 (32 bits is RGB only).
    f.u16(floating ? 32 : deep ? 16 : 8); f.u16(document.colorMode == ColorMode::CMYK ? 4 : document.colorMode == ColorMode::Lab ? 9 : 3);
    f.u32(0);   // colour mode data
    {
        // The resolution (ResolutionInfo, 0x03ED): pixels per inch as 16.16 fixed point, both axes.
        Out res;
        const uint32_t ppi = uint32_t(std::lround(std::clamp(document.resolution, 1.0, 30000.0) * 65536));
        res.str("8BIM"); res.u16(0x03ED); res.u8(0); res.u8(0); res.u32(16);
        res.u32(ppi); res.u16(1); res.u16(1); res.u32(ppi); res.u16(1); res.u16(1);
        // The PSD's own resources, when the document came from one.
        bool slicesWritten = false, guidesWritten = false, profileWritten = false;
        std::optional<std::pair<uint32_t, uint32_t>> grid;   // the file's grid words, kept when the guides change
        auto writeProfile = [&](const std::string& name) {
            res.str("8BIM"); res.u16(1039);
            res.u8(unsigned(name.size())); res.bytes(std::vector<uint8_t>(name.begin(), name.end()));
            if ((name.size() + 1) & 1) res.u8(0);
            res.u32(uint32_t(fileProfile.icc.size())); res.bytes(fileProfile.icc);
            if (fileProfile.icc.size() & 1) res.u8(0);
            profileWritten = true;
        };
        if (document.psdCarry) {
            const PsdDocumentCarry& c = *document.psdCarry;
            const bool sameCanvas = c.width == document.width && c.height == document.height;
            for (const auto& resource : c.resources) {
                // Guides, slices and paths (stored relative to the canvas size) belong to the canvas they were made on.
                const bool canvasBound = resource.id == 1032 || resource.id == 1050 || resource.id == 1025 || (resource.id >= 2000 && resource.id <= 2999);
                if (!sameCanvas && canvasBound) continue;
                // The colour profile (1039) is the document's: the file's own bytes while they are still it, the
                // document's in their place after Assign or Convert, none for an untagged document. The untagged flag
                // (1041) goes with a profile written.
                if (resource.id == 1039) {
                    if (fileProfile.empty()) continue;
                    if (resource.data != fileProfile.icc) { writeProfile(resource.name.substr(0, 255)); continue; }
                    profileWritten = true;
                }
                if (resource.id == 1041 && !fileProfile.empty() && !resource.data.empty() && resource.data[0] != 0) continue;
                if (resource.id == 1032) {
                    // The file's guides (and grid) while they are still the document's; else written anew below.
                    std::vector<Guide> held;
                    // Bytes that are not a guides resource go back untouched while the document has no guides of its own.
                    const bool parsed = parseGuidesResource(resource.data, held, &grid);
                    if (parsed ? held != document.guides : !document.guides.empty()) continue;
                    guidesWritten = true;
                }
                if (resource.id == 1050) {
                    // The file's slices while they are still the document's; else written anew below.
                    std::vector<Slice> held;
                    if (!parseSlicesResource(resource.data, held) || held != document.slices) continue;
                    slicesWritten = true;
                }
                res.str("8BIM"); res.u16(resource.id);
                const std::string name = resource.name.substr(0, 255);
                res.u8(unsigned(name.size())); res.bytes(std::vector<uint8_t>(name.begin(), name.end()));
                if ((name.size() + 1) & 1) res.u8(0);
                res.u32(uint32_t(resource.data.size())); res.bytes(resource.data);
                if (resource.data.size() & 1) res.u8(0);
            }
        }
        if (!profileWritten && !fileProfile.empty()) writeProfile("");
        // The alpha and spot channels' names, display and identifiers (psd_channels.h).
        for (const auto& [id, data] : psdChannelResourceBlocks(document)) {
            res.str("8BIM"); res.u16(id); res.u8(0); res.u8(0);
            res.u32(uint32_t(data.size())); res.bytes(data);
            if (data.size() & 1) res.u8(0);
        }
        if (!guidesWritten && !document.guides.empty()) {
            const std::vector<uint8_t> data = guidesResource(document.guides, grid);
            res.str("8BIM"); res.u16(1032); res.u8(0); res.u8(0);
            res.u32(uint32_t(data.size())); res.bytes(data);
            if (data.size() & 1) res.u8(0);
        }
        if (!slicesWritten && !document.slices.empty()) {
            const std::vector<uint8_t> data = slicesResource(document.slices, document.width, document.height);
            res.str("8BIM"); res.u16(1050); res.u8(0); res.u8(0);
            res.u32(uint32_t(data.size())); res.bytes(data);
            if (data.size() & 1) res.u8(0);
        }
        f.u32(uint32_t(res.b.size())); f.bytes(res.b);
    }
    {
        Out info;
        // Negative: the merged image's first alpha channel is its transparency.
        info.i16(-int(records.size()));
        for (const Record& r : records) writeRecord(info, r, large);
        for (const Record& r : records) for (auto& [id, data] : r.channels) info.bytes(data);
        if (info.b.size() & 1) info.u8(0);
        Out section;
        if (deep || floating) {
            // A 16-bit file keeps its layers in the 'Lr16' block after an empty layer information, as Photoshop writes it;
            // a 32-bit one in 'Lr32'.
            section.length(0, large);
            section.u32(0);   // global layer mask info
            section.str("8BIM"); section.str(floating ? "Lr32" : "Lr16");
            section.length(info.b.size(), large);
            section.bytes(info.b);
            for (size_t n = info.b.size(); n % 4; n++) section.u8(0);
        } else {
            section.length(info.b.size(), large);
            section.bytes(info.b);
            section.u32(0);   // global layer mask info
        }
        // Global blocks from the PSD the document came from (linked smart object data, patterns, text
        // engine data), each padded to four bytes outside its declared length, as Photoshop reads them.
        // Smart object sources: while every embedded one is as it was read, the file's own 'lnk2' goes back as it
        // was; otherwise 'lnk2' is rebuilt, unchanged elements byte for byte and new or edited ones written anew.
        bool rebuildLinks = false;
        for (auto& [id, source] : document.smartObjects)
            rebuildLinks |= source->kind == SmartObjectSource::Kind::Embedded && !source->psdElement;
        std::vector<uint8_t> links;
        if (rebuildLinks)
            for (auto& [id, source] : document.smartObjects) {
                if (!source->psdBlock.empty() && source->psdBlock != "lnk2") continue;   // stays in its own block
                if (source->psdElement) links.insert(links.end(), source->psdElement->begin(), source->psdElement->end());
                else if (source->kind == SmartObjectSource::Kind::Embedded) { auto e = psdEmbeddedElement(*source); links.insert(links.end(), e.begin(), e.end()); }
            }
        if (document.psdCarry) for (const PsdBlock& stored : document.psdCarry->globals) {
            if (rebuildLinks && stored.key == "lnk2") continue;
            // Smart Filter caches are 8-bit pixels: a 16-bit file leaves them for Photoshop to rebuild, as the reader does.
            if ((deep || floating) && (stored.key == "FEid" || stored.key == "FXid")) continue;
            PsdBlock block = stored;
            if (!writer.filterRecords_.empty() && (block.key == "FEid" || block.key == "FXid"))
                if (auto replaced = replaceSmartFilterRecords(block.data, writer.filterRecords_)) block.data = std::move(*replaced);
            section.str("8BIM"); section.str(block.key.c_str());
            section.length(block.data.size(), large && longKey(block.key)); section.bytes(block.data);
            for (size_t n = block.data.size(); n % 4; n++) section.u8(0);
        }
        if (!links.empty()) {
            section.str("8BIM"); section.str("lnk2");
            section.length(links.size(), large); section.bytes(links);
            for (size_t n = links.size(); n % 4; n++) section.u8(0);
        }
        if (!large && section.b.size() > 0xFFFFFFFFull) { if (error) *error = "The layers are too large for a PSD file; export it as PSB."; return {}; }
        f.length(section.b.size(), large);
        f.bytes(section.b);
    }
    {
        // The merged image from our own renderer, the look the layers should have. Its colour is matted
        // against white where it is transparent, as Photoshop stores it: premultiplied plus the white behind.
        // The alpha and spot channels follow, one plane each at the document's depth.
        const size_t extra = document.channels.size();
        if (floating) {
            // In float, raw (big-endian), colour matted against white where it is transparent as at 8 and 16 bits;
            // the alpha and spot channels as float coverage.
            auto flat = renderFlattenedF(document);
            const int w = document.width, h = document.height;
            const size_t n = size_t(w) * size_t(h);
            std::vector<std::vector<uint8_t>> raw(4 + extra, std::vector<uint8_t>(n * 4));
            auto put = [](std::vector<uint8_t>& plane, size_t i, float v) {
                uint32_t bits;
                std::memcpy(&bits, &v, 4);
                for (int b = 0; b < 4; b++) plane[i * 4 + size_t(b)] = uint8_t(bits >> (24 - 8 * b));
            };
            parallelRows(0, h, [&](int ya, int yb) {
                for (int y = ya; y < yb; y++) {
                    const float* s = flat->row(y);
                    for (int x = 0; x < w; x++, s += 4) {
                        const size_t i = size_t(y) * size_t(w) + size_t(x);
                        const float a = cleanCoverage(s[3]);
                        for (int c = 0; c < 3; c++) put(raw[size_t(c)], i, cleanColour(s[c]) + (1 - a));
                        put(raw[3], i, a);
                    }
                }
            }, 64);
            for (size_t k = 0; k < extra; k++) {
                const GrayFPtr gray = grayAtDepth(document.channels[k].image, SampleType::F32).f32();
                if (gray && gray->width() == w && gray->height() == h)
                    for (size_t i = 0; i < n; i++) put(raw[4 + k], i, cleanCoverage(gray->data()[i]));
            }
            f.u16(0);
            for (auto& p : raw) f.bytes(p);
            if (summaryOut) *summaryOut = summary;
            return std::move(f.b);
        }
        if (document.colorMode != ColorMode::RGB) {
            writeMergedMode(f, document, options);
            if (summaryOut) *summaryOut = summary;
            return std::move(f.b);
        }
        if (deep) {
            auto flat = renderFlattened16(document);
            const int w = document.width, h = document.height;
            std::vector<std::vector<uint8_t>> rows(size_t(h) * (4 + extra));
            std::vector<std::vector<uint8_t>> raw(4);
            for (auto& p : raw) p.resize(size_t(w) * h * 2);
            for (const Channel& channel : document.channels) raw.push_back(psdChannelPlane(channel, true, w, h));
            parallelRows(0, h, [&](int ya, int yb) {
                for (int y = ya; y < yb; y++) {
                    const uint16_t* s = flat->row(y);
                    for (int x = 0; x < w; x++, s += 4) {
                        const size_t i = (size_t(y) * w + x) * 2;
                        for (int c = 0; c < 4; c++) {
                            const uint16_t v = to65535(c == 3 ? s[3] : std::min<uint32_t>(one16, uint32_t(s[c]) + one16 - s[3]));
                            raw[size_t(c)][i] = uint8_t(v >> 8); raw[size_t(c)][i + 1] = uint8_t(v);
                        }
                    }
                }
            }, 64);
            if (options.compress) {
                parallelRows(0, h, [&](int ya, int yb) {
                    for (size_t c = 0; c < raw.size(); c++) for (int y = ya; y < yb; y++) {
                        const uint8_t* row = raw[size_t(c)].data() + size_t(y) * w * 2;
                        packBits(row, w * 2, rows[size_t(c) * h + size_t(y)]);
                        makeRowEven(rows[size_t(c) * h + size_t(y)], row, w * 2);
                    }
                }, 64);
                f.u16(1);
                for (auto& r : rows) { if (large) f.u32(uint32_t(r.size())); else f.u16(unsigned(r.size())); }
                for (auto& r : rows) f.bytes(r);
            } else {
                f.u16(0);
                for (auto& p : raw) f.bytes(p);
            }
            if (summaryOut) *summaryOut = summary;
            return std::move(f.b);
        }
        auto flat = renderFlattened(document);
        std::array<std::vector<uint8_t>, 4> planes;
        for (auto& p : planes) p.resize(size_t(document.width) * document.height);
        parallelRows(0, document.height, [&](int ya, int yb) {
            for (int y = ya; y < yb; y++) {
                const uint8_t* s = flat->row(y);
                for (int x = 0; x < document.width; x++, s += 4) {
                    const size_t i = size_t(y) * document.width + x;
                    planes[3][i] = s[3];
                    for (int c = 0; c < 3; c++) planes[size_t(c)][i] = uint8_t(std::min(255, int(s[c]) + 255 - int(s[3])));
                }
            }
        }, 64);
        const int w = document.width, h = document.height;
        std::vector<std::vector<uint8_t>> rows;
        rows.reserve(size_t(h) * 4);
        const int order[4] = {0, 1, 2, 3};
        bool rle = options.compress;
        std::vector<std::vector<uint8_t>> channelPlanes;
        for (const Channel& channel : document.channels) channelPlanes.push_back(psdChannelPlane(channel, false, w, h));
        if (rle) {
            rows.resize(size_t(h) * (4 + extra));
            parallelRows(0, h, [&](int ya, int yb) {
                for (int c : order) for (int y = ya; y < yb; y++) {
                    const uint8_t* raw = planes[size_t(c)].data() + size_t(y) * w;
                    packBits(raw, w, rows[size_t(c) * h + size_t(y)]);
                    makeRowEven(rows[size_t(c) * h + size_t(y)], raw, w);
                }
                for (size_t k = 0; k < extra; k++) for (int y = ya; y < yb; y++) {
                    const uint8_t* raw = channelPlanes[k].data() + size_t(y) * w;
                    std::vector<uint8_t>& row = rows[(4 + k) * size_t(h) + size_t(y)];
                    packBits(raw, w, row);
                    makeRowEven(row, raw, w);
                }
            }, 64);
        }
        if (rle) {
            f.u16(1);
            for (auto& r : rows) { if (large) f.u32(uint32_t(r.size())); else f.u16(unsigned(r.size())); }
            for (auto& r : rows) f.bytes(r);
        } else {
            f.u16(0);
            for (int c : order) f.bytes(planes[size_t(c)]);
            for (const auto& p : channelPlanes) f.bytes(p);
        }
    }
    if (summaryOut) *summaryOut = summary;
    return std::move(f.b);
}

namespace detail {
bool (*psdSaveFault)(const char* step) = nullptr;
} // namespace detail

namespace {

bool injectedFault(const char* step) { return detail::psdSaveFault && detail::psdSaveFault(step); }

/// Writes `bytes` to `path` and flushes them to the disk.
bool writeDurably(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    if (injectedFault("open")) return false;
#ifdef _WIN32
    FILE* file = _wfopen(path.c_str(), L"wb");
#else
    FILE* file = std::fopen(path.c_str(), "wb");
#endif
    if (!file) return false;
    bool ok = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size() && !injectedFault("write") && std::fflush(file) == 0;
#ifdef _WIN32
    ok = ok && _commit(_fileno(file)) == 0;
#else
    ok = ok && ::fsync(fileno(file)) == 0;
#endif
    ok = std::fclose(file) == 0 && ok;
    return ok;
}

/// Makes a rename in `directory` durable (Unix; Windows has no directory handle to flush). Best effort: the file is
/// already in place when this runs.
void syncDirectory([[maybe_unused]] const std::filesystem::path& directory) {
#ifndef _WIN32
    const int fd = ::open(directory.empty() ? "." : directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return;
    ::fsync(fd);
    ::close(fd);
#endif
}

} // namespace

bool exportPsd(const Document& document, const std::string& path, const PsdExportOptions& options, PsdExportSummary* summary, std::string* error) {
    std::vector<uint8_t> bytes = encodePsd(document, options, summary, error);
    if (bytes.empty()) return false;
    const std::filesystem::path target(path);
    std::filesystem::path temp = target;
    temp += ".part";
    std::error_code ec;
    if (!writeDurably(temp, bytes)) {
        if (error) *error = "Couldn't write " + temp.string();
        std::filesystem::remove(temp, ec);
        return false;
    }
    if (injectedFault("rename")) ec = std::make_error_code(std::errc::permission_denied);
    else std::filesystem::rename(temp, target, ec);
    if (ec) {
        if (error) *error = "Couldn't write " + path + ": " + ec.message();
        std::filesystem::remove(temp, ec);
        return false;
    }
    syncDirectory(target.parent_path());
    return true;
}

} // namespace nekophoto
