// The G'MIC filter catalogue: definition files written here in the forms gmic.eu serves (G'MIC 3.4 on:
// compressed, folders nested by underscores, author subfolders, marked controls and previews).
#include "check.h"
#include "Gmic.h"
#include "compositor/depth.h"
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

using namespace app;

namespace {

const char* definitions =
    "#@gmic\n"
    "#@gui _<b>Artistic</b>\n"
    "#@gui Painting:fx_painting,fx_painting_preview(1)+\n"
    "#@gui :Radius=~float(2.5,0,10)_1\n"
    "#@gui :Mode=choice{1,\"Soft\",\"Hard\"}\n"
    "#@gui :Refresh=button(0)_0+\n"
    "#@gui :_=link(\"Author\",\"https://example.org\")\n"
    "#@gui :_=separator()\n"
    "#@gui _<b>Testing</b>\n"
    "#@gui <i>Someone</i>\n"
    "#@gui Glow:someone_glow,someone_glow_preview(0)*\n"
    "#@gui :Amount=int(5,0,10)\n"
    "#@gui :_=value(50,30)\n"
    "#@gui __<b>Colors</b>\n"
    "#@gui Tint:someone_tint,someone_tint\n"
    "#@gui :Colour=color(255,0,0)\n"
    "#@gui :Outline=~color(#00000080)\n"
    "#@gui Load CLUT:fx_load_clut,fx_load_clut\n"
    "#@gui :File=file(\"\")\n";

QString write(const QString& name, const QByteArray& data) {
    const QString path = QDir::temp().filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
    f.write(data);
    return path;
}

void checkCatalogue(const GmicCatalogue& c) {
    REQUIRE(c.filters().size() == 3);   // the file picker's filter is left out
    const GmicFilter* painting = nullptr; const GmicFilter* glow = nullptr; const GmicFilter* tint = nullptr;
    for (const auto& f : c.filters()) {
        if (f.name == "Painting") painting = &f;
        if (f.name == "Glow") glow = &f;
        if (f.name == "Tint") tint = &f;
    }
    REQUIRE(painting && glow && tint);
    CHECK(painting->folder == "Artistic");
    CHECK(painting->previewCommand == "fx_painting_preview");
    CHECK(painting->commandLine(false) == "fx_painting 2.5,1,0");
    CHECK(glow->folder == "Testing / Someone");
    CHECK(glow->commandLine(false) == "someone_glow 5,50,30");   // a value() passes everything in it
    CHECK(tint->folder == "Testing / Colors");
    CHECK(tint->commandLine(false) == "someone_tint 255,0,0,0,0,0,128");   // the hex colour carries its alpha
}

} // namespace

TEST_CASE(gmic_catalogue_reads_marked_controls_folders_and_authors) {
    GmicCatalogue c;
    QString error;
    REQUIRE(c.load(write("compositor-test-catalogue.gmic", definitions), &error));
    checkCatalogue(c);
}

TEST_CASE(gmic_catalogue_reads_the_compressed_file_gmic_eu_serves) {
    const QByteArray text(definitions);
    QByteArray packed = qCompress(text).mid(4);   // qCompress prefixes the length; the file does not
    QByteArray file = "1 uint8 little_endian\n1 " + QByteArray::number(text.size()) + " 1 1 #" + QByteArray::number(packed.size()) + "\n" + packed;
    GmicCatalogue c;
    QString error;
    REQUIRE(c.load(write("compositor-test-catalogue-z.gmic", file), &error));
    checkCatalogue(c);
    // Something that only looks like one is refused, not read as no filters.
    CHECK(!c.load(write("compositor-test-catalogue-bad.gmic", "1 uint8 little_endian\n1 99 1 1 #3\nxyz"), &error));
}

TEST_CASE(gmic_patch_based_commands_are_never_offered_or_run) {
    // The legal boundary (docs/legal-boundaries.md, "G'MIC"): patch-match and patch-based inpainting, and anything
    // defined on top of them, are left out of the browser and refused by every run path.
    const QByteArray text =
        "#@gui _<b>Repair</b>\n"
        "#@gui Inpaint Patch:fx_my_inpaint,fx_my_inpaint\n"
        "#@gui :Size=int(5,1,10)\n"
        "#@gui Smooth:fx_my_smooth,fx_my_smooth\n"
        "#@gui :Size=int(5,1,10)\n"
        "fx_my_inpaint :\n"
        "  _fx_my_helper $1\n"
        "_fx_my_helper :\n"
        "  matchpatch[0] [1],$1\n"
        "fx_my_smooth :\n"
        "  blur $1\n";
    const QSet<QString> excluded = GmicCatalogue::excludedCommands(text);
    CHECK(excluded.contains("matchpatch"));
    CHECK(excluded.contains("_fx_my_helper"));
    CHECK(excluded.contains("fx_my_inpaint"));   // through the helper
    CHECK(!excluded.contains("fx_my_smooth"));
    GmicCatalogue c;
    REQUIRE(c.load(write("compositor-test-catalogue-patch.gmic", text)));
    REQUIRE(c.filters().size() == 1);
    CHECK(c.filters()[0].command == "fx_my_smooth");
    // The base set holds whatever the definition file: typed commands and automation are refused.
    QString why;
    CHECK(!GmicRunner::allowedForAutomation("inpaint_patch 7", &why));
    CHECK(!GmicCatalogue::excludedIn("blur 3 matchpatch[0] [1]").isEmpty());
    CHECK(GmicCatalogue::excludedIn("blur 3").isEmpty());
    compositor::Image tiny(4, 4);
    QString error;
    CHECK(!GmicRunner::runSync(tiny, "+matchpatch[0] [0],3", &error));
    CHECK(error.contains("matchpatch"));
    // The real definition file, when one is installed: report how much it leaves out.
    if (const QString path = GmicCatalogue::preferredFile(); !path.isEmpty()) {
        GmicCatalogue real;
        if (real.load(path)) {
            std::printf("  %s: %d filters offered, %d commands excluded\n", qPrintable(path), int(real.filters().size()), int(GmicCatalogue::excluded().size()));
            for (const GmicFilter& f : real.filters()) CHECK(GmicCatalogue::excludedIn(f.command).isEmpty());
        }
    }
}

TEST_CASE(gmic_at_sixteen_bits_agrees_with_eight_bits_on_eight_bit_input) {
    if (GmicRunner::executable().isEmpty()) { std::fprintf(stderr, "  skipped: no gmic executable\n"); return; }
    // An 8-bit picture (ramps, texture, and a half-transparent corner or not) and the same pixels widened to 16 bits.
    auto picture = [](bool corner) {
        compositor::Image image(48, 32);
        uint32_t noise = 99;
        for (int y = 0; y < 32; y++)
            for (int x = 0; x < 48; x++) {
                noise = noise * 1664525U + 1013904223U;
                const int a = corner && x < 12 && y < 8 ? 100 + x * 8 : 255;
                const int r = x * 255 / 47, g = y * 255 / 31, b = int((noise >> 24) % 256);
                uint8_t* p = image.pixel(x, y);
                p[0] = uint8_t((r * a + 127) / 255); p[1] = uint8_t((g * a + 127) / 255); p[2] = uint8_t((b * a + 127) / 255); p[3] = uint8_t(a);
            }
        return image;
    };
    // Within a level, but Solarize over half-transparent pixels: the 8-bit run hands G'MIC their straight colour
    // rounded to whole levels (the PNG), the 16-bit run the unrounded colour, and Solarize, which scales by the whole
    // image's range, carries that into every pixel (6 levels on 0.9% of samples; none on an opaque picture).
    struct Case { const char* command; bool corner; int allowed; };
    const Case cases[] = {{"blur 1.5", true, 1}, {"unsharp 1.5,2", true, 1}, {"sepia", true, 1}, {"sharpen 40", true, 1},
                          {"solarize", false, 1}, {"solarize", true, 6}};
    bool within = true;
    for (const Case& k : cases) {
        const compositor::Image source = picture(k.corner);
        const auto deep = compositor::widenImage(source);
        QString error8, error16;
        auto eight = GmicRunner::runSync(source, k.command, &error8);
        auto sixteen = GmicRunner::runSync(*deep, k.command, &error16);
        REQUIRE(eight && sixteen);
        auto narrowed = compositor::narrowImage(*sixteen);
        int worst = 0;
        long long over1 = 0, samples = 0;
        for (int y = 0; y < 32; y++)
            for (int x = 0; x < 48; x++)
                for (int c = 0; c < 4; c++) {
                    const int d = std::abs(int(eight->pixel(x, y)[c]) - int(narrowed->pixel(x, y)[c]));
                    worst = std::max(worst, d);
                    over1 += d > 1;
                    samples++;
                }
        std::fprintf(stderr, "  %-14s %-12s max %d level%s, over 1: %lld of %lld samples\n", k.command, k.corner ? "(corner)" : "(opaque)", worst,
                     worst == 1 ? "" : "s", over1, samples);
        within &= worst <= k.allowed;
    }
    CHECK(within);
    // 16-bit precision survives the round trip: a ramp finer than 8 bits through an identity command keeps its values.
    compositor::Image16 ramp(512, 2);
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 512; x++) { uint16_t* p = ramp.pixel(x, y); p[0] = p[1] = p[2] = uint16_t(8000 + x * 13); p[3] = 32768; }
    QString error;
    auto same = GmicRunner::runSync(ramp, "mul 1", &error);
    REQUIRE(same);
    int drift = 0;
    for (int x = 0; x < 512; x++) drift = std::max(drift, std::abs(int(same->pixel(x, 1)[0]) - int(ramp.pixel(x, 1)[0])));
    CHECK(drift <= 1);
}

TEST_MAIN()
