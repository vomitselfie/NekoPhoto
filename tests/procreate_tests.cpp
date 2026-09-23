// Procreate import: a .brushset and a .brush written here byte by byte (a stored ZIP, an XML brushset.plist,
// an NSKeyedArchiver binary plist, PNG shapes), and a real file when COMPOSITOR_PROCREATE_SAMPLE names one.
#include "check.h"
#include "compositor/brushimport.h"
#include "compositor/png.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <variant>
#include <vector>

using namespace compositor;
namespace fs = std::filesystem;

namespace {

uint32_t crc32(const std::vector<uint8_t>& data) {
    uint32_t crc = 0xFFFFFFFF;
    for (uint8_t b : data) {
        crc ^= b;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

/// A ZIP of stored (uncompressed) files.
std::vector<uint8_t> zip(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files) {
    std::vector<uint8_t> out, directory;
    auto le16 = [](std::vector<uint8_t>& v, unsigned x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); };
    auto le32 = [&](std::vector<uint8_t>& v, uint32_t x) { le16(v, x & 0xFFFF); le16(v, x >> 16); };
    for (const auto& [name, data] : files) {
        const uint32_t offset = uint32_t(out.size()), crc = crc32(data), size = uint32_t(data.size());
        le32(out, 0x04034b50); le16(out, 20); le16(out, 0); le16(out, 0); le32(out, 0); le32(out, crc); le32(out, size); le32(out, size);
        le16(out, unsigned(name.size())); le16(out, 0);
        out.insert(out.end(), name.begin(), name.end());
        out.insert(out.end(), data.begin(), data.end());
        le32(directory, 0x02014b50); le16(directory, 20); le16(directory, 20); le16(directory, 0); le16(directory, 0); le32(directory, 0);
        le32(directory, crc); le32(directory, size); le32(directory, size); le16(directory, unsigned(name.size())); le16(directory, 0); le16(directory, 0);
        le16(directory, 0); le16(directory, 0); le32(directory, 0); le32(directory, offset);
        directory.insert(directory.end(), name.begin(), name.end());
    }
    const uint32_t start = uint32_t(out.size());
    out.insert(out.end(), directory.begin(), directory.end());
    le32(out, 0x06054b50); le16(out, 0); le16(out, 0); le16(out, unsigned(files.size())); le16(out, unsigned(files.size()));
    le32(out, uint32_t(directory.size())); le32(out, start); le16(out, 0);
    return out;
}

/// A keyed archive of one root dictionary with number and string settings, as a binary plist.
std::vector<uint8_t> keyedArchive(const std::map<std::string, std::variant<double, std::string>>& settings) {
    // Objects: 0 top dict, 1 "$objects" array, 2 "$null", 3 root dict, then keys and values; the keys of the top.
    std::vector<std::vector<uint8_t>> objects;
    auto add = [&](std::vector<uint8_t> o) { objects.push_back(std::move(o)); return uint8_t(objects.size() - 1); };
    auto ascii = [&](const std::string& s) { std::vector<uint8_t> o{uint8_t(0x50 | std::min<size_t>(s.size(), 15))}; if (s.size() >= 15) { o.push_back(0x10); o.push_back(uint8_t(s.size())); } o.insert(o.end(), s.begin(), s.end()); return add(o); };
    auto real = [&](double v) { uint64_t bits; std::memcpy(&bits, &v, 8); std::vector<uint8_t> o{0x23}; for (int k = 7; k >= 0; k--) o.push_back(uint8_t(bits >> (8 * k))); return add(o); };
    auto uid = [&](uint8_t v) { return add({0x80, v}); };
    add({});   // 0: top, filled at the end
    add({});   // 1: $objects, filled at the end
    const uint8_t null = ascii("$null");
    const uint8_t root = add({});   // filled below
    std::vector<uint8_t> keys, values;
    std::vector<uint8_t> archivedStrings;   // $objects entries for string settings
    for (const auto& [key, value] : settings) {
        keys.push_back(ascii(key));
        if (std::holds_alternative<double>(value)) values.push_back(real(std::get<double>(value)));
        else { const uint8_t s = ascii(std::get<std::string>(value)); archivedStrings.push_back(s); values.push_back(uid(uint8_t(1 + archivedStrings.size()))); }
    }
    auto dict = [](const std::vector<uint8_t>& k, const std::vector<uint8_t>& v) {
        std::vector<uint8_t> o{uint8_t(0xD0 | std::min<size_t>(k.size(), 15))};
        if (k.size() >= 15) { o.push_back(0x10); o.push_back(uint8_t(k.size())); }
        o.insert(o.end(), k.begin(), k.end());
        o.insert(o.end(), v.begin(), v.end());
        return o;
    };
    objects[root] = dict(keys, values);
    // $objects: [$null, root, the string values...]; UIDs index this array.
    std::vector<uint8_t> list{uint8_t(0xA0 | (2 + archivedStrings.size()))};
    list.push_back(null);
    list.push_back(root);
    for (uint8_t s : archivedStrings) list.push_back(s);
    objects[1] = list;
    const uint8_t topKey = ascii("$top"), objectsKey = ascii("$objects"), archiverKey = ascii("$archiver"), archiver = ascii("NSKeyedArchiver");
    const uint8_t rootKey = ascii("root"), rootUid = uid(1);
    const uint8_t topDict = add(dict({rootKey}, {rootUid}));
    objects[0] = dict({topKey, objectsKey, archiverKey}, {topDict, 1, archiver});
    std::vector<uint8_t> out{'b', 'p', 'l', 'i', 's', 't', '0', '0'};
    std::vector<uint32_t> offsets;
    for (const auto& o : objects) { offsets.push_back(uint32_t(out.size())); out.insert(out.end(), o.begin(), o.end()); }
    const uint32_t table = uint32_t(out.size());
    for (uint32_t off : offsets) for (int k = 3; k >= 0; k--) out.push_back(uint8_t(off >> (8 * k)));
    std::vector<uint8_t> trailer(32, 0);
    trailer[6] = 4; trailer[7] = 1;
    auto be64 = [&](size_t at, uint64_t v) { for (int k = 7; k >= 0; k--) trailer[at + size_t(7 - k)] = uint8_t(v >> (8 * k)); };
    be64(8, objects.size()); be64(16, 0); be64(24, table);
    out.insert(out.end(), trailer.begin(), trailer.end());
    return out;
}

/// A shape: white bars on Procreate's near-black (20) background.
std::vector<uint8_t> shapePng() {
    Image image(32, 32);
    image.fill(20, 20, 20, 255);
    for (int y = 0; y < 32; y++) for (int x = 20; x < 28; x++) { uint8_t* p = image.pixel(x, y); p[0] = p[1] = p[2] = 255; }
    std::vector<uint8_t> bytes;
    encodePngImage(image, bytes);
    return bytes;
}

std::string write(const std::string& name, const std::vector<uint8_t>& bytes) {
    fs::path path = fs::temp_directory_path() / name;
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    return path.string();
}

} // namespace

TEST_CASE(procreate_brushset_reads_its_brushes_in_order_with_their_settings) {
    const std::string list = R"(<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict><key>brushes</key><array><string>BBBB</string><string>AAAA</string></array><key>name</key><string>Test &amp; Set</string></dict></plist>)";
    const auto archiveA = keyedArchive({{"name", std::string("Soft Ink")}, {"plotSpacing", 0.08}, {"plotJitter", 0.5}, {"shapeRotation", 1.0},
                                        {"shapeScatter", 0.5}, {"shapeCount", 0.25}, {"dynamicsPressureSize", 0.75}, {"maxSize", 0.1}, {"maxOpacity", 0.8}});
    const auto archiveB = keyedArchive({{"name", std::string("Bars")}, {"plotSpacing", 0.2}, {"maxSize", 0.5}, {"bundledGrainPath", std::string("paper")}});
    const auto file = zip({{"brushset.plist", std::vector<uint8_t>(list.begin(), list.end())},
                           {"AAAA/Brush.archive", archiveA}, {"AAAA/Shape.png", shapePng()},
                           {"AAAA/Reset/Brush.archive", archiveB},
                           {"BBBB/Brush.archive", archiveB}, {"BBBB/Shape.png", shapePng()}});
    std::string error;
    auto import = importBrushFile(write("test.brushset", file), &error);
    REQUIRE(import.has_value());
    CHECK(import->set == "Test & Set");
    REQUIRE(import->brushes.size() == 2);
    CHECK(import->brushes[0].name == "Bars");   // the list's order, not the archive's
    const TipPreset& ink = import->brushes[1];
    CHECK(ink.name == "Soft Ink");
    CHECK_EQ(ink.tip.spacing, 0.08);
    CHECK_EQ(ink.tip.scatter, 0.5);
    CHECK(ink.tip.followStroke);
    CHECK_EQ(ink.tip.angleJitter, 90.0);
    CHECK_EQ(ink.tip.count, 4);
    CHECK_EQ(ink.tip.pressureSize, 1.0);
    CHECK_EQ(ink.tip.minimumSize, 0.25);
    CHECK_EQ(ink.tip.flow, 0.8);
    CHECK_EQ(ink.diameter, 20.0);
    // White paints; the near-black background is taken off.
    REQUIRE(ink.tip.shape != nullptr);
    CHECK_EQ(int(ink.tip.shape->at(5, 5)), 0);
    CHECK_EQ(int(ink.tip.shape->at(24, 5)), 255);
    CHECK(!import->notes.empty());   // Bars' grain is Procreate's own
}

TEST_CASE(a_single_procreate_brush_and_damaged_archives) {
    const auto archive = keyedArchive({{"name", std::string("Solo")}, {"plotSpacing", 0.1}});
    std::string error;
    auto single = importBrushFile(write("solo.brush", zip({{"Brush.archive", archive}, {"Shape.png", shapePng()}})), &error);
    REQUIRE(single.has_value());
    REQUIRE(single->brushes.size() == 1);
    CHECK(single->brushes[0].name == "Solo");
    // A damaged plist, or a ZIP whose file does not match its checksum, imports nothing.
    std::vector<uint8_t> broken = archive;
    broken.resize(broken.size() - 10);
    CHECK(!importBrushFile(write("broken.brush", zip({{"Brush.archive", broken}})), &error).has_value());
    auto file = zip({{"Brush.archive", archive}});
    file[30 + 13 + 20] ^= 0xFF;   // inside the stored archive, past its 30-byte header and name
    CHECK(!importBrushFile(write("corrupt.brush", file), &error).has_value());
}

TEST_CASE(procreate_sample_file_when_available) {
    const char* sample = std::getenv("COMPOSITOR_PROCREATE_SAMPLE");
    if (!sample) { std::fprintf(stderr, "  (skipped: set COMPOSITOR_PROCREATE_SAMPLE to a .brushset file)\n"); return; }
    std::string error;
    auto import = importBrushFile(sample, &error);
    REQUIRE(import.has_value());
    std::fprintf(stderr, "  %s: %zu brushes, %zu notes\n", import->set.c_str(), import->brushes.size(), import->notes.size());
    CHECK(!import->brushes.empty());
}

TEST_MAIN()
