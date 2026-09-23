#include "compositor/brushimport.h"
#include "compositor/png.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>

namespace compositor {

namespace fs = std::filesystem;

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

std::optional<BrushImport> importBrushFile(const std::string& path, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { if (error) *error = "cannot read " + path; return std::nullopt; }
    char magic[8] = {};
    in.read(magic, 8);
    const std::string name = fs::path(path).stem().string();
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
