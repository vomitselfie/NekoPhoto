// The G'MIC filter catalogue: definition files written here in the forms gmic.eu serves (G'MIC 3.4 on:
// compressed, folders nested by underscores, author subfolders, marked controls and previews).
#include "check.h"
#include "Gmic.h"
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QStandardPaths>

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
    CHECK(glow->commandLine(false) == "someone_glow 5");
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

TEST_MAIN()
