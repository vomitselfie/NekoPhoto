// Checks the Layer Style dialog's writer against real files: every styled layer's style is read for editing,
// written again as a fresh 'lfx2' and put back on the layer; the document must render as before, and writing
// the rewritten style once more must give the same bytes. `restyle_check FILE_OR_DIR...`; exit 1 on a difference.
// Patchy's fixtures (Photoshop-saved) are the corpus: restyle_check ../Patchy/test-fixtures/psd
#include "compositor/layerstyle.h"
#include "compositor/psd.h"
#include "compositor/render.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace compositor;

namespace {

bool hasEffectsBlock(const Layer& layer) {
    if (!layer.psdCarry) return false;
    for (auto& b : layer.psdCarry->blocks) if (b.key == "lfx2" || b.key == "lmfx" || b.key == "lfxs") return true;
    return false;
}

int worstDifference(const Image& a, const Image& b) {
    if (a.width() != b.width() || a.height() != b.height()) return 256;
    int worst = 0;
    for (int y = 0; y < a.height(); y++)
        for (int x = 0; x < a.width(); x++)
            for (int c = 0; c < 4; c++) worst = std::max(worst, std::abs(int(a.pixel(x, y)[c]) - int(b.pixel(x, y)[c])));
    return worst;
}

bool check(const fs::path& path, int& restyled) {
    std::string error;
    auto imported = importPsd(path.string(), &error);
    if (!imported) { std::printf("SKIP %s: %s\n", path.filename().c_str(), error.c_str()); return true; }
    Document& doc = imported->document;
    std::vector<std::string> problems;
    int here = 0;
    auto before = renderFlattened(doc);
    for (Layer& layer : doc.layers) {
        if (!hasEffectsBlock(layer)) continue;
        const LayerStyle style = editableLayerStyle(layer, doc);
        if (!hasAnyEffect(style)) continue;
        setLayerStyle(layer, style);
        const auto first = authorLayerStyleBlock(style);
        const auto second = authorLayerStyleBlock(editableLayerStyle(layer, doc));
        if (first != second) problems.push_back("\"" + layer.name + "\": the rewritten style does not read back the same");
        here++;
    }
    if (here) {
        auto after = renderFlattened(doc);
        if (const int d = worstDifference(*before, *after); d > 0) problems.push_back("render changed by up to " + std::to_string(d) + " levels");
    }
    restyled += here;
    if (here || !problems.empty()) {
        std::printf("%s %s  [%d styled layer(s)]", problems.empty() ? "ok  " : "FAIL", path.filename().c_str(), here);
        for (auto& p : problems) std::printf("\n     %s", p.c_str());
        std::printf("\n");
    }
    return problems.empty();
}

} // namespace

int main(int argc, char** argv) {
    std::vector<fs::path> files;
    for (int i = 1; i < argc; i++) {
        if (fs::is_directory(argv[i])) { for (auto& e : fs::directory_iterator(argv[i])) if (e.path().extension() == ".psd" || e.path().extension() == ".psb") files.push_back(e.path()); }
        else files.push_back(argv[i]);
    }
    std::sort(files.begin(), files.end());
    int failed = 0, restyled = 0;
    for (auto& f : files) if (!check(f, restyled)) failed++;
    std::printf("%zu files, %d failed, %d styles rewritten\n", files.size(), failed, restyled);
    return failed ? 1 : 0;
}
