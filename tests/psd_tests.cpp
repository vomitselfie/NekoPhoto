// The PSD importer against files written here: raw and RLE channels, folders, clipping, masks, blend
// modes, adjustment layers, unicode names, and the merged image.
#include "check.h"
#include "compositor/psd.h"
#include "compositor/adjustments.h"
#include "compositor/render.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace compositor;

namespace {

/// A little PSD writer: big-endian fields appended to a byte vector.
struct Out {
    std::vector<uint8_t> b;
    void u8(unsigned v) { b.push_back(uint8_t(v)); }
    void u16(unsigned v) { u8(v >> 8); u8(v); }
    void u32(uint32_t v) { u16(v >> 16); u16(v & 0xffff); }
    void i32(int32_t v) { u32(uint32_t(v)); }
    void str(const std::string& s) { b.insert(b.end(), s.begin(), s.end()); }
    void pascal4(const std::string& s) { u8(unsigned(s.size())); str(s); size_t used = 1 + s.size(); while (used % 4) { u8(0); used++; } }
    void bytes(const std::vector<uint8_t>& v) { b.insert(b.end(), v.begin(), v.end()); }
};

/// PackBits with plain literal runs (no repeats): enough to exercise the decoder's row table.
std::vector<uint8_t> packBits(const std::vector<uint8_t>& row) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i < row.size(); i += 128) {
        size_t n = std::min<size_t>(128, row.size() - i);
        out.push_back(uint8_t(n - 1));
        out.insert(out.end(), row.begin() + long(i), row.begin() + long(i + n));
    }
    return out;
}

struct LayerSpec {
    std::string name;
    int left, top, w, h;
    std::vector<std::vector<uint8_t>> planes;   // channel ids in `ids`
    std::vector<int> ids;
    std::string blend = "norm";
    unsigned opacity = 255, clipping = 0, flags = 0;
    int section = 0;
    bool rle = false;
    bool mask = false; int maskLeft = 0, maskTop = 0, maskW = 0, maskH = 0; unsigned maskDefault = 255; std::vector<uint8_t> maskPlane;
    std::vector<uint8_t> extraBlock; std::string extraKey;
    std::string unicodeName;
};

std::vector<uint8_t> channelData(const LayerSpec& l, const std::vector<uint8_t>& plane, int w, int h) {
    Out o;
    if (!l.rle) { o.u16(0); o.bytes(plane); return o.b; }
    o.u16(1);
    std::vector<std::vector<uint8_t>> rows;
    for (int y = 0; y < h; y++) rows.push_back(packBits(std::vector<uint8_t>(plane.begin() + y * w, plane.begin() + (y + 1) * w)));
    for (auto& r : rows) o.u16(unsigned(r.size()));
    for (auto& r : rows) o.bytes(r);
    return o.b;
}

std::vector<uint8_t> writePsd(int width, int height, const std::vector<LayerSpec>& layers, const std::vector<std::vector<uint8_t>>& compositePlanes) {
    Out f;
    f.str("8BPS"); f.u16(1); for (int i = 0; i < 6; i++) f.u8(0);
    f.u16(unsigned(compositePlanes.size())); f.u32(uint32_t(height)); f.u32(uint32_t(width)); f.u16(8); f.u16(3);
    f.u32(0);   // colour mode data
    // Image resources: the resolution (0x03ED) at 144 ppi.
    Out res;
    res.str("8BIM"); res.u16(0x03ED); res.u8(0); res.u8(0); res.u32(16); res.u32(144 << 16); res.u16(1); res.u16(1); res.u32(144 << 16); res.u16(1); res.u16(1);
    f.u32(uint32_t(res.b.size())); f.bytes(res.b);
    // Layer records and their channel data.
    Out records;
    records.u16(uint16_t(int16_t(-int(layers.size()))));
    std::vector<uint8_t> channelBytes;
    for (const LayerSpec& l : layers) {
        records.i32(l.top); records.i32(l.left); records.i32(l.top + l.h); records.i32(l.left + l.w);
        std::vector<std::pair<int, std::vector<uint8_t>>> chans;
        for (size_t i = 0; i < l.ids.size(); i++) chans.push_back({l.ids[i], channelData(l, l.planes[i], l.w, l.h)});
        if (l.mask) chans.push_back({-2, channelData(l, l.maskPlane, l.maskW, l.maskH)});
        records.u16(unsigned(chans.size()));
        for (auto& [id, data] : chans) { records.u16(uint16_t(int16_t(id))); records.u32(uint32_t(data.size())); channelBytes.insert(channelBytes.end(), data.begin(), data.end()); }
        records.str("8BIM"); records.str(l.blend); records.u8(l.opacity); records.u8(l.clipping); records.u8(l.flags); records.u8(0);
        Out extra;
        if (l.mask) { extra.u32(20); extra.i32(l.maskTop); extra.i32(l.maskLeft); extra.i32(l.maskTop + l.maskH); extra.i32(l.maskLeft + l.maskW); extra.u8(l.maskDefault); extra.u8(0); extra.u16(0); }
        else extra.u32(0);
        extra.u32(0);   // blending ranges
        extra.pascal4(l.name);
        if (l.section) { extra.str("8BIM"); extra.str("lsct"); extra.u32(4); extra.u32(uint32_t(l.section)); }
        if (!l.unicodeName.empty()) { extra.str("8BIM"); extra.str("luni"); Out u; u.u32(uint32_t(l.unicodeName.size())); for (char c : l.unicodeName) u.u16(uint8_t(c)); extra.u32(uint32_t(u.b.size())); extra.bytes(u.b); }
        if (!l.extraKey.empty()) { extra.str("8BIM"); extra.str(l.extraKey); extra.u32(uint32_t(l.extraBlock.size())); extra.bytes(l.extraBlock); }
        records.u32(uint32_t(extra.b.size())); records.bytes(extra.b);
    }
    std::vector<uint8_t> layerInfo = records.b;
    layerInfo.insert(layerInfo.end(), channelBytes.begin(), channelBytes.end());
    if (layerInfo.size() % 2) layerInfo.push_back(0);
    Out lm;
    lm.u32(uint32_t(layerInfo.size())); lm.bytes(layerInfo);
    lm.u32(0);   // global layer mask info
    f.u32(uint32_t(lm.b.size())); f.bytes(lm.b);
    // The merged image, raw.
    f.u16(0);
    for (auto& plane : compositePlanes) f.bytes(plane);
    return f.b;
}

std::vector<uint8_t> solid(int w, int h, uint8_t v) { return std::vector<uint8_t>(size_t(w) * h, v); }

std::string writeTemp(const std::vector<uint8_t>& bytes, const char* name) {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "compositor-psd-tests";
    std::filesystem::create_directories(dir);
    std::filesystem::path file = dir / name;
    std::ofstream out(file, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), long(bytes.size()));
    return file.string();
}

} // namespace

TEST_CASE(psd_layers_folders_masks_and_blends_come_through) {
    const int W = 8, H = 6;
    std::vector<LayerSpec> layers;
    // Bottom: a red background over the whole canvas, RLE.
    LayerSpec back{"Background", 0, 0, W, H, {solid(W, H, 255), solid(W, H, 0), solid(W, H, 0)}, {0, 1, 2}};
    back.rle = true;
    layers.push_back(back);
    // A folder's end marker, then two members, then the folder record.
    LayerSpec divider{"</Layer group>", 0, 0, 0, 0, {}, {}}; divider.section = 3;
    layers.push_back(divider);
    LayerSpec blue{"Blue", 2, 1, 4, 3, {solid(4, 3, 0), solid(4, 3, 0), solid(4, 3, 255), solid(4, 3, 128)}, {0, 1, 2, -1}};
    blue.blend = "mul "; blue.opacity = 200;
    blue.mask = true; blue.maskLeft = 2; blue.maskTop = 1; blue.maskW = 2; blue.maskH = 3; blue.maskDefault = 255; blue.maskPlane = solid(2, 3, 0);
    blue.unicodeName = "Bl\xc3\xbc";   // "Blü" in UTF-8 for the expectation; the writer stores code points below
    blue.unicodeName = "Blue layer";
    layers.push_back(blue);
    LayerSpec clipped{"Clipped", 0, 0, W, H, {solid(W, H, 0), solid(W, H, 255), solid(W, H, 0)}, {0, 1, 2}};
    clipped.clipping = 1; clipped.blend = "lbrn"; clipped.flags = 2;   // hidden, and a blend mode without a counterpart
    layers.push_back(clipped);
    LayerSpec folder{"Group A", 0, 0, 0, 0, {}, {}}; folder.section = 1; folder.opacity = 128;
    layers.push_back(folder);
    // A Levels adjustment layer (version 2, 29 records; the first four matter).
    LayerSpec levels{"Levels 1", 0, 0, 0, 0, {}, {}};
    Out lv; lv.u16(2);
    for (int i = 0; i < 29; i++) { lv.u16(i == 0 ? 20 : 0); lv.u16(i == 0 ? 235 : 255); lv.u16(0); lv.u16(255); lv.u16(i == 0 ? 120 : 100); }
    levels.extraKey = "levl"; levels.extraBlock = lv.b;
    layers.push_back(levels);
    std::vector<std::vector<uint8_t>> composite{solid(W, H, 255), solid(W, H, 0), solid(W, H, 0)};
    std::string path = writeTemp(writePsd(W, H, layers, composite), "layers.psd");

    std::string error;
    auto imported = importPsd(path, &error);
    REQUIRE(imported.has_value());
    const Document& doc = imported->document;
    CHECK_EQ(doc.width, W); CHECK_EQ(doc.height, H);
    CHECK_NEAR(doc.resolution, 144, 1e-9);
    // Background, folder, Blue, Clipped, Levels: five layers, the divider consumed.
    REQUIRE(doc.layers.size() == 5u);
    const Layer& background = doc.layers[0];
    CHECK_EQ(background.name, std::string("Background"));
    CHECK_EQ(int(background.asset->image->pixel(3, 3)[0]), 255);
    const Layer& group = doc.layers[1];
    CHECK(group.isGroup);
    CHECK_EQ(group.name, std::string("Group A"));
    CHECK_NEAR(group.opacity, 128 / 255.0, 1e-9);
    const Layer& blueLayer = doc.layers[2];
    CHECK_EQ(blueLayer.name, std::string("Blue layer"));
    CHECK(blueLayer.parentId == std::optional<Uuid>(group.id));
    CHECK(blueLayer.blendMode == BlendMode::Multiply);
    CHECK_NEAR(blueLayer.opacity, 200 / 255.0, 1e-9);
    CHECK_NEAR(blueLayer.transform.origin.x, 2, 1e-9);
    CHECK_NEAR(blueLayer.transform.origin.y, 1, 1e-9);
    CHECK_EQ(blueLayer.asset->image->width(), 4);
    CHECK_EQ(int(blueLayer.asset->image->pixel(0, 0)[3]), 128);       // alpha from channel -1
    CHECK_EQ(int(blueLayer.asset->image->pixel(0, 0)[2]), 128);       // premultiplied blue
    REQUIRE(blueLayer.mask.has_value());
    CHECK_EQ(int(blueLayer.mask->asset.image->at(0, 0)), 0);           // inside the mask rect
    CHECK_EQ(int(blueLayer.mask->asset.image->at(3, 0)), 255);         // beyond it: the default
    const Layer& clippedLayer = doc.layers[3];
    CHECK(clippedLayer.maskSourceId == std::optional<Uuid>(blueLayer.id));
    CHECK(!clippedLayer.visible);
    CHECK(clippedLayer.blendMode == BlendMode::ColorBurn);            // the nearest to Linear Burn
    CHECK(clippedLayer.parentId == std::optional<Uuid>(group.id));
    const Layer& levelsLayer = doc.layers[4];
    REQUIRE(levelsLayer.adjustment.has_value());
    CHECK(levelsLayer.adjustment->kind == AdjustmentKind::Levels);
    AdjustmentSettings parsed;
    REQUIRE(AdjustmentSettings::parse(levelsLayer.adjustment->json, parsed));
    CHECK_NEAR(parsed.levels.ranges[0].black, 20, 1e-9);
    CHECK_NEAR(parsed.levels.ranges[0].white, 235, 1e-9);
    CHECK_NEAR(parsed.levels.ranges[0].gamma, 1.2, 1e-9);
    CHECK(!levelsLayer.parentId.has_value());
    // The merged image came along, and a note explains the blend mode.
    REQUIRE(imported->composite != nullptr);
    CHECK_EQ(int(imported->composite->pixel(1, 1)[0]), 255);
    bool blendNote = false;
    for (auto& n : imported->notes) if (n.find("Linear Burn") != std::string::npos) blendNote = true;
    CHECK(blendNote);
    // The imported document renders.
    auto flat = renderFlattened(doc);
    CHECK_EQ(int(flat->pixel(0, 5)[0]), 255);
}

TEST_CASE(psd_without_layers_becomes_one_background_layer) {
    const int W = 5, H = 4;
    std::vector<std::vector<uint8_t>> composite{solid(W, H, 10), solid(W, H, 200), solid(W, H, 30)};
    std::string path = writeTemp(writePsd(W, H, {}, composite), "flat.psd");
    std::string error;
    auto imported = importPsd(path, &error);
    REQUIRE(imported.has_value());
    REQUIRE(imported->document.layers.size() == 1u);
    CHECK_EQ(imported->document.layers[0].name, std::string("Background"));
    CHECK_EQ(int(imported->document.layers[0].asset->image->pixel(2, 2)[1]), 200);
    CHECK(!imported->notes.empty());
    // Not a PSD at all.
    std::string bogus = writeTemp({'h', 'e', 'l', 'l', 'o'}, "bogus.psd");
    CHECK(!importPsd(bogus, &error).has_value());
    CHECK(!error.empty());
}

TEST_CASE(psd_sample_file_when_available) {
    // COMPOSITOR_PSD_SAMPLE=path: a real Photoshop file; its layers must render close to its own merged image.
    const char* sample = std::getenv("COMPOSITOR_PSD_SAMPLE");
    if (!sample) { std::fprintf(stderr, "  (skipped: set COMPOSITOR_PSD_SAMPLE to a .psd to run)\n"); return; }
    std::string error;
    auto imported = importPsd(sample, &error);
    REQUIRE(imported.has_value());
    std::fprintf(stderr, "  %zu layers, %zu notes\n", imported->document.layers.size(), imported->notes.size());
    for (auto& n : imported->notes) std::fprintf(stderr, "    - %s\n", n.c_str());
    REQUIRE(imported->composite != nullptr);
    auto flat = renderFlattened(imported->document);
    double total = 0; long n = 0; int worst = 0;
    for (int y = 0; y < flat->height(); y += 3)
        for (int x = 0; x < flat->width(); x += 3)
            for (int c = 0; c < 3; c++) { int d = std::abs(int(flat->pixel(x, y)[c]) - int(imported->composite->pixel(x, y)[c])); total += d; n++; worst = std::max(worst, d); }
    std::fprintf(stderr, "  mean difference from Photoshop's merged image %.2f levels, worst %d\n", total / n, worst);
    CHECK(total / n < 8);
}

TEST_MAIN()
