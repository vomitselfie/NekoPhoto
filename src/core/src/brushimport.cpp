#include "compositor/brushimport.h"
#include "brushformats.h"
#include "compositor/png.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <new>
#include <stdexcept>
#include <iterator>
#include <set>

namespace compositor {

namespace fs = std::filesystem;

std::shared_ptr<GrayImage> roundTipImage(double diameter, double hardness, double roundness) {
    const int w = std::clamp(int(std::ceil(diameter)), 1, 1024), h = std::clamp(int(std::ceil(diameter * std::clamp(roundness, 0.01, 1.0))), 1, 1024);
    auto tip = std::make_shared<GrayImage>(w, h, 0);
    const double rx = w / 2.0, ry = h / 2.0, solid = std::clamp(hardness, 0.0, 1.0);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const double dx = (x + 0.5 - rx) / rx, dy = (y + 0.5 - ry) / ry, d = std::sqrt(dx * dx + dy * dy);
            double v = d >= 1 ? 0 : d <= solid ? 1 : 1 - (d - solid) / (1 - solid);
            v = v * v * (3 - 2 * v);   // smoothstep: the soft rim Photoshop draws
            tip->at(x, y) = uint8_t(std::lround(v * 255));
        }
    return tip;
}


std::shared_ptr<GrayImage> tipFromImage(const Image& image) {
    const int w = image.width(), h = image.height();
    if (w <= 0 || h <= 0) return nullptr;
    // Transparency decides: a shape on a clear background paints by its alpha; an opaque image by its darkness.
    size_t translucent = 0;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) translucent += image.pixel(x, y)[3] < 250;
    const bool byAlpha = translucent > size_t(w) * size_t(h) / 100;
    GrayImage tip(w, h, 0);
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t* p = image.pixel(x, y);
            unsigned v;
            if (byAlpha) v = p[3];
            else {
                // Premultiplied and opaque here, so the channels are the colour: darkness is 255 - luminance.
                const unsigned luminance = (p[0] * 54u + p[1] * 183u + p[2] * 19u + 128) >> 8;
                v = 255 - std::min(255u, luminance);
            }
            tip.at(x, y) = uint8_t(v);
            if (v > 2) { x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y); }
        }
    if (x1 < 0) return nullptr;
    auto cropped = std::make_shared<GrayImage>(x1 - x0 + 1, y1 - y0 + 1);
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) cropped->at(x - x0, y - y0) = tip.at(x, y);
    return cropped;
}

std::optional<TipPreset> presetFromImage(const Image& image, const std::string& name) {
    auto tip = tipFromImage(image);
    if (!tip) return std::nullopt;
    TipPreset preset;
    preset.name = name;
    preset.tip.shape = tip;
    preset.diameter = std::clamp(double(std::max(tip->width(), tip->height())), 2.0, 300.0);
    if (!preset.tip.normalize()) return std::nullopt;
    return preset;
}

namespace {

std::optional<BrushImport> readBrushFile(const std::string& path, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { if (error) *error = "cannot read " + path; return std::nullopt; }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    char magic[8] = {};
    std::copy_n(file.begin(), std::min<size_t>(8, file.size()), magic);
    const std::string name = fs::path(path).stem().string();
    std::string extension = fs::path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    // Photoshop: from version 6 an 8BIM section follows the version words; 1 and 2 are known by the name.
    const bool abrSections = file.size() >= 8 && file[0] == 0 && file[1] >= 6 && file[1] <= 10 && std::equal(magic + 4, magic + 8, "8BIM");
    if (abrSections || (extension == ".abr" && file.size() >= 4 && file[0] == 0 && (file[1] == 1 || file[1] == 2)))
        return readAbr(file.data(), file.size(), name, error);
    // Clip Studio: an SQLite database.
    if (file.size() >= 16 && std::equal(file.begin(), file.begin() + 16, "SQLite format 3")) return readClipStudio(path, name, error);
    // Procreate: a ZIP (local file header "PK\3\4") holding a Brush.archive.
    if (file.size() >= 4 && std::equal(magic, magic + 4, "PK\x03\x04")) return readProcreate(file.data(), file.size(), name, error);
    // A PNG is a single tip; other raster formats go through the application's image reader.
    if (std::equal(magic, magic + 8, "\x89PNG\r\n\x1a\n")) {
        auto image = readPngImage(path, error);
        if (!image) return std::nullopt;
        auto preset = presetFromImage(*image, name);
        if (!preset) { if (error) *error = "nothing in " + name + " would paint"; return std::nullopt; }
        return BrushImport{"Images", {*preset}, {}};
    }
    if (error) *error = "not a brush file this version can read";
    return std::nullopt;
}

} // namespace

std::optional<BrushImport> importBrushFile(const std::string& path, std::string* error) {
    // The readers bound what they decode, but a size in a crafted file can still ask for more than memory.
    try {
        return readBrushFile(path, error);
    } catch (const std::bad_alloc&) {
        if (error) *error = "the file asks for more memory than is available";
    } catch (const std::length_error&) {
        if (error) *error = "the file asks for more memory than is available";
    }
    return std::nullopt;
}

namespace {

/// A folder name from a display name: path separators and control characters go, the rest stays readable.
std::string folderName(const std::string& name) {
    std::string out;
    for (char c : name) out += (c == '/' || c == '\\' || c == ':' || static_cast<unsigned char>(c) < 32) ? '_' : c;
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    while (!out.empty() && (out.front() == ' ' || out.front() == '.')) out.erase(out.begin());
    return out.empty() ? std::string("Brush") : out;
}

} // namespace

bool saveBrushImport(const BrushImport& import, const std::string& root, std::vector<std::string>* written, std::string* error) {
    const fs::path setDir = fs::path(root) / folderName(import.set);
    std::error_code ec;
    fs::create_directories(setDir, ec);
    if (ec) { if (error) *error = "cannot create " + setDir.string() + ": " + ec.message(); return false; }
    std::set<std::string> used;
    for (const auto& entry : fs::directory_iterator(setDir, ec)) used.insert(entry.path().filename().string());
    for (const TipPreset& preset : import.brushes) {
        std::string base = folderName(preset.name), candidate = base;
        for (int n = 2; used.count(candidate); n++) candidate = base + " " + std::to_string(n);
        used.insert(candidate);
        const fs::path dir = setDir / candidate;
        if (!saveTipPreset(dir.string(), preset, error)) return false;
        auto preview = renderTipPreview(preset, 256, 64);
        if (preview && !writePngImage((dir / "preview.png").string(), *preview, 0, error)) return false;
        if (written) written->push_back(dir.string());
    }
    return true;
}

} // namespace compositor
