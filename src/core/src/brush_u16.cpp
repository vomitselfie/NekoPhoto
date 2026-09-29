// BrushStroke on a 16-bit document (docs/high-bit-depth-plan.md, P3b): StrokeRaster<U16>, the same stroke as at 8 bits
// with the working pixels, the mask, the coverage and the selection at 0..32768. The dab profile, the stamps, the
// build-up rules and the recompose follow the 8-bit code step for step, with 15-bit samples in place of bytes, so an
// 8-bit-sourced layer painted here and reduced to 8 bits lands within a level of the 8-bit stroke
// (tests/depth_paint_tests.cpp).
#include "compositor/brush.h"
#include "stroke_raster.h"

namespace compositor {

/// Half-open bounds of the pixels with any alpha within `within`: each row scanned in from both ends, rows in parallel.
PixelBounds StrokeOps<SampleType::U16>::bounds(const Image16& image, const PixelBounds& within) {
    const int x0 = std::max(0, within.x0), y0 = std::max(0, within.y0), x1 = std::min(image.width(), within.x1), y1 = std::min(image.height(), within.y1);
    if (x0 >= x1 || y0 >= y1) return {};
    std::vector<int> first(size_t(y1 - y0), x1), last(size_t(y1 - y0), x0);
    parallelRows(y0, y1, [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            const uint16_t* row = image.row(y);
            int a = x0;
            while (a < x1 && !row[a * 4 + 3]) a++;
            if (a == x1) continue;
            int b = x1;
            while (b > a && !row[(b - 1) * 4 + 3]) b--;
            first[size_t(y - y0)] = a;
            last[size_t(y - y0)] = b;
        }
    }, 64);
    int bx0 = x1, by0 = y1, bx1 = x0, by1 = y0;
    for (int y = y0; y < y1; y++) {
        const int a = first[size_t(y - y0)], b = last[size_t(y - y0)];
        if (a >= b) continue;
        bx0 = std::min(bx0, a); bx1 = std::max(bx1, b); by0 = std::min(by0, y); by1 = std::max(by1, y + 1);
    }
    if (bx0 >= bx1 || by0 >= by1) return {};
    return {bx0, by0, bx1, by1};
}

/// The working copy made in parallel into zero pages.
std::shared_ptr<Image16> StrokeOps<SampleType::U16>::copyOf(const Image16& image) {
    auto copy = std::make_shared<Image16>(image.width(), image.height());
    stroke::copyImage(image, *copy, 0, 0);
    return copy;
}

template class StrokeRasterOf<StrokeOps<SampleType::U16>>;

std::shared_ptr<TiledSource16> tiledProcessedDocument16(Document document, std::function<void(Image16&)> process, int margin) {
    const int w = document.width, h = document.height;
    margin = std::max(0, margin);
    auto shared = std::make_shared<Document>(std::move(document));
    return std::make_shared<TiledSource16>(w, h, [shared, process = std::move(process), margin, w, h](int x, int y, int tw, int th, Image16& out) {
        const int px0 = std::max(0, x - margin), py0 = std::max(0, y - margin);
        const int px1 = std::min(w, x + tw + margin), py1 = std::min(h, y + th + margin);
        RenderOptions options;
        options.region = Rect(px0, py0, px1 - px0, py1 - py0);
        Image16 padded;
        render16(*shared, options, padded);
        process(padded);
        for (int row = 0; row < th; row++) std::memcpy(out.row(row), padded.pixel(x - px0, y + row - py0), size_t(tw) * 4 * sizeof(uint16_t));
    });
}

} // namespace compositor
