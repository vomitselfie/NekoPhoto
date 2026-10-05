// CMYK and Lab PSDs (docs/high-bit-depth-plan.md, P7 step D), on constructed files: documents built here at 8 and 16
// bits (two layers, a layer mask, a Levels adjustment layer, a spot channel), written by NekoPhoto's PSD writer and
// read back by a small independent decoder in this file (header, layer records, channel data raw or PackBits, the
// merged image), then by NekoPhoto's reader. These are constructed files, a weaker check than files Photoshop saved:
// the only Photoshop-saved CMYK file in the corpus is Patchy's photoshop-cmyk-style-colors.psd (psd_roundtrip).
#include "check.h"
#include "compositor/adjustments.h"
#include "compositor/channels.h"
#include "compositor/colormgmt.h"
#include "compositor/depth.h"
#include "compositor/document.h"
#include "compositor/psd.h"
#include "compositor/psd_carry.h"
#include "compositor/psd_writer.h"
#include "compositor/render.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <zlib.h>

using namespace compositor;

namespace {

// ---- A reference decoder -------------------------------------------------------------------------------------------

struct In {
    const std::vector<uint8_t>& b;
    size_t p = 0;
    uint32_t u8() { if (p >= b.size()) throw std::runtime_error("short"); return b[p++]; }
    uint32_t u16() { uint32_t v = u8() << 8; return v | u8(); }
    uint32_t u32() { uint32_t v = u16() << 16; return v | u16(); }
    std::string key() { std::string s; for (int i = 0; i < 4; i++) s += char(u8()); return s; }
    void skip(size_t n) { if (n > b.size() - p) throw std::runtime_error("short"); p += n; }
};

/// One plane: raw, PackBits rows (16-bit counts), or zlib (2) and zlib over row deltas (3, 16-bit), `bytesPer` bytes
/// a sample, as big-endian values. `size` is the plane's stored size for the zlib forms.
std::vector<uint32_t> decodePlane(In& in, int compression, int w, int h, int bytesPer, const uint16_t* counts = nullptr, size_t size = 0) {
    std::vector<uint8_t> bytes;
    const size_t rowBytes = size_t(w) * size_t(bytesPer);
    if (compression == 2 || compression == 3) {
        bytes.resize(rowBytes * size_t(h));
        uLongf out = uLongf(bytes.size());
        if (uncompress(bytes.data(), &out, in.b.data() + in.p, uLong(size)) != Z_OK || out != bytes.size()) throw std::runtime_error("zlib");
        in.skip(size);
        if (compression == 3 && bytesPer == 2)
            for (int y = 0; y < h; y++) {
                uint8_t* row = bytes.data() + size_t(y) * rowBytes;
                for (int x = 1; x < w; x++) {
                    const uint16_t v = uint16_t((uint16_t(row[x * 2 - 2] << 8 | row[x * 2 - 1]) + uint16_t(row[x * 2] << 8 | row[x * 2 + 1])));
                    row[x * 2] = uint8_t(v >> 8); row[x * 2 + 1] = uint8_t(v);
                }
            }
    } else if (compression == 0) {
        bytes.assign(in.b.begin() + long(in.p), in.b.begin() + long(in.p + rowBytes * size_t(h)));
        in.skip(rowBytes * size_t(h));
    } else {
        std::vector<uint16_t> own;
        if (!counts) { for (int y = 0; y < h; y++) own.push_back(uint16_t(in.u16())); counts = own.data(); }
        for (int y = 0; y < h; y++) {
            const size_t end = in.p + counts[y];
            std::vector<uint8_t> row;
            while (in.p < end) {
                const int n = int8_t(in.u8());
                if (n >= 0) for (int i = 0; i <= n; i++) row.push_back(uint8_t(in.u8()));
                else if (n != -128) { const uint8_t v = uint8_t(in.u8()); for (int i = 0; i < 1 - n; i++) row.push_back(v); }
            }
            row.resize(rowBytes);
            bytes.insert(bytes.end(), row.begin(), row.end());
        }
    }
    std::vector<uint32_t> out(size_t(w) * size_t(h));
    for (size_t i = 0; i < out.size(); i++) out[i] = bytesPer == 2 ? uint32_t(bytes[i * 2]) << 8 | bytes[i * 2 + 1] : bytes[i];
    return out;
}

struct RefLayer {
    std::string name;
    int top = 0, left = 0, bottom = 0, right = 0;
    std::string blend;
    std::map<std::string, std::vector<uint8_t>> blocks;
    std::map<int, std::vector<uint32_t>> planes;   // channel id -> samples (masks at the mask's size)
    std::map<int, std::vector<uint8_t>> stored;    // channel id -> the channel's bytes as stored
    int maskTop = 0, maskLeft = 0, maskBottom = 0, maskRight = 0;
};

struct RefFile {
    int channels = 0, width = 0, height = 0, depth = 0, mode = 0;
    std::map<int, std::vector<uint8_t>> resources;
    std::vector<RefLayer> layers;
    std::vector<std::vector<uint32_t>> merged;
};

RefFile decode(const std::vector<uint8_t>& file) {
    RefFile f;
    In in{file};
    if (in.key() != "8BPS" || in.u16() != 1) throw std::runtime_error("not a PSD");
    in.skip(6);
    f.channels = int(in.u16()); f.height = int(in.u32()); f.width = int(in.u32()); f.depth = int(in.u16()); f.mode = int(in.u16());
    in.skip(in.u32());
    const size_t resEnd = in.p + in.u32() + 4;
    while (in.p + 12 <= resEnd) {
        if (in.key() != "8BIM") break;
        const int id = int(in.u16());
        const uint32_t nameLen = in.u8();
        in.skip(nameLen + ((nameLen + 1) & 1));
        const uint32_t len = in.u32();
        f.resources[id].assign(file.begin() + long(in.p), file.begin() + long(in.p + len));
        in.skip(len + (len & 1));
    }
    in.p = resEnd;
    const int bytesPer = f.depth / 8;
    const size_t layerEnd = in.p + in.u32() + 4;
    auto readLayers = [&](In& li) {
        const int count = std::abs(int(int16_t(li.u16())));
        std::vector<std::vector<std::pair<int, uint32_t>>> lengths;
        for (int i = 0; i < count; i++) {
            RefLayer l;
            l.top = int(int32_t(li.u32())); l.left = int(int32_t(li.u32())); l.bottom = int(int32_t(li.u32())); l.right = int(int32_t(li.u32()));
            const int n = int(li.u16());
            std::vector<std::pair<int, uint32_t>> ch;
            for (int c = 0; c < n; c++) { const int id = int(int16_t(li.u16())); ch.push_back({id, li.u32()}); }
            lengths.push_back(ch);
            li.key(); l.blend = li.key();
            li.skip(4);
            const size_t extraEnd = li.p + li.u32() + 4;
            const uint32_t maskLen = li.u32();
            if (maskLen >= 16) { l.maskTop = int(int32_t(li.u32())); l.maskLeft = int(int32_t(li.u32())); l.maskBottom = int(int32_t(li.u32())); l.maskRight = int(int32_t(li.u32())); li.skip(maskLen - 16); }
            else li.skip(maskLen);
            li.skip(li.u32());
            const uint32_t nameLen = li.u8();
            for (uint32_t k = 0; k < nameLen; k++) l.name += char(li.u8());
            li.skip((4 - (nameLen + 1) % 4) % 4);
            while (li.p + 12 <= extraEnd) {
                if (li.key() != "8BIM") break;
                const std::string key = li.key();
                const uint32_t len = li.u32();
                l.blocks[key].assign(file.begin() + long(li.p), file.begin() + long(li.p + len));
                li.skip(len);
            }
            li.p = extraEnd;
            f.layers.push_back(std::move(l));
        }
        for (size_t i = 0; i < f.layers.size(); i++) {
            RefLayer& l = f.layers[i];
            for (auto [id, len] : lengths[i]) {
                const size_t start = li.p;
                l.stored[id].assign(file.begin() + long(start), file.begin() + long(start + len));
                const int compression = int(li.u16());
                const int w = id == -2 ? l.maskRight - l.maskLeft : l.right - l.left, h = id == -2 ? l.maskBottom - l.maskTop : l.bottom - l.top;
                if (w > 0 && h > 0) l.planes[id] = decodePlane(li, compression, w, h, bytesPer, nullptr, len - 2);
                li.p = start + len;
            }
        }
    };
    const uint32_t infoLen = in.u32();
    if (infoLen) readLayers(in);
    if (f.depth == 16) {
        // An empty layer information, the global mask info, then 'Lr16'.
        In g{file, 0};
        g.p = resEnd + 4;
        g.skip(g.u32());   // layer info (empty)
        g.skip(g.u32());   // global layer mask info
        while (g.p + 12 <= layerEnd) {
            if (g.key() != "8BIM") break;
            const std::string key = g.key();
            const uint32_t len = g.u32();
            const size_t start = g.p;
            if (key == "Lr16") { In li{file, start}; readLayers(li); }
            g.p = start + len;
            while ((g.p - start) % 4) g.p++;
        }
    }
    in.p = layerEnd;
    const int compression = int(in.u16());
    std::vector<uint16_t> counts;
    if (compression == 1) for (int i = 0; i < f.channels * f.height; i++) counts.push_back(uint16_t(in.u16()));
    for (int c = 0; c < f.channels; c++) f.merged.push_back(decodePlane(in, compression, f.width, f.height, bytesPer, compression ? counts.data() + size_t(c) * size_t(f.height) : nullptr));
    return f;
}

// ---- The constructed documents -------------------------------------------------------------------------------------

/// A CMYK or Lab document at `type`: an opaque background, a translucent layer with a mask, a Levels layer and a
/// spot channel. The pixels are premultiplied; `straight` holds what the file must store for the opaque one.
Document constructed(ColorMode mode, SampleType type) {
    const int w = 24, h = 16;
    Document doc(w, h);
    doc.colorMode = mode;
    const int n = colorModeChannels(mode);
    auto bottom8 = std::make_shared<ImageC8>(w, h, n), top8 = std::make_shared<ImageC8>(12, 8, n);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t* p = bottom8->pixel(x, y);
            for (int c = 0; c < n - 1; c++) p[c] = uint8_t((x * 11 + y * 7 + c * 50) % 256);
            p[n - 1] = 255;
        }
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 12; x++) {
            uint8_t* p = top8->pixel(x, y);
            const unsigned a = 64 + unsigned(x * 16);
            for (int c = 0; c < n - 1; c++) p[c] = uint8_t(((c * 90 + y * 20) % 256) * a / 255);
            p[n - 1] = uint8_t(a);
        }
    auto asAny = [&](const std::shared_ptr<ImageC8>& img) -> AnyImage {
        if (mode == ColorMode::CMYK) return ImageC8Ptr(img);
        auto rgbLike = std::make_shared<Image>(img->width(), img->height());
        std::memcpy(rgbLike->data(), img->data(), img->byteCount());
        return ImagePtr(rgbLike);
    };
    doc.layers.push_back(Layer(Asset::makeAny(asAny(bottom8), "Background"), Point(0, 0)));
    Layer top(Asset::makeAny(asAny(top8), "Top"), Point(6, 4));
    top.blendMode = BlendMode::Multiply;
    LayerMask mask;
    auto maskImage = std::make_shared<GrayImage>(12, 8, 255);
    for (int x = 0; x < 6; x++) for (int y = 0; y < 8; y++) maskImage->at(x, y) = 0;
    mask.asset = MaskAsset::make(maskImage);
    top.mask = mask;
    doc.layers.push_back(top);
    Layer levels("Levels", doc.size());
    levels.asset.reset();
    AdjustmentSettings settings = AdjustmentSettings::defaults(AdjustmentKind::Levels);
    settings.levels.ranges[0].black = 20;
    levels.adjustment = settings.toLayerAdjustment();
    doc.layers.push_back(levels);
    Channel spot = makeAlphaChannel(doc, "PANTONE Test");
    spot.kind = ChannelKind::Spot;
    spot.color = {0.9, 0.1, 0.4};
    spot.opacity = 1.0;
    doc.channels.push_back(spot);
    if (mode == ColorMode::CMYK) doc.profile = defaultCmykProfile();
    if (type == SampleType::U16) { std::string e; if (!convertSampleType(doc, SampleType::U16, &e)) check::fail(__FILE__, __LINE__, e); }
    return doc;
}

uint32_t storedSample(const AnyImage& image, int x, int y, int c) {
    if (image.u16()) return to65535(image.u16()->pixel(x, y)[c]);
    if (image.c8()) return image.c8()->pixel(x, y)[c];
    return image.u8()->pixel(x, y)[c];
}

void checkMode(ColorMode mode, SampleType type) {
    const Document doc = constructed(mode, type);
    std::string error;
    PsdExportSummary summary;
    const std::vector<uint8_t> bytes = encodePsd(doc, PsdExportOptions(), &summary, &error);
    REQUIRE(!bytes.empty());
    RefFile f;
    try { f = decode(bytes); } catch (std::exception& e) { check::fail(__FILE__, __LINE__, std::string("reference decoder: ") + e.what()); return; }
    const int colours = colorModeColorChannels(mode);
    CHECK_EQ(f.mode, mode == ColorMode::CMYK ? 4 : 9);
    CHECK_EQ(f.depth, type == SampleType::U16 ? 16 : 8);
    CHECK_EQ(f.channels, colours + 1 + 1);   // colour, transparency, the spot channel
    CHECK_EQ(int(f.merged.size()), f.channels);
    CHECK_EQ(mode == ColorMode::CMYK, f.resources.count(1039) > 0);
    if (mode == ColorMode::CMYK && f.resources.count(1039)) CHECK(f.resources.at(1039) == defaultCmykProfile().icc);
    REQUIRE(f.layers.size() == 3);
    // The background: opaque, so each plane is the stored sample exactly.
    const RefLayer& bg = f.layers[0];
    for (int c = 0; c < colours; c++) {
        REQUIRE(bg.planes.count(c));
        int wrong = 0;
        for (int y = 0; y < 16; y++) for (int x = 0; x < 24; x++) wrong += bg.planes.at(c)[size_t(y) * 24 + size_t(x)] != storedSample(doc.layers[0].asset->image, x, y, c);
        CHECK_EQ(wrong, 0);
    }
    // The top layer: its rectangle, the mask channel, Multiply.
    const RefLayer& top = f.layers[1];
    CHECK_EQ(top.left, 6); CHECK_EQ(top.top, 4); CHECK_EQ(top.right, 18); CHECK_EQ(top.bottom, 12);
    CHECK(top.planes.count(-1) && top.planes.count(-2));
    CHECK(top.planes.count(colours - 1));
    CHECK_EQ(top.blend, std::string("mul "));
    // The adjustment layer as Photoshop's own block.
    CHECK(f.layers[2].blocks.count("levl"));
    // The merged image: opaque over the canvas where the background is.
    CHECK_EQ(f.merged[size_t(colours)][0], uint32_t(type == SampleType::U16 ? 65535 : 255));

    // NekoPhoto's reader: the same mode, depth, pixels, mask, adjustment and spot channel.
    auto imported = importPsdBytes(bytes, &error);
    REQUIRE(imported);
    const Document& back = imported->document;
    CHECK(back.colorMode == mode);
    CHECK(back.sampleType == type);
    REQUIRE(back.layers.size() == 3);
    CHECK_EQ(back.layers[0].asset->image.channels(), colorModeChannels(mode));
    CHECK_EQ(psdContentHash(back.layers[0].asset->image), psdContentHash(doc.layers[0].asset->image));
    CHECK(back.layers[1].mask.has_value());
    CHECK(back.layers[2].adjustment && back.layers[2].adjustment->kind == AdjustmentKind::Levels);
    REQUIRE(back.channels.size() == 1);
    CHECK(back.channels[0].kind == ChannelKind::Spot);
    CHECK(imported->compositeNative);
    if (mode == ColorMode::CMYK) CHECK(back.profile.icc == defaultCmykProfile().icc);
    // Untouched layers go back byte for byte: every layer channel of a second export is the first's.
    const std::vector<uint8_t> again = encodePsd(back, PsdExportOptions(), &summary, &error);
    REQUIRE(!again.empty());
    const RefFile g = decode(again);
    REQUIRE(g.layers.size() == f.layers.size());
    for (size_t i = 0; i < f.layers.size(); i++)
        for (auto& [id, data] : f.layers[i].stored)
            if (id >= -1) CHECK(g.layers[i].stored.count(id) && g.layers[i].stored.at(id) == data);
}

} // namespace

TEST_CASE(constructed_cmyk_8) { checkMode(ColorMode::CMYK, SampleType::U8); }
TEST_CASE(constructed_cmyk_16) { checkMode(ColorMode::CMYK, SampleType::U16); }
TEST_CASE(constructed_lab_8) { checkMode(ColorMode::Lab, SampleType::U8); }
TEST_CASE(constructed_lab_16) { checkMode(ColorMode::Lab, SampleType::U16); }

TEST_CASE(photoshop_cmyk_fixture_opens_as_cmyk) {
    // Patchy's Photoshop-saved CMYK file, when the checkout is beside this one.
    const char* dir = std::getenv("PATCHY_FIXTURES");
    const std::string path = std::string(dir ? dir : PATCHY_FIXTURES) + "/photoshop-cmyk-style-colors.psd";
    std::string error;
    auto imported = importPsd(path, &error);
    if (!imported) { std::printf("  skipped: %s\n", error.c_str()); return; }
    CHECK(imported->document.colorMode == ColorMode::CMYK);
    CHECK(!imported->document.layers.empty());
    for (const Layer& l : imported->document.layers) if (l.asset && l.asset->image) CHECK_EQ(l.asset->image.channels(), 5);
    CHECK(imported->document.profile.model == ColorModel::CMYK);
    // Our render of its layers against Photoshop's merged image (both shown in sRGB): close, not exact (layer styles
    // are not drawn in CMYK yet).
    if (imported->composite && imported->realComposite) {
        Image ours;
        render(imported->document, RenderOptions(), ours);
        double sum = 0;
        const Image& ps = *imported->composite;
        for (int y = 0; y < ps.height(); y++) for (int x = 0; x < ps.width() * 4; x++) sum += std::abs(int(ps.row(y)[x]) - int(ours.row(y)[x]));
        std::printf("  mean difference from Photoshop's merged image: %.2f levels\n", sum / (ps.width() * ps.height() * 4.0));
    }
}

TEST_CASE(a_cmyk_layers_fingerprint_sees_every_sample) {
    // What decides that a layer is unchanged since it was read (its stored channels then go back byte for byte) sees
    // every sample: an 8-bit CMYK layer's (once always 0, so an edit was written as the pixels first read) and the last
    // fifth of a 16-bit CMYK layer's rows.
    auto c8 = std::make_shared<ImageC8>(8, 4, 5);
    const uint64_t blank = psdContentHash(AnyImage(ImageC8Ptr(c8)));
    CHECK(blank != 0);
    auto painted = std::make_shared<ImageC8>(*c8);
    painted->pixel(2, 1)[3] = 9;
    CHECK(psdContentHash(AnyImage(ImageC8Ptr(painted))) != blank);
    auto deep = std::make_shared<Image16>(10, 2, 5);
    const uint64_t before = psdContentHash(AnyImage(Image16Ptr(deep)));
    auto edited = std::make_shared<Image16>(*deep);
    edited->pixel(9, 1)[4] = 77;   // the last pixel's alpha: past four fifths of the row
    CHECK(psdContentHash(AnyImage(Image16Ptr(edited))) != before);
}

TEST_MAIN()
