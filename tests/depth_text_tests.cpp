// Text in a 16-bit document (docs/bit-depth.md): painted at 16 bits per channel, it is within a level of the 8-bit
// raster reduced to 8 bits, rich text and Warp Text included; a 16-bit document with a text layer goes out to PSD as a
// Photoshop type layer at 16 bits and comes back as editable text; and a text layer's outlines are the same at both
// depths. COMPOSITOR_REPORT_U16_CALIBRATION=1 prints the worst difference for each.
#include "check.h"
#include "TextLayer.h"
#include "compositor/depth.h"
#include "compositor/psd.h"
#include "compositor/psd_writer.h"
#include "compositor/render.h"
#include <QGuiApplication>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace compositor;

namespace {

LayerText plain() {
    LayerText t;
    t.text = "Deep text\nsecond line";
    t.fontSize = 28;
    t.red = 0.13; t.green = 0.4; t.blue = 0.8;
    return t;
}

LayerText rich() {
    LayerText t = plain();
    TextRun a; a.length = 5; a.fontSize = 34; a.bold = true; a.red = 0.9; a.green = 0.2; a.blue = 0.1;
    TextRun b; b.length = utf16Length(t.text) - 5; b.fontSize = 20; b.italic = true; b.red = 0.1; b.green = 0.6; b.blue = 0.3;
    t.runs = {a, b};
    settleTextRuns(t);
    return t;
}

/// The largest difference between the 16-bit raster reduced to 8 bits and the 8-bit raster.
int worstApart(const LayerText& text, const char* what) {
    auto eight = app::renderTextLayer(text);
    auto deep = app::renderTextLayer16(text);
    if (!eight || !deep) { check::fail(__FILE__, __LINE__, "text did not render"); return 256; }
    if (eight->width() != deep->width() || eight->height() != deep->height()) { check::fail(__FILE__, __LINE__, "sizes differ"); return 256; }
    auto reduced = narrowImage(*deep);
    int worst = 0;
    for (int y = 0; y < eight->height(); y++)
        for (int i = 0; i < eight->width() * 4; i++) worst = std::max(worst, std::abs(int(eight->row(y)[i]) - int(reduced->row(y)[i])));
    if (std::getenv("COMPOSITOR_REPORT_U16_CALIBRATION")) std::fprintf(stderr, "  %-24s worst %d\n", what, worst);
    return worst;
}

} // namespace

TEST_CASE(text_painted_at_sixteen_bits_is_within_a_level_of_eight_bits) {
    CHECK(worstApart(plain(), "plain text") <= 1);
    CHECK(worstApart(rich(), "rich text") <= 1);
    LayerText warped = plain();
    warped.warp.style = "warpArc";
    warped.warp.bend = 40;
    CHECK(worstApart(warped, "warped text") <= 1);
    // A 16-bit raster of text in the document's depth.
    const AnyImage at16 = app::renderTextLayerAt(plain(), SampleType::U16);
    CHECK(at16.u16() != nullptr);
    CHECK(app::renderTextLayerAt(plain(), SampleType::U8).u8() != nullptr);
}

TEST_CASE(a_sixteen_bit_text_layer_goes_to_psd_as_type_and_comes_back) {
    Document doc(240, 120);
    auto white = std::make_shared<Image>(240, 120);
    white->fill(255, 255, 255, 255);
    doc.layers.push_back(Layer(Asset::make(white, "Background"), Point(0, 0)));
    REQUIRE(convertSampleType(doc, SampleType::U16));
    const LayerText text = rich();
    const AnyImage raster = app::renderTextLayerAt(text, SampleType::U16);
    REQUIRE(raster.u16() != nullptr);
    Layer layer(Asset::makeAny(raster, "Words"), Point(10, 10));
    layer.text = text;
    layer.textImage = raster;
    doc.layers.push_back(layer);
    REQUIRE(doc.layers.back().isLiveText());
    PsdExportSummary summary;
    std::string error;
    const auto bytes = encodePsd(doc, app::psdExportOptions(), &summary, &error);
    REQUIRE(!bytes.empty());
    CHECK_EQ(summary.texts, 1);
    CHECK(bytes.size() > 24 && bytes[22] == 0 && bytes[23] == 16);   // a 16-bit file
    auto back = importPsdBytes(bytes, &error);
    REQUIRE(back.has_value());
    app::finishPsdText(*back);
    CHECK(back->document.sampleType == SampleType::U16);
    REQUIRE(back->document.layers.size() == 2);
    const Layer& words = back->document.layers[1];
    REQUIRE(words.text.has_value());
    CHECK_EQ(words.text->text, text.text);
    CHECK_EQ(words.text->runs.size(), size_t(2));
    CHECK(words.asset && words.asset->image.u16() != nullptr);   // its pixels at 16 bits
}

TEST_CASE(text_outlines_do_not_depend_on_the_depth) {
    const LayerText text = plain();
    Layer eight(Asset::makeAny(app::renderTextLayerAt(text, SampleType::U8), "A"), Point(5, 5));
    eight.text = text;
    eight.textImage = eight.asset->image;
    Layer deep(Asset::makeAny(app::renderTextLayerAt(text, SampleType::U16), "B"), Point(5, 5));
    deep.text = text;
    deep.textImage = deep.asset->image;
    auto a = app::textLayerOutline(eight);
    auto b = app::textLayerOutline(deep);
    REQUIRE(a && b);
    CHECK(a->subpaths.size() == b->subpaths.size());
    CHECK(!a->subpaths.empty());
    for (size_t i = 0; i < a->subpaths.size() && i < b->subpaths.size(); i++) CHECK(a->subpaths[i].knots.size() == b->subpaths[i].knots.size());
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    return check::run(argc, argv);
}
