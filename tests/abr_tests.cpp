// Photoshop .abr import: versions 2 and 6 written here byte by byte, and a real file when
// COMPOSITOR_ABR_SAMPLE names one.
#include "check.h"
#include "compositor/brushimport.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace compositor;
namespace fs = std::filesystem;

namespace {

struct Out {
    std::vector<uint8_t> b;
    void u8(unsigned v) { b.push_back(uint8_t(v)); }
    void u16(unsigned v) { u8(v >> 8); u8(v); }
    void u32(uint32_t v) { u16(v >> 16); u16(v & 0xFFFF); }
    void f64(double v) { uint64_t bits; std::memcpy(&bits, &v, 8); u32(uint32_t(bits >> 32)); u32(uint32_t(bits)); }
    void chars(const std::string& s) { b.insert(b.end(), s.begin(), s.end()); }
    void unicode(const std::string& s) { u32(uint32_t(s.size() + 1)); for (char c : s) u16(uint8_t(c)); u16(0); }
    void key(const std::string& k) { if (k.size() == 4) { u32(0); chars(k); } else { u32(uint32_t(k.size())); chars(k); } }
    void append(const Out& o) { b.insert(b.end(), o.b.begin(), o.b.end()); }
};

/// A 16x12 tip: a solid block in the right half, empty left half.
std::vector<uint8_t> tipPixels() {
    std::vector<uint8_t> p(16 * 12, 0);
    for (int y = 0; y < 12; y++) for (int x = 8; x < 16; x++) p[size_t(y * 16 + x)] = 255;
    return p;
}

/// The same rows PackBits-encoded (a literal run of 8 zeros, then a repeat of 8 x 255), counts first.
void packedTip(Out& o) {
    for (int y = 0; y < 12; y++) o.u16(4);
    for (int y = 0; y < 12; y++) { o.u8(uint8_t(-7)); o.u8(0); o.u8(uint8_t(-7)); o.u8(255); }
}

std::string write(const std::string& name, const Out& o) {
    fs::path path = fs::temp_directory_path() / name;
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(o.b.data()), std::streamsize(o.b.size()));
    return path.string();
}

} // namespace

TEST_CASE(abr_version_2_reads_computed_raw_and_packed_brushes) {
    Out f;
    f.u16(2); f.u16(3);
    // A computed brush: 40 px, round, soft.
    Out computed;
    computed.u32(0); computed.u16(30); computed.u16(40); computed.u16(100); computed.u16(0); computed.u16(50);
    f.u16(1); f.u32(uint32_t(computed.b.size())); f.append(computed);
    // A sampled brush, raw.
    Out raw;
    raw.u32(0); raw.u16(20); raw.unicode("Block"); raw.u8(1);
    for (int i = 0; i < 4; i++) raw.u16(0);
    raw.u32(0); raw.u32(0); raw.u32(12); raw.u32(16); raw.u16(8); raw.u8(0);
    for (uint8_t v : tipPixels()) raw.u8(v);
    f.u16(2); f.u32(uint32_t(raw.b.size())); f.append(raw);
    // The same, PackBits.
    Out packed;
    packed.u32(0); packed.u16(50); packed.unicode("Packed"); packed.u8(1);
    for (int i = 0; i < 4; i++) packed.u16(0);
    packed.u32(0); packed.u32(0); packed.u32(12); packed.u32(16); packed.u16(8); packed.u8(1);
    packedTip(packed);
    f.u16(2); f.u32(uint32_t(packed.b.size())); f.append(packed);
    std::string error;
    auto import = importBrushFile(write("v2-test.abr", f), &error);
    REQUIRE(import.has_value());
    REQUIRE(import->brushes.size() == 3);
    CHECK_EQ(import->brushes[0].diameter, 40.0);
    CHECK_EQ(import->brushes[0].tip.spacing, 0.3);
    CHECK(import->brushes[1].name == "Block");
    CHECK(import->brushes[2].name == "Packed");
    for (int i : {1, 2}) {
        const auto& shape = *import->brushes[size_t(i)].tip.shape;
        CHECK_EQ(shape.width(), 16);
        CHECK_EQ(shape.height(), 12);
        CHECK_EQ(int(shape.at(3, 5)), 0);
        CHECK_EQ(int(shape.at(12, 5)), 255);
    }
    CHECK_EQ(import->brushes[2].tip.spacing, 0.5);
}

TEST_CASE(abr_version_6_ties_presets_to_their_samples_and_reads_dynamics) {
    const std::string uuid = "11111111-2222-3333-4444-555555555555";
    // samp: one entry, bounds 301 bytes after its start (the version 6.2 layout).
    Out entry;
    entry.u8(uint8_t(uuid.size())); entry.chars(uuid);
    while (entry.b.size() < 301) entry.u8(0);
    entry.u32(0); entry.u32(0); entry.u32(12); entry.u32(16); entry.u16(8); entry.u8(1);
    packedTip(entry);
    Out samp;
    samp.u32(uint32_t(entry.b.size())); samp.append(entry);
    while (samp.b.size() % 4) samp.u8(0);
    // desc: one preset using that sample, with pressure on size and scatter.
    Out d;
    d.u32(16); d.unicode(""); d.key("null"); d.u32(1);
    d.key("Brsh"); d.chars("VlLs"); d.u32(1);
    d.chars("Objc"); d.unicode(""); d.key("brushPreset"); d.u32(8);
    d.key("Nm  "); d.chars("TEXT"); d.unicode("Leaf");
    d.key("Brsh"); d.chars("Objc"); d.unicode(""); d.key("sampledBrush"); d.u32(6);
    d.key("Dmtr"); d.chars("UntF"); d.chars("#Pxl"); d.f64(64);
    d.key("Angl"); d.chars("UntF"); d.chars("#Ang"); d.f64(30);
    d.key("Rndn"); d.chars("UntF"); d.chars("#Prc"); d.f64(80);
    d.key("Spcn"); d.chars("UntF"); d.chars("#Prc"); d.f64(40);
    d.key("Intr"); d.chars("bool"); d.u8(1);
    d.key("sampledData"); d.chars("TEXT"); d.unicode(uuid);
    d.key("useTipDynamics"); d.chars("bool"); d.u8(1);
    d.key("szVr"); d.chars("Objc"); d.unicode(""); d.key("brVr"); d.u32(2);
    d.key("bVTy"); d.chars("long"); d.u32(2);
    d.key("jitter"); d.chars("UntF"); d.chars("#Prc"); d.f64(20);
    d.key("minimumDiameter"); d.chars("UntF"); d.chars("#Prc"); d.f64(25);
    d.key("useScatter"); d.chars("bool"); d.u8(1);
    d.key("scatterDynamics"); d.chars("Objc"); d.unicode(""); d.key("brVr"); d.u32(1);
    d.key("jitter"); d.chars("UntF"); d.chars("#Prc"); d.f64(150);
    d.key("Cnt "); d.chars("doub"); d.f64(3);
    Out f;
    f.u16(6); f.u16(2);
    f.chars("8BIM"); f.chars("samp"); f.u32(uint32_t(samp.b.size())); f.append(samp);
    f.chars("8BIM"); f.chars("desc"); f.u32(uint32_t(d.b.size())); f.append(d);
    std::string error;
    auto import = importBrushFile(write("v6-test.abr", f), &error);
    REQUIRE(import.has_value());
    REQUIRE(import->brushes.size() == 1);   // the one sample is used by the preset, so no extra tip
    const TipPreset& leaf = import->brushes[0];
    CHECK(leaf.name == "Leaf");
    CHECK_EQ(leaf.diameter, 64.0);
    CHECK_EQ(leaf.tip.angle, 30.0);
    CHECK_EQ(leaf.tip.roundness, 0.8);
    CHECK_EQ(leaf.tip.spacing, 0.4);
    CHECK_EQ(leaf.tip.pressureSize, 1.0);
    CHECK_EQ(leaf.tip.minimumSize, 0.25);
    CHECK_EQ(leaf.tip.sizeJitter, 0.2);
    CHECK_EQ(leaf.tip.scatter, 1.5);
    CHECK_EQ(leaf.tip.count, 3);
    REQUIRE(leaf.tip.shape != nullptr);
    CHECK_EQ(int(leaf.tip.shape->at(12, 5)), 255);
    // A damaged file is refused, not read past its end.
    Out cut;
    cut.b.assign(f.b.begin(), f.b.begin() + 200);
    CHECK(!importBrushFile(write("v6-cut.abr", cut), &error).has_value());
}

TEST_CASE(abr_sample_file_when_available) {
    const char* sample = std::getenv("COMPOSITOR_ABR_SAMPLE");
    if (!sample) { std::fprintf(stderr, "  (skipped: set COMPOSITOR_ABR_SAMPLE to a .abr file)\n"); return; }
    std::string error;
    auto import = importBrushFile(sample, &error);
    REQUIRE(import.has_value());
    std::fprintf(stderr, "  %zu brushes, %zu notes\n", import->brushes.size(), import->notes.size());
    CHECK(!import->brushes.empty());
    for (const auto& b : import->brushes) CHECK(b.tip.shape && b.tip.shape->width() > 0);
}

TEST_MAIN()
