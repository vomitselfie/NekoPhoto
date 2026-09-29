// BrushStroke in a 32-bit document (docs/high-bit-depth-plan.md, P5c): StrokeRasterOf<StrokeOps<F32>>, the same stroke
// as at 8 and 16 bits on premultiplied linear float. The dab profile, the stamps and the build-up are the lower depths'
// without rounding; the colour is the pickers' colour linearised through the document's curve; colour above 1 is kept.
#include "compositor/brush.h"
#include "stroke_raster.h"

namespace compositor {

template class StrokeRasterOf<StrokeOps<SampleType::F32>>;

template <>
std::shared_ptr<TiledSourceOf<ImageF>> tiledProcessedNative<ImageF>(Document document, std::function<void(ImageF&)> process, int margin) {
    const int w = document.width, h = document.height;
    margin = std::max(0, margin);
    auto shared = std::make_shared<Document>(std::move(document));
    return std::make_shared<TiledSourceOf<ImageF>>(w, h, [shared, process = std::move(process), margin, w, h](int x, int y, int tw, int th, ImageF& out) {
        const int px0 = std::max(0, x - margin), py0 = std::max(0, y - margin);
        const int px1 = std::min(w, x + tw + margin), py1 = std::min(h, y + th + margin);
        RenderOptions options;
        options.region = Rect(px0, py0, px1 - px0, py1 - py0);
        ImageF padded;
        renderF(*shared, options, padded);
        process(padded);
        for (int row = 0; row < th; row++) std::memcpy(out.row(row), padded.pixel(x - px0, y + row - py0), size_t(tw) * 4 * sizeof(float));
    });
}

} // namespace compositor
