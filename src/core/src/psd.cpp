#include "nekophoto/psd.h"
#include "psd_channels.h"
#include "psd_read.h"
#include "photoshop.h"
#include "nekophoto/adjustments.h"
#include "nekophoto/colour.h"
#include "nekophoto/colormgmt.h"
#include "nekophoto/colormgmt.h"
#include "nekophoto/depth.h"
#include "nekophoto/parallel.h"
#include "nekophoto/png.h"
#include "nekophoto/render.h"
#include "nekophoto/smartfilter.h"
#include "nekophoto/modetransform.h"
#include "nekophoto/vectorlayer.h"
#include "nekophoto/vectormask.h"
#include "psd/psd_descriptor.hpp"
#include <cstdio>
#include <new>
#include <stdexcept>
#include <zlib.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>

namespace nekophoto {

using namespace photoshop;
using namespace psd_read;

// ---- The import ---------------------------------------------------------------------------------------

std::optional<PsdEstimate> estimatePsd(const std::string& path, std::string* error) {
    // The header and the layer records only, seeking past everything else (a PSB's pixels can run to gigabytes).
    std::ifstream in(path, std::ios::binary);
    if (!in) { if (error) *error = "The file could not be opened."; return std::nullopt; }
    auto read = [&](size_t n) {
        std::vector<uint8_t> bytes(n);
        if (n && !in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(n))) throw Truncated{};
        return bytes;
    };
    auto be = [](const std::vector<uint8_t>& b, size_t at, int n) { uint64_t v = 0; for (int i = 0; i < n; i++) v = v << 8 | b[at + size_t(i)]; return v; };
    try {
        const std::vector<uint8_t> header = read(26);
        if (std::memcmp(header.data(), "8BPS", 4) != 0) { if (error) *error = "Not a Photoshop file (no 8BPS signature)."; return std::nullopt; }
        const bool psb = be(header, 4, 2) == 2;
        const int depth = int(be(header, 22, 2)), mode = int(be(header, 24, 2));
        PsdEstimate e;
        e.height = int(be(header, 14, 4)); e.width = int(be(header, 18, 4));
        const bool nativeMode = mode == CMYK || mode == Lab;
        e.colorMode = mode == CMYK ? ColorMode::CMYK : mode == Lab ? ColorMode::Lab : ColorMode::RGB;
        const bool deep = depth == 16 && (mode == RGB || mode == Grayscale || nativeMode);
        const bool float32 = depth == 32 && (mode == RGB || mode == Grayscale);
        e.sampleType = deep ? SampleType::U16 : float32 ? SampleType::F32 : SampleType::U8;
        e.canvasFits = e.width >= 1 && e.height >= 1 && e.width <= 30000 && e.height <= 30000 &&
                       (long long)e.width * e.height <= Document::imagePixelBudget(e.sampleType, e.colorMode);
        auto skipSection = [&](int bytes) { const uint64_t n = be(read(size_t(bytes)), 0, bytes); in.seekg(std::streamoff(n), std::ios::cur); };
        skipSection(4);   // colour mode data
        skipSection(4);   // image resources
        const int wide = psb ? 8 : 4;
        const uint64_t layerMaskLen = be(read(size_t(wide)), 0, wide);
        const uint64_t layerMaskStart = uint64_t(in.tellg());
        std::vector<Record> records;
        // The records at `at`, `limit` bytes at most: read a growing window until they parse.
        auto readRecords = [&](uint64_t at, uint64_t limit) {
            for (uint64_t window = std::min<uint64_t>(limit, 1 << 22);; window = std::min<uint64_t>(limit, window * 4)) {
                in.clear();
                in.seekg(std::streamoff(at));
                const std::vector<uint8_t> bytes = read(size_t(window));
                try {
                    Reader li(bytes.data(), bytes.size());
                    const int count = std::abs(int(li.i16()));
                    if (count > Document::maxLayers) throw Truncated{};
                    records.clear();
                    for (int i = 0; i < count; i++) records.push_back(readRecord(li, psb));
                    return;
                } catch (Truncated&) {
                    if (window >= limit) throw;
                }
            }
        };
        if (layerMaskLen > 0) {
            const uint64_t layerInfoLen = be(read(size_t(wide)), 0, wide);
            const uint64_t layerInfoStart = uint64_t(in.tellg());
            if (layerInfoLen >= 2) readRecords(layerInfoStart, layerInfoLen);
            if (records.empty()) {
                // 16- and 32-bit files keep their layers in an Lr16 or Lr32 block after the global mask.
                const uint64_t end = layerMaskStart + layerMaskLen;
                in.clear();
                in.seekg(std::streamoff(layerInfoStart + layerInfoLen));
                skipSection(4);
                while (uint64_t(in.tellg()) + 12 <= end) {
                    const std::vector<uint8_t> tag = read(8);
                    const std::string sig(tag.begin(), tag.begin() + 4), key(tag.begin() + 4, tag.end());
                    if (sig != "8BIM" && sig != "8B64") break;
                    const int lengthBytes = psb && psbLongKey(key) ? 8 : 4;
                    const uint64_t len = be(read(size_t(lengthBytes)), 0, lengthBytes);
                    const uint64_t data = uint64_t(in.tellg());
                    if (key == "Lr16" || key == "Lr32") { readRecords(data, len); break; }
                    in.seekg(std::streamoff(data + len + (len % 4 ? 4 - len % 4 : 0)));
                }
            }
        }
        e.layers = int(records.size());
        for (const Record& rec : records) {
            e.layerPixels += (long long)rec.width() * rec.height();
            e.maskPixels += (long long)rec.mask.width() * rec.mask.height();
        }
        const long long projectBudget = Document::projectPixelBudgetAt(e.sampleType, e.colorMode);
        e.layersFit = e.layerPixels <= projectBudget && e.maskPixels <= projectBudget;
        const unsigned long long pixel = (unsigned long long)sampleBytes(e.sampleType) * colorModeChannels(e.colorMode);
        e.mergedBytes = 3ull * (unsigned long long)e.width * (unsigned long long)e.height * pixel;   // the layer, the render, its display copy
        e.bytes = (unsigned long long)e.layerPixels * pixel + (unsigned long long)e.maskPixels * sampleBytes(e.sampleType) + e.mergedBytes;
        return e;
    } catch (Truncated&) {
        if (error) *error = "The file ends early or has a structure this reader does not understand.";
        return std::nullopt;
    }
}

std::optional<PsdImport> importPsd(const std::string& path, std::string* error, const PsdImportOptions& options) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { if (error) *error = "The file could not be opened."; return std::nullopt; }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return importPsdBytes(file, error, options);
}

namespace {

/// A smart object source's contents as an image at their own depth: an embedded PSD/PSB through this importer
/// (Photoshop's merged image when it is real, else our render of its layers; 16 bits from a 16-bit file), PNG
/// directly (16 bits from a 16-bit PNG), anything else through the app's hook.
AnyImage decodeSource(SmartObjectSource& source, const PsdImportOptions& options) {
    if (source.kind != SmartObjectSource::Kind::Embedded || !source.bytes || source.bytes->empty()) return nullptr;
    const std::vector<uint8_t>& bytes = *source.bytes;
    const bool psdFile = bytes.size() >= 4 && std::memcmp(bytes.data(), "8BPS", 4) == 0;
    if (psdFile) {
        if (options.depth + 1 >= psdSmartObjectDepthLimit) return nullptr;
        PsdImportOptions inner = options;
        inner.depth++;
        std::string error;
        auto nested = importPsdBytes(bytes, &error, inner);
        if (!nested) return nullptr;
        // The profile the image below is in, and CMYK or Lab contents at their own layout too (smartobject.h): what a
        // CMYK or Lab document places.
        if (nested->document.colorMode != ColorMode::RGB) {
            source.native = nested->realComposite && nested->compositeNative ? nested->compositeNative : renderNative(nested->document);
            source.nativeMode = nested->document.colorMode;
            source.nativeProfile = nested->document.profile;
        } else source.profile = nested->document.profile;
        if (nested->document.sampleType == SampleType::U16) {
            if (nested->realComposite && nested->composite16) return nested->composite16;
            return Image16Ptr(renderFlattened16(nested->document));
        }
        if (nested->realComposite && nested->composite) return nested->composite;
        return ImagePtr(renderFlattened(nested->document));
    }
    if (bytes.size() >= 8 && std::memcmp(bytes.data(), "\x89PNG", 4) == 0) return decodeSmartObjectPng(bytes);
    if (options.decodeImage) return options.decodeImage(bytes, source.fileType, source.fileName);
    return nullptr;
}

} // namespace

namespace {

/// One layer record's channels, decoded (at 8 bits, and at the file's depth when the document keeps it) and as stored.
struct RecordPlanes {
    std::map<int, std::vector<uint8_t>> planes, maskPlanes;
    /// The mask channels as stored (the carry's).
    std::vector<std::pair<int, std::vector<uint8_t>>> maskRaw;
    std::map<int, std::vector<uint16_t>> widePlanes, wideMaskPlanes;
    std::map<int, std::vector<float>> floatPlanes, floatMaskPlanes;
    /// The colour and transparency channels as stored, for the ones written back while the pixels are these.
    std::vector<PsdLayerCarry::CarriedPlane> rawPlanes;
};

/// A record's tagged block, or none.
const std::pair<const uint8_t*, size_t>* blockOf(const Record& rec, const char* key) {
    auto it = rec.blocks.find(key);
    return it == rec.blocks.end() ? nullptr : &it->second;
}

/// One PSD or PSB being read into a document: the file's sections in order (header, colour mode data, image
/// resources, layer and mask information, the layers' channel data, the merged image), then the document they build.
class PsdReader {
public:
    PsdReader(const std::vector<uint8_t>& file, const PsdImportOptions& options) : file(file), options(options), r(file.data(), file.size()) {}

    std::optional<PsdImport> read(std::string* error) {
        if (!readHeader(error)) return std::nullopt;
        readColourModeData();
        readImageResources();
        readLayerAndMaskInformation();
        if (!layersFit(error)) return std::nullopt;
        buildLayers();
        readMergedImage();
        if (!finish(error)) return std::nullopt;
        result.document = std::move(document);
        return std::move(result);
    }

private:
    const std::vector<uint8_t>& file;
    const PsdImportOptions& options;
    Reader r;
    PsdImport result;
    std::vector<std::string>& notes = result.notes;

    // The header's facts.
    bool psb = false;
    int channels = 0;
    uint32_t width = 0, height = 0;
    int depth = 8, mode = RGB;
    bool nativeMode = false;          // CMYK and Lab open in their own mode
    ColorMode colorMode = ColorMode::RGB;
    bool deep = false, float32 = false;
    SampleType sampleType = SampleType::U8;

    std::vector<uint8_t> palette;
    Document document;
    std::shared_ptr<PsdDocumentCarry> docCarry;
    PsdChannelResources channelResources;
    std::shared_ptr<const CmykToSrgb> cmykProfile;
    /// Whether the file holds a Smart Filter cache at all (a 16-bit one is not read here, see the instances below).
    bool fileHasFilterCache = false;

    std::vector<Record> records;
    bool transparencyFirst = false;
    size_t channelDataStart = 0;

    // ---- the file's sections ---------------------------------------------------------------------------------

    bool readHeader(std::string* error) {
        if (r.chars(4) != "8BPS") { if (error) *error = "Not a Photoshop file (no 8BPS signature)."; return false; }
        const uint16_t version = r.u16();
        if (version != 1 && version != 2) { if (error) *error = "Unknown Photoshop file version."; return false; }
        psb = version == 2;
        r.skip(6);
        channels = r.u16();
        height = r.u32(); width = r.u32();
        depth = r.u16(); mode = r.u16();
        if (width < 1 || height < 1 || width > 30000 || height > 30000) { if (error) *error = "The canvas size is outside the supported range (up to 30,000 pixels per side)."; return false; }
        // CMYK and Lab files open in their own mode (docs/color-modes.md); Duotone, Multichannel, Indexed, Bitmap and
        // Grayscale are converted to RGB.
        nativeMode = mode == CMYK || mode == Lab;
        colorMode = mode == CMYK ? ColorMode::CMYK : mode == Lab ? ColorMode::Lab : ColorMode::RGB;
        // A 16-bit RGB, grayscale, CMYK or Lab file opens as a 16-bit document (docs/bit-depth.md); other depths are reduced.
        deep = depth == 16 && (mode == RGB || mode == Grayscale || nativeMode);
        // A 32-bit RGB or grayscale file opens as a 32-bit document: linear float, its profile read as linear.
        float32 = depth == 32 && (mode == RGB || mode == Grayscale);
        sampleType = deep ? SampleType::U16 : float32 ? SampleType::F32 : SampleType::U8;
        if ((long long)width * height > Document::imagePixelBudget(sampleType, colorMode)) {
            if (error) *error = deep ? "The canvas exceeds the 50-megapixel budget of a 16-bit document." : float32 ? "The canvas exceeds the 25-megapixel budget of a 32-bit document." : "The canvas exceeds the 100-megapixel budget.";
            return false;
        }
        if (depth != 8 && !deep && !float32) notes.push_back(std::to_string(depth) + "-bit channels were reduced to 8 bits.");
        if (nativeMode) {}
        else if (mode == Indexed || mode == Duotone || mode == Multichannel || mode == Bitmap) notes.push_back("The file's colour mode (" + std::string(mode == Indexed ? "indexed" : mode == Duotone ? "duotone" : mode == Multichannel ? "multichannel" : "bitmap") + ") was converted to RGB.");
        else if (mode != RGB && mode != Grayscale) { if (error) *error = "Unsupported colour mode " + std::to_string(mode) + "."; return false; }
        return true;
    }

    /// Colour mode data (a palette for indexed files).
    void readColourModeData() {
        uint32_t colourDataLen = r.u32();
        if (colourDataLen) { const uint8_t* p = r.bytes(colourDataLen); if (mode == Indexed && colourDataLen >= 768) palette.assign(p, p + 768); }
    }

    /// Image resources: the resolution is ours; the rest is carried for PSD export (psd_carry.h).
    void readImageResources() {
        document = Document{int(width), int(height)};
        document.colorMode = colorMode;
        docCarry = std::make_shared<PsdDocumentCarry>();
        docCarry->width = int(width); docCarry->height = int(height);
        uint32_t resourcesLen = r.u32();
        size_t resourcesEnd = r.position() + resourcesLen;
        while (r.position() + 12 <= resourcesEnd) {
            if (r.chars(4) != "8BIM") break;
            uint16_t id = r.u16();
            std::string resourceName = r.pascal(2);
            uint32_t len = r.u32();
            size_t dataStart = r.position();
            if (len <= r.remaining() && carriedResource(id, mode)) {
                const uint8_t* p = r.bytes(len);
                docCarry->resources.push_back({id, resourceName, std::vector<uint8_t>(p, p + len)});
                r.seek(dataStart);
            }
            if ((id == 1006 || id == 1045 || id == 1077 || id == 1007 || id == 1053) && len <= r.remaining() && len <= 65536) {
                // Alpha and spot channels' names, display and identifiers (psd_channels.h).
                const uint8_t* p = r.bytes(len);
                std::vector<uint8_t>& into = id == 1006 ? channelResources.names : id == 1045 ? channelResources.unicodeNames : id == 1077 ? channelResources.displayInfo
                                             : id == 1007 ? channelResources.oldDisplayInfo : channelResources.identifiers;
                into.assign(p, p + len);
                r.seek(dataStart);
            }
            if (id == 0x03ED && len >= 4) { double hres = r.u32() / 65536.0; if (hres > 0 && hres < 100000) document.resolution = hres; }
            if (id == 1057 && len >= 5) { r.u32(); result.realComposite = r.u8() != 0; }   // version info: hasRealMergedData
            if (id == 1039 && mode == RGB && len <= r.remaining()) {
                // An RGB file's profile: the document's (colormgmt.h), kept byte for byte for the round trip.
                const uint8_t* icc = r.bytes(len);
                if (auto profile = profileFromIcc(icc, len); profile && profile->model == ColorModel::RGB) document.profile = std::move(*profile);
                r.seek(dataStart);
            }
            if (id == 1039 && mode == CMYK && len <= r.remaining()) {
                // A CMYK file's profile is the document's (kept byte for byte for the round trip); the RGB stand-ins
                // made while reading (Photoshop's merged image for checks) go to sRGB through it.
                const uint8_t* icc = r.bytes(len);
                cmykProfile = CmykToSrgb::fromProfile(std::vector<uint8_t>(icc, icc + len));
                if (auto profile = profileFromIcc(icc, len); profile && profile->model == ColorModel::CMYK) document.profile = std::move(*profile);
                r.seek(dataStart);
            }
            r.seek(dataStart + len + (len & 1));
        }
        r.seek(resourcesEnd);
        for (auto& n : notes)
            if (cmykProfile && n.rfind("CMYK colour was converted", 0) == 0) n = "CMYK colour was converted to sRGB through the file's own colour profile.";
    }

    /// The layer records at `li` (a layer info section, or an Lr16 / Lr32 block's).
    void readLayerInfo(Reader& li) {
        int16_t count = li.i16();
        transparencyFirst = count < 0;
        count = int16_t(std::abs(int(count)));
        if (count > Document::maxLayers) throw Truncated{};
        for (int i = 0; i < count; i++) records.push_back(readRecord(li, psb));
        channelDataStart = li.position();
    }

    /// Layer and mask information: the layer records, then the global blocks (carried, and the smart object sources
    /// in the linked-file blocks).
    void readLayerAndMaskInformation() {
        const uint64_t layerMaskLen = r.length(psb);
        const size_t layerMaskEnd = r.position() + size_t(layerMaskLen);
        if (layerMaskLen > 0) {
            const uint64_t layerInfoLen = r.length(psb);
            const size_t layerInfoStart = r.position();
            if (layerInfoLen > 0) { Reader li(file.data(), file.size()); li.seek(layerInfoStart); readLayerInfo(li); }
            r.seek(layerInfoStart + size_t(layerInfoLen));
            // Global layer mask info, then additional blocks: 16- and 32-bit files keep their layers there.
            if (r.position() + 4 <= layerMaskEnd) { uint32_t globalLen = r.u32(); r.skip(globalLen); }
            std::map<std::string, std::pair<const uint8_t*, size_t>> globalBlocks;
            std::vector<std::pair<std::string, std::pair<const uint8_t*, size_t>>> globalOrder;
            if (r.position() < layerMaskEnd) readTaggedBlocks(r, layerMaskEnd, psb, 4, globalBlocks, &globalOrder);
            for (auto& [key, data] : globalOrder) {
                if (key == "FEid" || key == "FXid") fileHasFilterCache = true;
                if (carriedGlobalBlock(key) && !((psb || depth != 8) && (key == "FEid" || key == "FXid"))) docCarry->globals.push_back({key, std::vector<uint8_t>(data.first, data.first + data.second)});
                // Smart object sources: the linked-file blocks' embedded (and linked) files.
                if (!options.mergedOnly && (key == "lnk2" || key == "lnkD" || key == "lnk3" || key == "lnkE"))
                    for (SmartObjectSource& s : parsePsdLinkBlock(std::vector<uint8_t>(data.first, data.first + data.second))) {
                        if (s.id.empty() || document.smartObjects.count(s.id)) continue;
                        s.psdBlock = key;
                        s.image = decodeSource(s, options);
                        if (s.image) { s.width = s.image.width(); s.height = s.image.height(); }
                        const std::string id = s.id;
                        document.smartObjects[id] = std::make_shared<const SmartObjectSource>(std::move(s));
                    }
            }
            if (records.empty()) {
                for (const char* key : {"Lr16", "Lr32"}) {
                    auto it = globalBlocks.find(key);
                    if (it == globalBlocks.end()) continue;
                    Reader li(file.data(), file.size());
                    li.seek(size_t(it->second.first - file.data()));
                    readLayerInfo(li);
                    break;
                }
            }
        }
        if (options.mergedOnly) {
            // Photoshop's merged image alone: no layers, nothing of theirs carried.
            records.clear();
            docCarry->globals.clear();
            document.smartObjects.clear();
        }
        r.seek(layerMaskEnd);
    }

    /// A project holds a gigapixel of layers (and as much of masks); refuse more before decoding any of it.
    bool layersFit(std::string* error) const {
        long long layerTotal = 0, maskTotal = 0;
        for (const Record& rec : records) {
            layerTotal += (long long)rec.width() * rec.height();
            maskTotal += (long long)rec.mask.width() * rec.mask.height();
        }
        const long long projectBudget = Document::projectPixelBudgetAt(sampleType, colorMode);
        if (layerTotal > projectBudget || maskTotal > projectBudget) {
            if (error) *error = "The layers total " + std::to_string(std::max(layerTotal, maskTotal) / 1000000) + " megapixels; a " + (deep ? "16-bit document holds up to 500." : float32 ? "32-bit document holds up to 250." : "project holds up to 1,000.");
            return false;
        }
        return true;
    }

    // ---- a record's channels ---------------------------------------------------------------------------------

    /// A shape layer whose pixels are its drawing (a path with a fill or a stroke, and pixels): Photoshop's own,
    /// the fill cut by the path and the stroke over it. A soft vector mask (density, feather) is drawn here instead, and
    /// so is a 32-bit shape (Photoshop stores none; pixels there are a fill written by us, which the path cuts).
    bool drawnShapeRecord(const Record& rec) const {
        auto has = [&](const char* key) { return rec.blocks.count(key) > 0; };
        return !float32 && rec.width() > 0 && rec.height() > 0 && rec.section == 0 && (has("vmsk") || has("vsms"))
            && (has("SoCo") || has("GdFl") || has("PtFl") || has("vscg") || has("vstk")) && !softVectorMask(rec.mask.section);
    }

    /// Channel image data follows the records, one channel after another in record order: `rec`'s, from `cursor` on.
    void decodeRecordChannels(const Record& rec, RecordPlanes& out, size_t& cursor) {
        // An 8-bit RGB smart object's channels too: with Smart Filters and a filter mask, its pixels here are the
        // mask applied (applySmartFilterMask), and Photoshop's own unmasked ones go back on export.
        const bool smartChannels = depth == 8 && mode == RGB && (rec.blocks.count("SoLd") || rec.blocks.count("SoLE"));
        for (const RecordChannel& c : rec.channels) {
            // Subtraction, not addition: a PSB's channel length is a full 64-bit field, so `cursor +
            // c.length` wraps and a wrapped sum passes the test while the reader runs off the file.
            if (c.length < 2 || cursor > file.size() || c.length > file.size() - cursor) throw Truncated{};
            Reader ch(file.data() + cursor, size_t(c.length));
            int compression = ch.u16();
            const uint8_t* data = file.data() + cursor + 2;
            size_t size = size_t(c.length) - 2;
            int w = rec.width(), h = rec.height();
            if (c.id == -2) { w = rec.mask.width(); h = rec.mask.height(); }
            if (c.id == -3) { w = rec.mask.realWidth(); h = rec.mask.realHeight(); }
            if (c.id == -2 || c.id == -3) out.maskRaw.push_back({c.id, std::vector<uint8_t>(file.data() + cursor, file.data() + cursor + size_t(c.length))});
            // Only a file whose colour model is the document's: a Grayscale (or Duotone…) file opens as RGB, and
            // its one grey plane written back into an RGB record would leave Green and Blue empty.
            else if ((deep || float32 || nativeMode || smartChannels || drawnShapeRecord(rec)) && (mode == RGB || nativeMode) && !psb && c.id >= -1 && c.id <= (mode == CMYK ? 3 : 2)) out.rawPlanes.push_back({c.id, std::vector<uint8_t>(file.data() + cursor, file.data() + cursor + size_t(c.length))});
            std::vector<uint8_t> plane;
            std::vector<uint16_t> wide;
            std::vector<float> floats;
            std::string why;
            if (w > 0 && h > 0 && decodePlane(data, size, compression, w, h, depth, psb, plane, &why, deep ? &wide : nullptr, nullptr, float32 ? &floats : nullptr)) {
                const bool isMask = c.id == -2 || c.id == -3;
                (isMask ? out.maskPlanes : out.planes)[c.id] = std::move(plane);
                if (deep) (isMask ? out.wideMaskPlanes : out.widePlanes)[c.id] = std::move(wide);
                if (float32) (isMask ? out.floatMaskPlanes : out.floatPlanes)[c.id] = std::move(floats);
            }
            else if (w > 0 && h > 0) notes.push_back("Layer \"" + rec.name + "\": a channel could not be read (" + why + ").");
            cursor += size_t(c.length);
        }
    }

    // ---- records to layers -----------------------------------------------------------------------------------

    /// What the record holds that NekoPhoto does not model, bound to the layer's content as imported.
    std::shared_ptr<const PsdLayerCarry> carryFor(const Record& rec, const Layer& layer, bool modelledAdjustment,
                                                  const std::vector<std::pair<int, std::vector<uint8_t>>>& maskRaw,
                                                  std::vector<PsdLayerCarry::CarriedPlane> rawPlanes = {}, bool planesRead = false,
                                                  bool filterMasked = false) const {
        auto carry = std::make_shared<PsdLayerCarry>();
        for (auto& [key, data] : rec.ordered)
            if (carriedLayerBlock(key, modelledAdjustment, layer.smartObject.has_value())) carry->blocks.push_back({key, std::vector<uint8_t>(data.first, data.first + data.second)});
        carry->blendingRanges = rec.blendingRanges;
        carry->blendKey = rec.blend;
        carry->blendAs = int(layer.blendMode);
        carry->flags = rec.flags;
        carry->closedFolder = rec.section == 2;
        carry->opacity = rec.opacity;
        carry->fill = rec.fillOpacity;
        auto lyid = rec.blocks.find("lyid");
        if (lyid != rec.blocks.end() && lyid->second.second >= 4) { Reader id(lyid->second.first, 4); carry->layerId = id.u32(); }
        carry->contentHash = psdContentHash(layer.asset ? layer.asset->image : AnyImage());
        // A 16- or 32-bit layer's channels as stored, and a CMYK or Lab layer's at either depth: written back while
        // its pixels are these (psd_carry.h). A CMYK or Lab layer whose pixels were not read from its planes (a smart
        // object's contents drawn here) has none.
        // A smart object drawn from its contents keeps the record's planes as Photoshop's raster of the placement.
        const bool smartRaster = layer.smartObject && !layer.smartObject->locked() && layer.smartImage == layer.asset->image
                                 && smartObjectPixelsArePlacement(*layer.smartObject);
        // An 8-bit RGB smart object with its filter mask applied here keeps Photoshop's unmasked raster the same way.
        const bool eightBit = !deep && !float32 && !nativeMode;
        // A shape's drawing as Photoshop stored it (vectorlayer.h, shapeShowsDrawnPixels), its channels kept at 8 bits too
        // so an unedited shape goes back byte for byte (premultiplying loses its faint edge's colour).
        carry->drawnShape = drawnShapeRecord(rec) && layer.asset && layer.asset->image && !layer.text && !layer.smartObject && !layer.adjustment;
        const bool drawnPlanes = carry->drawnShape && mode == RGB && bool(layer.asset->image.u8());
        if (!rawPlanes.empty() && layer.asset && rec.width() > 0 && rec.height() > 0
            && (eightBit ? drawnPlanes || filterMasked : smartRaster || (nativeMode ? planesRead : bool(layer.asset->image.u16()) || bool(layer.asset->image.f32())))) {
            carry->planes = std::move(rawPlanes);
            carry->planesHash = carry->contentHash;
            if (smartRaster || filterMasked) carry->planesRect = std::array<int, 4>{rec.left, rec.top, rec.right, rec.bottom};
        }
        if (layer.adjustment) {
            // Settings as read, so an unchanged adjustment's own block goes back byte for byte.
            AdjustmentSettings read;
            if (AdjustmentSettings::parse(layer.adjustment->json, read)) carry->adjustmentJson = read.toJson();
        }
        carry->placement = layer.transform;
        if (!psb && (depth == 8 || deep || float32) && !rec.mask.section.empty()) {
            carry->maskData = rec.mask.section;
            carry->maskChannels = maskRaw;
            carry->maskDepth = depth;
            carry->maskHash = layer.mask ? psdMaskHash(layer.mask->asset.image, layer.mask->enabled) : 0;
        }
        const bool plainBlend = rec.blend == "norm" || (rec.blend == "pass" && layer.isGroup);
        if (carry->blocks.empty() && carry->blendingRanges.empty() && carry->layerId == 0 && carry->maskData.empty()
            && plainBlend && (rec.flags & ~0x0A) == 0 && !carry->closedFolder && carry->planes.empty()) return nullptr;
        return carry;
    }

    /// The user mask over a lw x lh grid at (lx, ly): Photoshop keeps it in its own rectangle with a default beyond.
    std::optional<LayerMask> userMaskFor(const Record& rec, const RecordPlanes& planes, int lw, int lh, int lx, int ly) const {
        // The painted mask: the real one when there is one, else -2 (which may be the vector mask
        // rendered: then it stands in for it here, and the vector mask itself is carried).
        const bool real = rec.mask.real && planes.maskPlanes.count(-3);
        // Without a painted mask, a plane "rendered from other data" (flag bit 3) is the vector mask baked,
        // unfeathered: the vector mask itself draws it here, and the section goes back as stored.
        if (!real && (rec.mask.flags & 0x08)) return std::nullopt;
        if (!rec.mask.present || lw <= 0 || lh <= 0) return std::nullopt;
        const int top = real ? rec.mask.realTop : rec.mask.top, left = real ? rec.mask.realLeft : rec.mask.left;
        const int mw = real ? rec.mask.realWidth() : rec.mask.width(), mh = real ? rec.mask.realHeight() : rec.mask.height();
        const uint8_t outside = real ? rec.mask.realDefault : rec.mask.defaultColour;
        // A feathered mask: Photoshop blurs the mask's own plane (its rectangle, the default colour beyond,
        // the canvas edge repeated), not the layer's, so what the mask holds past the layer's pixels shades
        // their edges. The mask then keeps everything within the blur's reach of them, in a placement of its
        // own (photoshop-user-mask-params: a mask wider than its layer leaves the layer's sides unfaded).
        bool placed = false;
        if (const auto parameters = parseMaskParameters(rec.mask.section); parameters && parameters->userFeather && *parameters->userFeather > 0) {
            const int reach = int(std::ceil(std::min(*parameters->userFeather, 1000.0) * 3)) + 1;
            const int64_t x0 = std::min<int64_t>(std::max<int64_t>(std::min<int64_t>(lx, left) - reach, 0), lx);
            const int64_t y0 = std::min<int64_t>(std::max<int64_t>(std::min<int64_t>(ly, top) - reach, 0), ly);
            const int64_t x1 = std::max<int64_t>(std::min<int64_t>(std::max<int64_t>(int64_t(lx) + lw, int64_t(left) + mw) + reach, int64_t(width)), int64_t(lx) + lw);
            const int64_t y1 = std::max<int64_t>(std::min<int64_t>(std::max<int64_t>(int64_t(ly) + lh, int64_t(top) + mh) + reach, int64_t(height)), int64_t(ly) + lh);
            if ((x0 < lx || y0 < ly || x1 > int64_t(lx) + lw || y1 > int64_t(ly) + lh) && x1 - x0 <= maxImageSide && y1 - y0 <= maxImageSide
                && uint64_t(x1 - x0) * uint64_t(y1 - y0) <= uint64_t(maxImageSide) * uint64_t(maxImageSide)) {
                lx = int(x0); ly = int(y0); lw = int(x1 - x0); lh = int(y1 - y0);
                placed = true;
            }
        }
        // The mask over the layer's grid at the document's depth; the default colour beyond its rectangle.
        auto place = [&](const auto& maskPlanesAt, auto mask) -> bool {
            auto userMask = maskPlanesAt.find(real ? -3 : -2);
            if (userMask == maskPlanesAt.end() || size_t(std::max(0, mw)) * size_t(std::max(0, mh)) > userMask->second.size()) return false;
            for (int y = 0; y < mh; y++) {
                const int ty = top + y - ly;
                if (ty < 0 || ty >= lh) continue;
                for (int x = 0; x < mw; x++) { const int tx = left + x - lx; if (tx >= 0 && tx < lw) mask->at(tx, ty) = userMask->second[size_t(y) * mw + size_t(x)]; }
            }
            return true;
        };
        LayerMask lm;
        if (deep) {
            auto mask = std::make_shared<Gray16>(lw, lh, widen8(outside));
            if (!place(planes.wideMaskPlanes, mask)) return std::nullopt;
            lm.asset = MaskAsset::make(Gray16Ptr(mask));
        } else if (float32) {
            auto mask = std::make_shared<GrayF>(lw, lh, float(outside) / 255.0f);
            if (!place(planes.floatMaskPlanes, mask)) return std::nullopt;
            cleanFloat(*mask);
            lm.asset = MaskAsset::make(GrayFPtr(mask));
        } else {
            auto mask = std::make_shared<GrayImage>(lw, lh, outside);
            if (!place(planes.maskPlanes, mask)) return std::nullopt;
            lm.asset = MaskAsset::make(mask);
        }
        lm.enabled = !((real ? rec.mask.realFlags : rec.mask.flags) & 2);
        if (placed) lm.placement = LayerTransform(Point(lx, ly), Size(lw, lh));
        return lm;
    }

    // Records run bottom to top. Folders arrive as an end marker first, then their contents, then the
    // folder itself; clipped layers sit directly above what they clip to.
    struct OpenGroup { size_t firstChild; std::vector<Uuid> members; const Record* end = nullptr; };
    std::vector<OpenGroup> open;
    std::map<std::string, int> lockedSmartObjects;

    void buildLayers() {
        size_t cursor = channelDataStart;
        for (const Record& rec : records) {
            RecordPlanes planes;
            decodeRecordChannels(rec, planes, cursor);
            if (rec.section == 3) { open.push_back({document.layers.size(), {}, &rec}); continue; }   // a folder's end marker: its contents follow
            if (rec.section == 1 || rec.section == 2) addFolder(rec, planes);
            else addLayer(rec, planes);
        }
        // Layer styles, vector masks and shapes, and editable smart objects open as Photoshop has them: nothing to note.
        for (auto& [why, count] : lockedSmartObjects)
            notes.push_back(std::to_string(count) + " smart object(s) " + why + " show Photoshop's preview: they can be moved and scaled, and stay smart objects on PSD export.");
        while (!open.empty()) open.pop_back();   // an unterminated folder: its members stay at the top level
    }

    /// A folder, from its record (its contents were read after its end marker's).
    void addFolder(const Record& rec, const RecordPlanes& planes) {
        if (open.empty()) return;
        std::vector<Layer>& layers = document.layers;
        bool lossyBlend = false;
        BlendMode blend = blendFor(rec.blend, &lossyBlend);
        OpenGroup group = open.back();
        open.pop_back();
        Layer folder(rec.name.empty() ? "Folder" : rec.name, document.size());
        folder.isGroup = true;
        folder.visible = !(rec.flags & 2);
        // Fill fades the folder's contents as its opacity does (a Photoshop folder at Fill 0 hides them),
        // its effects excepted.
        folder.opacity = rec.opacity / 255.0;
        folder.fill = rec.fillOpacity / 255.0;
        folder.blendMode = blend;
        folder.passThrough = rec.blend == "pass";
        if (auto lm = userMaskFor(rec, planes, int(width), int(height), 0, 0)) folder.mask = lm;   // over the canvas, as our folders are
        folder.psdCarry = carryFor(rec, folder, false, planes.maskRaw);
        for (const char* key : {"artb", "artd", "abdd"}) {
            if (folder.artboard) break;
            auto it = rec.blocks.find(key);
            if (it != rec.blocks.end()) folder.artboard = parseArtboardBlock(std::vector<uint8_t>(it->second.first, it->second.first + it->second.second));
        }
        if (group.end) {
            auto carry = std::make_shared<PsdLayerCarry>(folder.psdCarry ? *folder.psdCarry : PsdLayerCarry{});
            for (auto& [key, data] : group.end->ordered)
                if (carriedLayerBlock(key, false)) carry->endBlocks.push_back({key, std::vector<uint8_t>(data.first, data.first + data.second)});
            carry->endRanges = group.end->blendingRanges;
            if (!folder.psdCarry) { carry->placement = folder.transform; carry->opacity = rec.opacity; carry->fill = rec.fillOpacity; carry->blendKey = rec.blend; carry->blendAs = int(folder.blendMode); carry->flags = rec.flags; carry->closedFolder = rec.section == 2; }
            if (!carry->endBlocks.empty() || !carry->endRanges.empty()) folder.psdCarry = carry;
        }
        if (lossyBlend && rec.blend != "pass") notes.push_back("Folder \"" + folder.name + "\": blend mode " + blendDescription(rec.blend) + " has no counterpart; " + blendModeName(blend) + " was used.");
        for (const Uuid& id : group.members) if (Layer* l = document.find(id)) l->parentId = folder.id;
        layers.insert(layers.begin() + long(group.firstChild), folder);
        if (!open.empty()) open.back().members.push_back(folder.id);
    }

    /// An adjustment layer's settings, from the first block that holds them; a note for one that does not read.
    std::optional<AdjustmentSettings> adjustmentSettings(const Record& rec) {
        auto block = [&](const char* key) { return blockOf(rec, key); };
        std::optional<AdjustmentSettings> settings;
        if (auto b = block("levl")) settings = levelsFrom(b->first, b->second, mode == Grayscale || mode == Duotone);
        else if (auto b = block("curv")) settings = curvesFrom(b->first, b->second, mode == Grayscale || mode == Duotone);
        else if (auto b = block("hue2")) settings = hueSaturationFrom(b->first, b->second);
        else if (auto b = block("expA")) settings = exposureFrom(b->first, b->second, mode == Grayscale || mode == Duotone);
        else if (auto b = block("grdm")) settings = gradientMapFrom(b->first, b->second, mode, document.profile);
        else if (block("CgEd") || block("brit")) settings = brightnessContrastFrom(block("CgEd"), block("brit"));
        if (!settings)
            for (const char* key : {"nvrt", "post", "thrs", "blnc", "mixr", "selc", "phfl", "blwh", "vibA", "clrL"})
                if (auto b = block(key)) { settings = simpleAdjustmentFrom(mode == 4 && std::string(key) == "mixr" ? "mixr/cmyk" : key, b->first, b->second); break; }
        if (!settings) {
            static const std::map<std::string, const char*> others{{"brit", "Brightness/Contrast"}, {"blwh", "Black & White"}, {"vibA", "Vibrance"}, {"phfl", "Photo Filter"}, {"mixr", "Channel Mixer"},
                {"clrL", "Color Lookup"}, {"nvrt", "Invert"}, {"post", "Posterize"}, {"thrs", "Threshold"}, {"selc", "Selective Color"}, {"blnc", "Color Balance"}};
            for (auto& [key, name] : others) if (block(key.c_str())) { notes.push_back("Layer \"" + rec.name + "\": its " + name + " settings could not be read; it shows as an empty layer here and is written back to PSD as it was."); break; }
        }
        return settings;
    }

    /// A layer (pixels, an adjustment, a fill, text or a smart object), from its record.
    void addLayer(const Record& rec, RecordPlanes& planes) {
        std::vector<Layer>& layers = document.layers;
        const bool hidden = rec.flags & 2;
        bool lossyBlend = false;
        BlendMode blend = blendFor(rec.blend, &lossyBlend);
        // Pixels, or what stands in for them.
        std::shared_ptr<Image> image;
        std::optional<std::array<float, 4>> linearFill;   // a 32-bit file's solid colour: its values are linear
        std::optional<LayerAdjustment> adjustment;
        std::string extraJson;
        auto block = [&](const char* key) { return blockOf(rec, key); };
        if (rec.width() > 0 && rec.height() > 0 && !planes.planes.empty()) image = assemble(mode, rec.width(), rec.height(), planes.planes, palette, cmykProfile.get());
        const std::optional<AdjustmentSettings> settings = adjustmentSettings(rec);
        if (settings) adjustment = settings->toLayerAdjustment();
        if (!image && !adjustment) {
            if (auto b = block("SoCo")) {
                if (auto colour = solidColourFrom(b->first, b->second)) {
                    image = std::make_shared<Image>(int(width), int(height));
                    image->fill(uint8_t(std::lround(colour->red * 255)), uint8_t(std::lround(colour->green * 255)), uint8_t(std::lround(colour->blue * 255)), 255);
                    if (float32) linearFill = std::array<float, 4>{float(colour->red), float(colour->green), float(colour->blue), 1.0f};
                }
            }
        }
        std::optional<PsdTypeLayer> type;
        if (block("TySh")) {
            std::string why;
            type = readPhotoshopType(block("TySh")->first, block("TySh")->second, &why);
            // Without pixels only an empty type layer (a click with the Type tool, nothing typed) can be text.
            if (type && !image && !type->text.text.empty()) { type.reset(); why.clear(); }
            if (auto text = textFrom(block("TySh")->first, block("TySh")->second)) {
                extraJson = "{\"psdText\":\"" + jsonEscape(*text) + "\"";
                // Where Photoshop anchored the first baseline, so the first redraw here lands on it.
                if (type) {
                    char anchor[128];
                    std::snprintf(anchor, sizeof anchor, ",\"psdTextAnchor\":[%.4f,%.4f]", type->anchorX, type->anchorY);
                    extraJson += anchor;
                    if (type->rotation != 0) { std::snprintf(anchor, sizeof anchor, ",\"psdTextRotation\":%.6f", type->rotation); extraJson += anchor; }
                }
                extraJson += "}";
            }
            if (!type) notes.push_back("Layer \"" + rec.name + "\": its text is " + (why.empty() ? std::string("without pixels") : why) + ", which NekoPhoto text cannot be; it shows as Photoshop drew it, and is Photoshop text again on PSD export while its pixels are unchanged.");
        }
        // An adjustment or fill kind with no counterpart stays as an empty layer that carries it, as does an
        // empty layer (a divider, a layer never painted on), so the structure survives.

        Layer layer;
        if (image) {
            const bool solidFill = rec.width() == 0;
            layer = Layer(Asset::make(image, rec.name), Point(solidFill ? 0 : rec.left, solidFill ? 0 : rec.top));
        } else {
            layer = Layer(rec.name, document.size());
        }
        layer.name = rec.name.empty() ? "Layer" : rec.name;
        layer.visible = !hidden;
        layer.opacity = rec.opacity / 255.0;
        layer.fill = rec.fillOpacity / 255.0;
        layer.blendMode = blend;
        layer.adjustment = adjustment;
        layer.extraJson = extraJson;
        if (type && !image) {
            // Empty text: a transparent pixel where Photoshop anchored it, so it is a text layer to type into.
            layer = Layer(Asset::make(std::make_shared<Image>(1, 1), rec.name), Point(std::floor(type->anchorX), std::floor(type->anchorY)));
            layer.name = rec.name.empty() ? "Layer" : rec.name;
            layer.visible = !hidden;
            layer.opacity = rec.opacity / 255.0;
            layer.fill = rec.fillOpacity / 255.0;
            layer.blendMode = blend;
            layer.extraJson = extraJson;
        }
        // A 16-bit file's pixels at their depth: the 8-bit image stood in for the checks above.
        if (deep && !nativeMode && image && layer.asset && layer.asset->image.u8() == image && !planes.widePlanes.empty()) {
            layer.asset = Asset::make(Image16Ptr(assemble16(mode, rec.width(), rec.height(), planes.widePlanes)), layer.asset->name);
        }
        // A 32-bit file's pixels in linear float, likewise.
        if (float32 && image && layer.asset && layer.asset->image.u8() == image && !planes.floatPlanes.empty()) {
            layer.asset = Asset::make(ImageFPtr(assembleF(mode, rec.width(), rec.height(), planes.floatPlanes)), layer.asset->name);
        } else if (linearFill && image && layer.asset && layer.asset->image.u8() == image) {
            // A solid fill without pixels: Photoshop's 32-bit colour is linear already (psd-tools' 300dpi: 172, 11.5,
            // 11.5 shows as 214, 60, 60), not an sRGB colour to linearise.
            auto fill = std::make_shared<ImageF>(image->width(), image->height());
            fill->fill(linearFill->data());
            layer.asset = Asset::make(ImageFPtr(fill), layer.asset->name);
        }
        if (type) {
            layer.text = type->text;
            layer.textImage = layer.asset->image;
            result.texts.push_back({layer.id, type->postScriptName, type->runPostScriptNames, type->leading, type->autoLeading});
        }
        if (lossyBlend) notes.push_back("Layer \"" + layer.name + "\": blend mode " + blendDescription(rec.blend) + " has no counterpart; " + blendModeName(blend) + " was used.");
        // The mask covers the layer's pixels (the canvas for a layer without any).
        if (auto lm = userMaskFor(rec, planes, image ? image->width() : int(width), image ? image->height() : int(height),
                                  image ? int(layer.transform.origin.x) : 0, image ? int(layer.transform.origin.y) : 0)) layer.mask = lm;
        bool smartInMode = false, filterMasked = false;
        if (image) placeSmartObject(rec, layer, smartInMode, filterMasked);
        const bool planesRead = toDocumentLayout(layer, rec, planes, image, smartInMode);
        layer.psdCarry = carryFor(rec, layer, settings.has_value(), planes.maskRaw, std::move(planes.rawPlanes), planesRead, filterMasked);
        if (rec.clipping) {
            // Clipped to the nearest unclipped layer below it in the same folder.
            const size_t from = open.empty() ? 0 : open.back().firstChild;
            for (size_t i = layers.size(); i-- > from;) if (!layers[i].maskSourceId && !layers[i].isGroup) { layer.maskSourceId = layers[i].id; break; }
        }
        layers.push_back(layer);
        if (!open.empty()) open.back().members.push_back(layer.id);
    }

    /// A placed layer (it has pixels): an instance of its source. Editable, its pixels become the source's image placed
    /// by the quad (the mask keeps the place it had); otherwise it shows Photoshop's preview, locked. `inMode`: the
    /// layer's pixels are already in a CMYK or Lab document's layout; `filterMasked`: Photoshop's filtered pixels with
    /// the filter mask applied here.
    void placeSmartObject(const Record& rec, Layer& layer, bool& smartInMode, bool& filterMasked) {
        auto block = [&](const char* key) { return blockOf(rec, key); };
        std::optional<PsdPlacement> placement;
        SmartObjectInstance instance;
        for (const char* key : {"SoLd", "SoLE", "PlLd", "plLd"})
            if (auto b = block(key)) {
                instance.psdBlocks.push_back({key, std::vector<uint8_t>(b->first, b->first + b->second)});
                if (!placement) placement = parsePsdPlacement(key, instance.psdBlocks.back().data);
            }
        if (!placement) return;
        instance.sourceId = placement->sourceId;
        instance.placedId = placement->placedId;
        instance.quad = placement->quad;
        auto source = document.smartObjects.find(placement->sourceId);
        const bool legacy = !block("SoLd") && !block("SoLE");
        std::optional<LayerTransform> placed;
        if (source != document.smartObjects.end() && source->second->image)
            placed = transformForQuad(placement->quad, source->second->image.width(), source->second->image.height());
        using Lock = SmartObjectInstance::Lock;
        instance.lock = legacy ? Lock::Legacy : placement->warped ? Lock::Warp : placement->filtered ? Lock::Filters
            : source == document.smartObjects.end() || source->second->kind == SmartObjectSource::Kind::Linked ? Lock::Linked
            : !source->second->image ? Lock::Unreadable : placement->nonAffine || !placed ? Lock::Perspective : Lock::None;
        std::optional<AnyPlacedRaster> warped;
        bool keptFiltered = false;
        // A CMYK or Lab document places the contents in its own layout (through the profiles when they are
        // of another mode); `inMode` says the layer's pixels below are already in it.
        const SmartObjectTarget placeTarget = nativeMode ? SmartObjectTarget(sampleType, colorMode, &document.profile) : SmartObjectTarget(sampleType);
        bool inMode = false;
        if (instance.lock == Lock::Filters && source != document.smartObjects.end() && source->second->kind == SmartObjectSource::Kind::Embedded
            && source->second->image) {
            // Smart Filters NekoPhoto draws: editable. The layer's pixels are Photoshop's own filtered raster
            // for this placement, so they are kept as read (drawing the stack here can take seconds);
            // moving, scaling or editing the filters draws it (refreshSmartObjectRasters, setSmartFilters).
            // A 16-bit file with no cache at all (as NekoPhoto's 16-bit writer leaves it): the stack is drawn
            // on the document's canvas with its mask all white, a cache record made for it here.
            if (deep && !fileHasFilterCache && !psb)
                addDefaultSmartFilterCache(docCarry->globals, instance, *source->second, int(width), int(height));
            if (smartFiltersDrawable(docCarry->globals, instance)) {
                if (layer.asset && layer.asset->image && layer.asset->image.width() > 0 && layer.asset->image.height() > 0) {
                    instance.lock = Lock::None;
                    keptFiltered = true;
                } else if (auto filtered = drawSmartObjectRaster(docCarry->globals, instance, *source->second, placement->quad, placeTarget,
                                                                 SmartObjectDraw::Filtered)) {
                    warped = std::move(filtered);
                    instance.lock = Lock::None;
                    inMode = nativeMode;
                }
            }
        }
        if (!warped && !keptFiltered && placement->warp && (instance.lock == Lock::None || instance.lock == Lock::Perspective)) {
            // A warp NekoPhoto draws: from the contents, through the mesh, onto the quad (any quad).
            if (nativeMode) {
                if (const AnyImage contents = smartObjectSourceImage(*source->second, placeTarget))
                    if (auto w = renderWarpedImageAny(contents, *placement->warp, placement->quad); w && w->image) {
                        warped = AnyPlacedRaster{w->image, 0, 0, w->transform};
                        inMode = true;
                    }
            } else if (const AnyImage contents = smartObjectSourceImage(*source->second, sampleType); contents.u16()) {
                if (auto w = renderWarpedImage(*contents.u16(), *placement->warp, placement->quad))
                    warped = AnyPlacedRaster{Image16Ptr(w->image), 0, 0, w->transform};
            } else if (contents.u8()) {
                if (auto w = renderWarpedImage(*contents.u8(), *placement->warp, placement->quad))
                    warped = AnyPlacedRaster{ImagePtr(w->image), 0, 0, w->transform};
            }
            instance.lock = warped ? Lock::None : Lock::Warp;
        }
        if (warped) {
            const LayerTransform raster = layer.transform;
            layer.asset = Asset::makeAny(warped->image, layer.name);
            layer.transform = warped->transform;
            layer.transform.sampling = raster.sampling;
            if (layer.mask && !layer.mask->placement) layer.mask->placement = raster;
        } else if (!instance.locked() && !keptFiltered) {
            const LayerTransform raster = layer.transform;
            const AnyImage placedContents = nativeMode ? smartObjectSourceImage(*source->second, placeTarget) : AnyImage();
            inMode = bool(placedContents);
            layer.asset = Asset::makeAny(inMode ? placedContents : smartObjectSourceImage(*source->second, sampleType), layer.name);
            layer.transform = *placed;
            layer.transform.sampling = raster.sampling;
            if (layer.mask && !layer.mask->placement) layer.mask->placement = raster;
        }
        if (!warped && (keptFiltered || instance.locked()) && placement->filtered && mode == RGB && depth == 8 && layer.asset->image.u8()) {
            // Photoshop's pixels for a filtered instance are the filters' result before the filter mask; its
            // merged image shows them through the mask over the unfiltered instance the cache record holds.
            const LayerTransform raster = layer.transform;
            const PlacedRaster stored{std::make_shared<Image>(*layer.asset->image.u8()), int(std::lround(raster.origin.x)),
                                      int(std::lround(raster.origin.y))};
            if (raster.rotation == 0 && !raster.flipX && !raster.flipY && raster.size.width == stored.image->width()
                && raster.size.height == stored.image->height())
                if (auto masked = applySmartFilterMask(docCarry->globals, instance, stored)) {
                    layer.asset = Asset::make(masked->image, layer.name);
                    layer.transform = LayerTransform(Point(masked->x, masked->y), Size(masked->image->width(), masked->image->height()));
                    layer.transform.sampling = raster.sampling;
                    if (layer.mask && !layer.mask->placement) layer.mask->placement = raster;
                    filterMasked = true;
                }
        }
        instance.placedTransform = layer.transform;
        instance.placedWidth = layer.asset->image.width();
        instance.placedHeight = layer.asset->image.height();
        layer.smartImage = layer.asset->image;
        layer.smartObject = std::move(instance);
        smartInMode = inMode;
        if (layer.smartObject->locked()) lockedSmartObjects[smartObjectLockDescription(layer.smartObject->lock)]++;
    }

    /// CMYK and Lab: the layer's pixels in the document's mode. What was read from the record's planes is
    /// assembled as stored (the RGB image above only stood in for the checks); anything drawn here in RGB (a
    /// solid fill, a smart object's contents, an empty type layer's pixel) is converted from sRGB. True when the
    /// pixels are the record's planes.
    bool toDocumentLayout(Layer& layer, const Record& rec, const RecordPlanes& planes, const std::shared_ptr<Image>& image, bool smartInMode) const {
        bool planesRead = false;
        if (nativeMode && layer.asset && layer.asset->image && !smartInMode) {
            const bool live = layer.text && layer.textImage == layer.asset->image, smart = layer.smartObject && layer.smartImage == layer.asset->image;
            if (image && layer.asset->image.u8() == image && !planes.planes.empty()) {
                layer.asset = Asset::makeAny(deep && !planes.widePlanes.empty() ? assembleMode16(mode, rec.width(), rec.height(), planes.widePlanes)
                                                                                : assembleMode8(mode, rec.width(), rec.height(), planes.planes), layer.asset->name);
                planesRead = true;
            } else if (layer.asset->image.channels() == 4) {
                const AnyImage converted = convertImage(layer.asset->image, ColorMode::RGB, ColorProfile(), colorMode, document.profile);
                if (converted) layer.asset = Asset::makeAny(converted, layer.asset->name);
            }
            if (live) layer.textImage = layer.asset->image;
            if (smart) layer.smartImage = layer.asset->image;
        }
        return planesRead;
    }

    // ---- the merged image ------------------------------------------------------------------------------------

    /// The merged image's planes as decoded, one per channel.
    struct MergedPlanes {
        std::vector<std::vector<uint8_t>> decoded;
        std::vector<std::vector<uint16_t>> decodedWide;
        std::vector<std::vector<float>> decodedFloat;
        std::vector<std::vector<uint8_t>> raw16;
        int colourPlanes = 0, firstExtra = 0;
        bool extrasWanted = false;
        const uint8_t* data = nullptr;
        size_t available = 0;
    };

    /// The merged image: every channel one after another, one compression for all.
    void readMergedImage() {
        MergedPlanes m;
        if (decodeMerged(m)) {
            readExtraChannels(m);
            mergedComposites(m);
        } else notes.push_back("The merged image could not be read; only the layers were imported.");
    }

    bool decodeMerged(MergedPlanes& m) {
        const int compression = r.u16();
        const int planeCount = channels;
        const size_t bytesPer = size_t(depth) / 8, planeBytes = size_t(width) * height * bytesPer;
        m.data = file.data() + r.position();
        m.available = file.size() - r.position();
        const uint8_t* data = m.data;
        const size_t available = m.available;
        const size_t planeTotal = static_cast<size_t>(planeCount);
        m.decoded.resize(planeTotal);
        m.decodedWide.resize(planeTotal);
        m.decodedFloat.resize(planeTotal);
        // The alpha and spot channels' planes follow the colour (and the transparency): their 16-bit samples as stored too.
        m.colourPlanes = mode == RGB || mode == Lab ? 3 : mode == CMYK ? 4 : 1;
        m.firstExtra = m.colourPlanes + (transparencyFirst ? 1 : 0);
        m.extrasWanted = mode != Bitmap && mode != Multichannel;
        m.raw16.resize(planeTotal);
        auto rawFor = [&](int c) { return deep && m.extrasWanted && c >= m.firstExtra ? &m.raw16[size_t(c)] : nullptr; };
        bool ok = true;
        if (compression == 0) {
            for (int c = 0; c < planeCount && ok; c++) {
                if (size_t(c + 1) * planeBytes > available) { ok = false; break; }
                std::string why;
                ok = decodePlane(data + size_t(c) * planeBytes, planeBytes, 0, int(width), int(height), depth, psb, m.decoded[size_t(c)], &why, deep ? &m.decodedWide[size_t(c)] : nullptr, rawFor(c),
                                 float32 ? &m.decodedFloat[size_t(c)] : nullptr);
            }
        } else if (compression == 1) {
            // Row byte counts for every channel first, then the rows.
            Reader counts(data, available);
            // The counts must be in the file before anything is sized by them (a hostile file claimed gigabytes).
            if (size_t(planeCount) * height * (psb ? 4 : 2) > available) { ok = false; }
            std::vector<size_t> rows(ok ? size_t(planeCount) * height : 0);
            for (size_t i = 0; i < rows.size(); i++) rows[i] = psb ? counts.u32() : counts.u16();
            size_t offset = counts.position();
            for (int c = 0; c < planeCount && ok; c++) {
                size_t total = 0;
                for (uint32_t y = 0; y < height; y++) total += rows[size_t(c) * height + y];
                if (offset + total > available) { ok = false; break; }
                // A channel's rows as their own RLE stream: its own counts, then its data.
                std::vector<uint8_t> stream;
                stream.reserve(total + height * (psb ? 4 : 2));
                for (uint32_t y = 0; y < height; y++) { size_t n = rows[size_t(c) * height + y]; if (psb) { stream.push_back(uint8_t(n >> 24)); stream.push_back(uint8_t(n >> 16)); } stream.push_back(uint8_t(n >> 8)); stream.push_back(uint8_t(n)); }
                if (offset + total > available) { ok = false; break; }
                stream.insert(stream.end(), data + offset, data + offset + total);
                offset += total;
                std::string why;
                ok = decodePlane(stream.data(), stream.size(), 1, int(width), int(height), depth, psb, m.decoded[size_t(c)], &why, deep ? &m.decodedWide[size_t(c)] : nullptr, rawFor(c),
                                 float32 ? &m.decodedFloat[size_t(c)] : nullptr);
            }
        } else ok = false;
        return ok;
    }

    /// The alpha and spot channels, from the planes after the colour (and the transparency).
    void readExtraChannels(MergedPlanes& m) {
        const int planeCount = channels;
        if (!m.extrasWanted || planeCount <= m.firstExtra) return;
        std::vector<PsdExtraPlane> extras;
        for (int c = m.firstExtra; c < planeCount; c++)
            extras.push_back({std::move(m.decoded[size_t(c)]), std::move(m.decodedWide[size_t(c)]), std::move(m.raw16[size_t(c)])});
        document.channels = psdChannels(channelResources, extras, transparencyFirst, int(width), int(height), deep);
        // A 32-bit file's channels in float (coverage, not linearised): one channel for each plane read.
        if (float32)
            for (size_t i = 0; i < document.channels.size() && size_t(m.firstExtra) + i < m.decodedFloat.size(); i++) {
                const std::vector<float>& plane = m.decodedFloat[size_t(m.firstExtra) + i];
                if (plane.size() != size_t(width) * height) continue;
                auto gray = std::make_shared<GrayF>(int(width), int(height));
                std::copy(plane.begin(), plane.end(), gray->data());
                cleanFloat(*gray);
                document.channels[i].image = GrayFPtr(gray);
            }
        size_t spots = 0;
        for (const nekophoto::Channel& ch : document.channels) spots += ch.kind == ChannelKind::Spot;
        if (spots) notes.push_back(std::to_string(spots) + " spot channel(s) are kept and shown in the Channels panel, but not edited.");
    }

    /// Photoshop's flattened image at every depth the document has, the white matte taken out.
    void mergedComposites(MergedPlanes& m) {
        const int planeCount = channels;
        const uint8_t* data = m.data;
        const size_t available = m.available;
        std::map<int, std::vector<uint8_t>> planes;
        const int colourChannels = m.colourPlanes;
        std::map<int, std::vector<uint16_t>> widePlanes;
        for (int c = 0; c < planeCount; c++) {
            const int id = c < colourChannels ? c : c == colourChannels && transparencyFirst ? -1 : -9;
            if (id == -9) continue;
            planes[id] = std::move(m.decoded[size_t(c)]);
            if (deep) widePlanes[id] = std::move(m.decodedWide[size_t(c)]);
        }
        // The same at 16 bits, the white matte taken out likewise.
        if (deep && widePlanes.count(0)) {
            if (auto alpha = widePlanes.find(-1); alpha != widePlanes.end())
                for (auto& [id, plane] : widePlanes) {
                    if (id < 0 || plane.size() != alpha->second.size()) continue;
                    const int64_t paper = mode == Lab && id > 0 ? 16384 : int64_t(one16);
                    for (size_t i = 0; i < plane.size(); i++) {
                        const int64_t A = alpha->second[i];
                        plane[i] = A == 0 ? 0 : uint16_t(std::clamp<int64_t>((int64_t(plane[i]) - (paper * (int64_t(one16) - A) + int64_t(one16) / 2) / int64_t(one16)) * int64_t(one16) / A, 0, one16));
                    }
                }
            if (!nativeMode) result.composite16 = assemble16(mode, int(width), int(height), widePlanes);
        }
        // In float from a 32-bit file, the white matte taken out likewise (colour above 1 kept).
        if (float32) {
            std::map<int, std::vector<float>> floatPlanes;
            for (int c = 0; c < planeCount; c++) {
                const int id = c < colourChannels ? c : c == colourChannels && transparencyFirst ? -1 : -9;
                if (id != -9) floatPlanes[id] = std::move(m.decodedFloat[size_t(c)]);
            }
            if (auto alpha = floatPlanes.find(-1); alpha != floatPlanes.end())
                for (auto& [id, plane] : floatPlanes) {
                    if (id < 0 || plane.size() != alpha->second.size()) continue;
                    for (size_t i = 0; i < plane.size(); i++) {
                        const float A = cleanCoverage(alpha->second[i]);
                        plane[i] = A <= 0 ? 0.0f : std::max(0.0f, (cleanColour(plane[i]) - (1 - A)) / A);
                    }
                }
            if (floatPlanes.count(0)) result.compositeF = assembleF(mode, int(width), int(height), floatPlanes);
        }
        if (mode == Bitmap) {
            // One bit per pixel, 1 = black, rows padded to bytes.
            std::vector<uint8_t> gray(size_t(width) * height);
            const size_t rowBytes = (width + 7) / 8;
            if (available >= rowBytes * height) for (uint32_t y = 0; y < height; y++) for (uint32_t x = 0; x < width; x++) gray[size_t(y) * width + x] = (data[y * rowBytes + x / 8] >> (7 - x % 8)) & 1 ? 0 : 255;
            planes.clear(); planes[0] = std::move(gray);
        }
        // With transparency, Photoshop stores the merged colour matted against white: take the white out (Lab's
        // a and b are matted against neutral, 128).
        if (auto alpha = planes.find(-1); alpha != planes.end() && mode != Bitmap && mode != Indexed) {
            const std::vector<uint8_t>& a = alpha->second;
            for (auto& [id, plane] : planes) {
                if (id < 0 || plane.size() != a.size()) continue;
                const int paper = mode == Lab && id > 0 ? 128 : 255;
                for (size_t i = 0; i < plane.size(); i++) {
                    const int A = a[i];
                    plane[i] = A == 0 ? 0 : uint8_t(std::clamp((int(plane[i]) - (paper * (255 - A) + 127) / 255) * 255 / A, 0, 255));
                }
            }
        }
        if (nativeMode) result.compositeNative = deep && !widePlanes.empty() ? assembleMode16(mode, int(width), int(height), widePlanes)
                                                                             : assembleMode8(mode, int(width), int(height), planes);
        result.composite = assemble(mode == Bitmap ? Grayscale : mode, int(width), int(height), planes, palette, cmykProfile.get());
    }

    // ---- the document ----------------------------------------------------------------------------------------

    /// The document as a whole: a file without layers as its merged image, the slices and guides, and the one depth
    /// and layout throughout.
    bool finish(std::string* error) {
        std::vector<Layer>& layers = document.layers;
        if (layers.empty()) {
            if (!result.composite) { if (error) *error = "The file has neither layers nor a readable merged image."; return false; }
            Layer background(result.compositeF ? Asset::make(result.compositeF, "Background")
                             : result.compositeNative ? Asset::makeAny(result.compositeNative, "Background")
                             : result.composite16 ? Asset::make(result.composite16, "Background") : Asset::make(result.composite, "Background"), Point(0, 0));
            background.name = "Background";
            layers.push_back(background);
            if (options.mergedOnly) notes.push_back(result.realComposite ? "Opened as the merged image Photoshop stored, without its layers." : "Opened as the merged image, but the file was saved without Maximize Compatibility, so Photoshop stored a blank stand-in.");
            // A file saved flattened simply opens as its one layer: nothing to note.
        }
        for (const auto& resource : docCarry->resources)
            if (resource.id == 1050) parseSlicesResource(resource.data, document.slices);
            else if (resource.id == 1032) parseGuidesResource(resource.data, document.guides);
        if (!docCarry->resources.empty() || !docCarry->globals.empty()) document.psdCarry = docCarry;
        if (deep) {
            // What was not read from 16-bit planes (fills, smart object contents, text drawn anew) is widened, so the
            // document holds one depth throughout.
            std::string why;
            if (!convertSampleType(document, SampleType::U16, &why)) { if (error) *error = why; return false; }
        }
        if (float32) {
            // A 32-bit file's values are linear in its profile's primaries (resource 1039, which Photoshop writes with its
            // working space's own curve): the document holds the linear version and remembers the file's for 16 and 8
            // bits, and the PSD it writes. Untagged is linear sRGB. What was not read from 32-bit planes (fills, text
            // drawn anew, the alpha channels) is linearised through that curve.
            if (!isLinearProfile(document.profile)) {
                document.encodedProfile = document.profile;
                document.profile = linearProfile(document.profile);
            }
            if (std::string problem = sampleTypeBudgetProblem(document, SampleType::F32); !problem.empty()) { if (error) *error = problem; return false; }
            document.sampleType = SampleType::F32;
            conformToFormat(document);
        }
        if (nativeMode) {
            // One layout throughout (a raster held at another depth widened), and the Layers panel's thumbnails drawn
            // through the document's profile.
            conformToFormat(document);
            refreshModeThumbnails(document);
        }
        return true;
    }
};

} // namespace

std::optional<PsdImport> importPsdBytes(const std::vector<uint8_t>& file, std::string* error, const PsdImportOptions& options) {
    try {
        return PsdReader(file, options).read(error);
    } catch (Truncated&) {
        if (error) *error = "The file ends early or has a structure this reader does not understand.";
        return std::nullopt;
    } catch (const std::bad_alloc&) {
        // A size in the file (a layer, a channel, a compressed stream) larger than memory allows.
        if (error) *error = "The file asks for more memory than is available.";
        return std::nullopt;
    } catch (const std::length_error&) {
        if (error) *error = "The file asks for more memory than is available.";
        return std::nullopt;
    }
}

} // namespace nekophoto
