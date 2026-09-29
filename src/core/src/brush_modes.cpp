// BrushStroke in a CMYK document (docs/high-bit-depth-plan.md, P7 E): StrokeRasterOf<CmykStrokeOps<S>>, the stroke of
// its depth on five samples (the four inks stored inverted, then alpha). Lab documents paint with the RGB rasters of
// their depth (four samples; L, a and b as stored). The colour arrives in the document's model (brush.cpp).
#include "compositor/brush.h"
#include "stroke_raster.h"

namespace compositor {

template class StrokeRasterOf<CmykStrokeOps<SampleType::U8>>;
template class StrokeRasterOf<CmykStrokeOps<SampleType::U16>>;

namespace {
/// A tile of `document` at its own layout (renderNative), processed; `Img` the layout's buffer.
template <class Img>
std::shared_ptr<TiledSourceOf<Img>> tiledNative(Document document, std::function<void(Img&)> process, int margin) {
    const int w = document.width, h = document.height;
    margin = std::max(0, margin);
    const int channels = colorModeChannels(document.colorMode);
    auto shared = std::make_shared<Document>(std::move(document));
    return std::make_shared<TiledSourceOf<Img>>(w, h, [shared, process = std::move(process), margin, w, h, channels](int x, int y, int tw, int th, Img& out) {
        const int px0 = std::max(0, x - margin), py0 = std::max(0, y - margin);
        const int px1 = std::min(w, x + tw + margin), py1 = std::min(h, y + th + margin);
        RenderOptions options;
        options.region = Rect(px0, py0, px1 - px0, py1 - py0);
        const AnyImage native = renderNative(*shared, options);
        std::shared_ptr<const Img> rendered;
        if constexpr (std::is_same_v<Img, Image>) rendered = native.u8();
        else if constexpr (std::is_same_v<Img, Image16>) rendered = native.u16();
        else rendered = native.c8();
        if (!rendered) return;
        Img padded(*rendered);
        process(padded);
        using Sample = std::remove_cv_t<std::remove_pointer_t<decltype(padded.row(0))>>;
        for (int row = 0; row < th; row++) std::memcpy(out.row(row), padded.pixel(x - px0, y + row - py0), size_t(tw) * size_t(channels) * sizeof(Sample));
    }, 256, channels);
}
} // namespace

template <>
std::shared_ptr<TiledSourceOf<Image>> tiledProcessedNative<Image>(Document document, std::function<void(Image&)> process, int margin) {
    return tiledNative<Image>(std::move(document), std::move(process), margin);
}
template <>
std::shared_ptr<TiledSourceOf<Image16>> tiledProcessedNative<Image16>(Document document, std::function<void(Image16&)> process, int margin) {
    return tiledNative<Image16>(std::move(document), std::move(process), margin);
}
template <>
std::shared_ptr<TiledSourceOf<ImageC8>> tiledProcessedNative<ImageC8>(Document document, std::function<void(ImageC8&)> process, int margin) {
    return tiledNative<ImageC8>(std::move(document), std::move(process), margin);
}

} // namespace compositor
