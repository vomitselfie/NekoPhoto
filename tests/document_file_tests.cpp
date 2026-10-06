// The single-file .nekophoto document: the ZIP container (zipfile.h) and the project round trip through it, against the
// .comp folder holding the same files (docs/project-format.md).
#include "check.h"
#include "compositor/colormgmt.h"
#include "compositor/png.h"
#include "compositor/project.h"
#include "compositor/smartobject.h"
#include "compositor/zipfile.h"
#include <filesystem>
#include <fstream>
#include <random>

using namespace compositor;
namespace fs = std::filesystem;

namespace {

fs::path tempDir() {
    std::mt19937 rng{std::random_device{}()};
    fs::path dir = fs::temp_directory_path() / ("nekophoto-document-" + std::to_string(rng()));
    fs::create_directories(dir);
    return dir;
}

std::vector<uint8_t> fileBytes(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// An 8-bit RGB document with a pixel layer, a mask, a group, an alpha channel and a smart object.
Document richDocument() {
    Document doc(48, 32);
    auto image = std::make_shared<Image>(40, 30);
    for (int y = 0; y < 30; y++)
        for (int x = 0; x < 40; x++) {
            uint8_t* p = image->pixel(x, y);
            p[3] = uint8_t(255 - (x * 3) % 256);
            p[0] = uint8_t(std::min<int>(p[3], x * 6));
            p[1] = uint8_t(std::min<int>(p[3], y * 8));
            p[2] = uint8_t(std::min<int>(p[3], (x * y) % 256));
        }
    Layer group("Group", doc.size());
    group.isGroup = true;
    Layer pixels(Asset::make(ImagePtr(image), "Pixels"), Point{3, 1});
    pixels.parentId = group.id;
    auto maskImage = std::make_shared<GrayImage>(40, 30, 255);
    for (int i = 0; i < 40; i++) maskImage->at(i, i % 30) = uint8_t(i * 5);
    LayerMask mask;
    mask.asset = MaskAsset::make(maskImage);
    pixels.mask = mask;
    doc.layers = {group, pixels};
    auto source = std::make_shared<SmartObjectSource>();
    source->id = "source-1";
    source->fileName = "Art.png";
    source->fileType = "png ";
    source->bytes = std::make_shared<const std::vector<uint8_t>>(500, uint8_t(7));
    source->image = std::make_shared<Image>(16, 16);
    source->width = source->height = 16;
    doc.smartObjects[source->id] = source;
    Channel alpha;
    alpha.id = makeUuid();
    alpha.name = "Alpha 1";
    auto gray = std::make_shared<GrayImage>(48, 32, 0);
    gray->at(5, 5) = 200;
    alpha.image = GrayPtr(gray);
    doc.channels.push_back(alpha);
    return doc;
}

/// Every file of the .comp folder is in the .nekophoto file under the same name, byte for byte.
void checkSameFiles(const fs::path& folder, const fs::path& file) {
    ZipFileReader zip;
    std::string why;
    REQUIRE(zip.openFile(file.string(), &why));
    int files = 0;
    for (auto& entry : fs::recursive_directory_iterator(folder)) {
        if (!entry.is_regular_file()) continue;
        const std::string name = fs::relative(entry.path(), folder).generic_string();
        const ZipFileEntry* inside = zip.find(name);
        CHECK(inside != nullptr);
        if (!inside) { std::fprintf(stderr, "    missing %s\n", name.c_str()); continue; }
        std::vector<uint8_t> bytes;
        CHECK(zip.read(*inside, bytes, &why));
        CHECK(bytes == fileBytes(entry.path()));
        files++;
    }
    CHECK(files > 1);
}

bool samePixels(const AnyImage& a, const AnyImage& b) {
    if (!a || !b) return !a && !b;
    if (a.width() != b.width() || a.height() != b.height() || a.sampleType() != b.sampleType() || a.channels() != b.channels()) return false;
    if (a.u8() && b.u8()) return *a.u8() == *b.u8();
    if (a.u16() && b.u16()) return *a.u16() == *b.u16();
    if (a.c8() && b.c8()) return *a.c8() == *b.c8();
    if (a.f32() && b.f32()) return *a.f32() == *b.f32();
    return false;
}

bool sameGray(const AnyGray& a, const AnyGray& b) {
    if (!a || !b) return !a && !b;
    if (a.u8() && b.u8()) return *a.u8() == *b.u8();
    if (a.u16() && b.u16()) return *a.u16() == *b.u16();
    if (a.f32() && b.f32()) return *a.f32() == *b.f32();
    return false;
}

/// Saves `doc` both ways, checks the files match, and that both load to the same pixels as `doc`.
void roundTrip(const Document& doc, const std::string& name) {
    const fs::path dir = tempDir();
    const fs::path folder = dir / (name + ".comp"), file = dir / (name + ".nekophoto");
    ProjectError error;
    REQUIRE(saveProject(doc, std::nullopt, folder.string(), error));
    REQUIRE(saveProject(doc, std::nullopt, file.string(), error));
    CHECK(fs::is_directory(folder));
    CHECK(fs::is_regular_file(file));
    CHECK(!fs::exists(dir / (name + ".nekophoto.saving")));
    checkSameFiles(folder, file);
    auto fromFolder = loadProject(folder.string(), error);
    REQUIRE(fromFolder.has_value());
    auto fromFile = loadProject(file.string(), error);
    if (!fromFile) std::fprintf(stderr, "    %s\n", error.message.c_str());
    REQUIRE(fromFile.has_value());
    CHECK(fromFile->colorMode == doc.colorMode);
    CHECK(fromFile->sampleType == doc.sampleType);
    CHECK_EQ(fromFile->smartObjects.size(), doc.smartObjects.size());
    CHECK_EQ(fromFile->channels.size(), doc.channels.size());
    REQUIRE(fromFile->layers.size() == doc.layers.size());
    for (size_t i = 0; i < doc.layers.size(); i++) {
        const Layer &a = doc.layers[i], &b = fromFile->layers[i], &c = fromFolder->layers[i];
        CHECK(a.id == b.id);
        CHECK(samePixels(a.asset ? a.asset->image : AnyImage(), b.asset ? b.asset->image : AnyImage()));
        CHECK(samePixels(c.asset ? c.asset->image : AnyImage(), b.asset ? b.asset->image : AnyImage()));
        CHECK(a.mask.has_value() == b.mask.has_value());
        if (a.mask && b.mask) CHECK(sameGray(a.mask->asset.image, b.mask->asset.image));
    }
    for (size_t i = 0; i < doc.channels.size() && i < fromFile->channels.size(); i++) CHECK(sameGray(doc.channels[i].image, fromFile->channels[i].image));
    std::error_code ec;
    fs::remove_all(dir, ec);
}

} // namespace

TEST_CASE(zip_writer_and_reader_round_trip) {
    const fs::path dir = tempDir();
    const fs::path file = dir / "a.zip";
    std::vector<uint8_t> text(5000, 'a'), binary(3000);
    for (size_t i = 0; i < binary.size(); i++) binary[i] = uint8_t(i * 31);
    {
        ZipFileWriter zip;
        std::string why;
        REQUIRE(zip.open(file.string(), &why));
        REQUIRE(zip.add("mimetype", text.data(), 10, false, &why));
        REQUIRE(zip.add("data/text.json", text, true, &why));
        REQUIRE(zip.add("data/binary.png", binary, false, &why));
        REQUIRE(zip.add("empty", nullptr, 0, true, &why));
        CHECK(!zip.add("../escape", binary, false, &why));
        REQUIRE(zip.finish(&why));
    }
    ZipFileReader zip;
    std::string why;
    REQUIRE(zip.openFile(file.string(), &why));
    REQUIRE(zip.entries().size() == 4u);
    CHECK_EQ(zip.entries()[0].name, std::string("mimetype"));
    CHECK_EQ(int(zip.entries()[1].method), 8);   // text is deflated
    CHECK_EQ(int(zip.entries()[2].method), 0);   // images are stored
    std::vector<uint8_t> out;
    REQUIRE(zip.read(*zip.find("data/text.json"), out, &why));
    CHECK(out == text);
    REQUIRE(zip.read(*zip.find("data/binary.png"), out, &why));
    CHECK(out == binary);
    REQUIRE(zip.read(*zip.find("empty"), out, &why));
    CHECK(out.empty());
    zip.close();
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE(zip_entry_names_must_stay_inside) {
    CHECK(isSafeZipEntryName("images/a.png"));
    CHECK(isSafeZipEntryName("images/"));
    CHECK(!isSafeZipEntryName(""));
    CHECK(!isSafeZipEntryName("/etc/passwd"));
    CHECK(!isSafeZipEntryName("../a"));
    CHECK(!isSafeZipEntryName("images/../../a"));
    CHECK(!isSafeZipEntryName("images/./a"));
    CHECK(!isSafeZipEntryName("images//a"));
    CHECK(!isSafeZipEntryName("C:/a"));
    CHECK(!isSafeZipEntryName("images\\a"));
    CHECK(!isSafeZipEntryName(std::string("a\0b", 3)));
}

TEST_CASE(document_file_starts_like_an_odf_package) {
    const fs::path dir = tempDir();
    const fs::path file = dir / "Magic.nekophoto";
    ProjectError error;
    REQUIRE(saveProject(richDocument(), std::nullopt, file.string(), error));
    const std::vector<uint8_t> bytes = fileBytes(file);
    REQUIRE(bytes.size() > 100);
    CHECK(bytes[0] == 'P' && bytes[1] == 'K' && bytes[2] == 3 && bytes[3] == 4);
    const std::string head(bytes.begin() + 30, bytes.begin() + 30 + 8 + 34);
    CHECK_EQ(head, std::string("mimetypeapplication/vnd.nekophoto.document"));
    ZipFileReader zip;
    REQUIRE(zip.openFile(file.string()));
    CHECK_EQ(zip.entries()[0].name, std::string("mimetype"));
    CHECK_EQ(int(zip.entries()[0].method), 0);
    std::vector<uint8_t> header;
    REQUIRE(zip.find("nekophoto.json") && zip.read(*zip.find("nekophoto.json"), header));
    const std::string text(header.begin(), header.end());
    CHECK(text.find("\"format_id\": \"org.nekophoto.document\"") != std::string::npos);
    CHECK(text.find("\"version\": 1") != std::string::npos);
    CHECK(text.find("\"minimum_reader_version\": 1") != std::string::npos);
    CHECK(text.find("\"writer\": \"NekoPhoto ") != std::string::npos);
    const ZipFileEntry* preview = zip.find("previews/composite.png");
    REQUIRE(preview != nullptr);
    std::vector<uint8_t> png;
    REQUIRE(zip.read(*preview, png));
    auto image = decodePngImage(png.data(), png.size());
    REQUIRE(image != nullptr);
    CHECK_EQ(image->width(), 48);
    CHECK(zip.find("manifest.json") != nullptr);
    zip.close();
    CHECK(loadedActiveLayer(file.string()) == std::nullopt);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE(document_preview_is_at_most_1024_pixels) {
    const fs::path dir = tempDir();
    const fs::path file = dir / "Big.nekophoto";
    Document doc(3000, 1500);
    doc.layers.emplace_back(Asset::make(ImagePtr(std::make_shared<Image>(10, 10)), "Pixels"), Point{0, 0});
    ProjectError error;
    REQUIRE(saveProject(doc, std::nullopt, file.string(), error));
    ZipFileReader zip;
    REQUIRE(zip.openFile(file.string()));
    std::vector<uint8_t> png;
    REQUIRE(zip.read(*zip.find("previews/composite.png"), png));
    zip.close();
    auto image = decodePngImage(png.data(), png.size());
    REQUIRE(image != nullptr);
    CHECK_EQ(image->width(), 1024);
    CHECK_EQ(image->height(), 512);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE(document_round_trip_8_bit_masks_channels_smart_objects) {
    roundTrip(richDocument(), "Rich");
}

TEST_CASE(document_round_trip_16_bit) {
    Document doc = richDocument();
    REQUIRE(convertSampleType(doc, SampleType::U16));
    roundTrip(doc, "Deep");
}

TEST_CASE(document_round_trip_32_bit) {
    Document doc = richDocument();
    REQUIRE(convertSampleType(doc, SampleType::F32));
    roundTrip(doc, "Float");
}

TEST_CASE(document_round_trip_cmyk_and_lab) {
    for (ColorMode mode : {ColorMode::CMYK, ColorMode::Lab})
        for (SampleType depth : {SampleType::U8, SampleType::U16}) {
            Document doc = richDocument();
            doc.smartObjects.clear();
            if (depth == SampleType::U16) REQUIRE(convertSampleType(doc, depth));
            std::string why;
            REQUIRE(convertDocumentMode(doc, mode, ColorProfile(), ConvertOptions(), &why));
            roundTrip(doc, mode == ColorMode::CMYK ? "Cmyk" : "Lab");
        }
}

TEST_CASE(document_save_replaces_and_keeps_unknown_entries) {
    const fs::path dir = tempDir();
    const fs::path file = dir / "Later.nekophoto";
    ProjectError error;
    Document doc = richDocument();
    REQUIRE(saveProject(doc, doc.layers[1].id, file.string(), error));
    CHECK(loadedActiveLayer(file.string()) == doc.layers[1].id);
    // A later version's file: one more entry at the top, one in a folder of its own.
    {
        ZipFileReader in;
        REQUIRE(in.openFile(file.string()));
        ZipFileWriter out;
        REQUIRE(out.open((dir / "copy.zip").string()));
        for (const ZipFileEntry& e : in.entries()) {
            std::vector<uint8_t> bytes;
            REQUIRE(in.read(e, bytes));
            REQUIRE(out.add(e.name, bytes, false));
        }
        const std::string extra = "{\"later\":true}";
        REQUIRE(out.add("future/state.json", reinterpret_cast<const uint8_t*>(extra.data()), extra.size(), true));
        REQUIRE(out.add("notes.txt", reinterpret_cast<const uint8_t*>(extra.data()), 3, false));
        REQUIRE(out.finish());
    }
    fs::rename(dir / "copy.zip", file);
    auto loaded = loadProject(file.string(), error);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->packageExtras != nullptr);
    CHECK_EQ(loaded->packageExtras->size(), size_t(2));
    // Saved again (over itself): the entries come through.
    REQUIRE(saveProject(*loaded, std::nullopt, file.string(), error));
    ZipFileReader zip;
    REQUIRE(zip.openFile(file.string()));
    const ZipFileEntry* future = zip.find("future/state.json");
    REQUIRE(future != nullptr);
    std::vector<uint8_t> bytes;
    REQUIRE(zip.read(*future, bytes));
    CHECK_EQ(std::string(bytes.begin(), bytes.end()), std::string("{\"later\":true}"));
    CHECK(zip.find("notes.txt") != nullptr);
    zip.close();
    // Only the one file is left beside it: no staging folder or temporary file.
    int siblings = 0;
    for (auto& e : fs::directory_iterator(dir)) { (void)e; siblings++; }
    CHECK_EQ(siblings, 1);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE(document_from_a_newer_version_is_refused_as_such) {
    const fs::path dir = tempDir();
    const fs::path file = dir / "Newer.nekophoto";
    ZipFileWriter out;
    REQUIRE(out.open(file.string()));
    const std::string mime = documentMimeType;
    const std::string header = "{\"format\":\"NekoPhoto Document\",\"format_id\":\"org.nekophoto.document\",\"version\":3,\"minimum_reader_version\":2}";
    REQUIRE(out.add("mimetype", reinterpret_cast<const uint8_t*>(mime.data()), mime.size(), false));
    REQUIRE(out.add("nekophoto.json", reinterpret_cast<const uint8_t*>(header.data()), header.size(), true));
    REQUIRE(out.finish());
    ProjectError error;
    CHECK(!loadProject(file.string(), error));
    CHECK(error.kind == ProjectError::Version);
    // A ZIP without the mimetype is not a document.
    ZipFileWriter plain;
    REQUIRE(plain.open(file.string()));
    REQUIRE(plain.add("manifest.json", reinterpret_cast<const uint8_t*>(header.data()), header.size(), true));
    REQUIRE(plain.finish());
    CHECK(!loadProject(file.string(), error));
    CHECK(error.kind == ProjectError::Invalid);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE(failed_document_save_leaves_the_original) {
    const fs::path dir = tempDir();
    const fs::path file = dir / "Keep.nekophoto";
    ProjectError error;
    REQUIRE(saveProject(richDocument(), std::nullopt, file.string(), error));
    const std::vector<uint8_t> before = fileBytes(file);
    Document bad = richDocument();
    bad.layers[1].asset = Asset::makeAny(Image16Ptr(std::make_shared<Image16>(2, 2)), "wrong depth");
    CHECK(!saveProject(bad, std::nullopt, file.string(), error));
    CHECK(fileBytes(file) == before);
    int siblings = 0;
    for (auto& e : fs::directory_iterator(dir)) { (void)e; siblings++; }
    CHECK_EQ(siblings, 1);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_MAIN()
