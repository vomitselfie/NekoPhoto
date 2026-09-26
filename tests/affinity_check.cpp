// Opens Affinity documents and reports what the import made of them: the canvas, every layer, the notes, and how
// far the flattened result is from the preview Affinity embedded (both reduced to the preview's size).
//   affinity_check [--out DIR] file.af ...
// With --out, writes <name>.png (the import, flattened) and <name>.preview.png (Affinity's preview) for a look.
#include "compositor/affinity.h"
#include "compositor/png.h"
#include "compositor/render.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

using namespace compositor;
namespace fs = std::filesystem;

int main(int argc, char** argv) {
    std::string out;
    int failures = 0;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--out" && i + 1 < argc) { out = argv[++i]; fs::create_directories(out); continue; }
        std::string error;
        auto imported = importAffinity(arg, &error);
        std::printf("== %s\n", arg.c_str());
        if (!imported) { std::printf("   FAILED: %s\n", error.c_str()); failures++; continue; }
        const Document& doc = imported->document;
        std::printf("   canvas %d x %d, %zu layers\n", doc.width, doc.height, doc.layers.size());
        for (const Layer& l : doc.layers) {
            int depth = 0;
            for (auto p = l.parentId; p; p = doc.find(*p) ? doc.find(*p)->parentId : std::nullopt) depth++;
            std::printf("   %*s%s \"%s\" at %.0f,%.0f %dx%d opacity %.2f %s%s%s%s%s\n", depth * 2, "", l.isGroup ? "group" : "layer", l.name.c_str(),
                        l.transform.origin.x, l.transform.origin.y, l.asset ? l.asset->image->width() : 0, l.asset ? l.asset->image->height() : 0, l.opacity,
                        blendModeName(l.blendMode), l.isGroup && l.passThrough ? " pass-through" : "", l.visible ? "" : " hidden", l.mask ? " masked" : "",
                        l.maskSourceId ? " clipped" : "");
        }
        for (const auto& note : imported->notes) std::printf("   note: %s\n", note.c_str());
        auto flat = renderFlattened(doc);
        if (imported->composite) {
            const int side = std::min(std::max(flat->width(), flat->height()), std::max(imported->composite->width(), imported->composite->height()));
            auto reduced = makeThumbnail(*flat, side);
            auto shrunk = makeThumbnail(*imported->composite, side);
            const Image& preview = *shrunk;
            if (std::abs(reduced->width() - preview.width()) <= 2 && std::abs(reduced->height() - preview.height()) <= 2) {
                const int cw = std::min(reduced->width(), preview.width()), ch = std::min(reduced->height(), preview.height());
                double total = 0;
                for (int y = 0; y < ch; y++)
                    for (int x = 0; x < cw * 4; x++) total += std::abs(int(reduced->row(y)[x]) - int(preview.row(y)[x]));
                std::printf("   differs from Affinity's preview by %.2f / 255 on average\n", total / (double(cw) * ch * 4));
            } else {
                std::printf("   preview is %d x %d, the reduced import %d x %d: not compared\n", preview.width(), preview.height(), reduced->width(), reduced->height());
            }
        }
        if (!out.empty()) {
            const std::string stem = (fs::path(out) / fs::path(arg).stem()).string();
            auto straight = *flat;
            unpremultiply(straight);
            writePngImage(stem + ".png", straight);
            if (imported->composite) { Image p = *imported->composite; unpremultiply(p); writePngImage(stem + ".preview.png", p); }
        }
    }
    return failures ? 1 : 0;
}
