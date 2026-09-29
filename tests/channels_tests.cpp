// Alpha and spot channels (channels.h): the model and undo, Save and Load Selection in every combine mode, the
// thumbnail click modifiers, the canvas operations, single-channel painting and adjustments at 8 and 16 bits, the
// channel view, and the PSD and project round trips.
#include "check.h"
#include "compositor/adjustments.h"
#include "compositor/brush.h"
#include "compositor/channels.h"
#include "compositor/depth.h"
#include "compositor/history.h"
#include "compositor/project.h"
#include "compositor/psd.h"
#include "compositor/psd_writer.h"
#include <filesystem>
#include <unistd.h>

using namespace compositor;
namespace fs = std::filesystem;

namespace {

constexpr int W = 40, H = 30;

/// A document with one opaque gray layer, at `depth`.
Document grayDocument(SampleType depth = SampleType::U8, uint8_t value = 128) {
    Document d(W, H);
    auto image = std::make_shared<Image>(W, H);
    image->fill(value, value, value, 255);
    Layer layer = depth == SampleType::U16 ? Layer(Asset::make(Image16Ptr(widenImage(*image)), "Layer"), Point(0, 0)) : Layer(Asset::make(ImagePtr(image), "Layer"), Point(0, 0));
    d.layers.push_back(layer);
    d.sampleType = depth;
    return d;
}

Selection rectSelection(const Rect& r, SampleType depth = SampleType::U8) {
    Selection s;
    auto g = rasterizeRect(r, W, H, false);
    if (depth == SampleType::U16) s.coverage = Gray16Ptr(widenGray(*g));
    else s.coverage = GrayPtr(g);
    return s;
}

/// A gray's value at (x, y) as 0..255, whatever its depth.
int at(const AnyGray& g, int x, int y) {
    if (const Gray16Ptr& p = g.u16()) return narrow16(p->at(x, y));
    return g.u8()->at(x, y);
}

int selected(const std::optional<Selection>& s, int x, int y) { return s && s->coverage ? at(s->coverage, x, y) : 0; }

} // namespace

TEST_CASE(channel_model_and_undo) {
    Document d = grayDocument();
    CHECK_EQ(nextChannelName(d), std::string("Alpha 1"));
    CHECK(channelAddProblem(d).empty());
    DocumentHistory history;
    std::optional<Document> doc = d;
    history.begin("New Channel", doc, std::nullopt);
    doc->channels.push_back(makeAlphaChannel(*doc, nextChannelName(*doc)));
    history.end(doc, std::nullopt);
    CHECK_EQ(nextChannelName(*doc), std::string("Alpha 2"));
    REQUIRE(doc->channels.size() == 1);
    const Channel& c = doc->channels[0];
    CHECK_EQ(c.image.width(), W);
    CHECK_EQ(at(c.image, 5, 5), 0);   // black: nothing selected
    CHECK(!(d == *doc));
    // The channel is document memory: masks' budget and the history's bytes.
    CHECK_EQ(doc->maskPixels(), (long long)W * H);
    auto undone = history.undo();
    REQUIRE(undone && undone->document);
    CHECK(undone->document->channels.empty());
    CHECK(history.retainedBytes(undone->document) >= size_t(W * H));   // the redo step keeps the channel's gray
    auto redone = history.redo();
    REQUIRE(redone && redone->document);
    CHECK_EQ(redone->document->channels.size(), size_t(1));
    CHECK(redone->document->channels[0] == c);
    // Photoshop's limit.
    Document full = grayDocument();
    for (int i = 0; i < Document::maxChannels; i++) full.channels.push_back(makeAlphaChannel(full, nextChannelName(full)));
    CHECK(!channelAddProblem(full).empty());
    // Depth conversion takes the channels along.
    Document deep = *doc;
    REQUIRE(convertSampleType(deep, SampleType::U16));
    CHECK(deep.channels[0].image.u16() != nullptr);
}

TEST_CASE(save_selection_modes) {
    for (SampleType depth : {SampleType::U8, SampleType::U16}) {
        Document d = grayDocument(depth);
        const Selection left = rectSelection(Rect(0, 0, 20, H), depth), top = rectSelection(Rect(0, 0, W, 15), depth);
        for (bool selectedAreas : {false, true}) {
            auto fresh = [&] {
                Channel c = makeAlphaChannel(d, "Alpha 1", left.coverage);
                setSelectedAreas(c, selectedAreas);
                return c;
            };
            // Color Indicates Selected Areas keeps what it selects: the gray is inverted.
            Channel c = fresh();
            CHECK_EQ(at(c.image, 5, 20), selectedAreas ? 0 : 255);
            CHECK_EQ(at(channelCoverage(c, depth), 5, 20), 255);
            struct Case { SelectionMode mode; int tl, tr, bl, br; };   // top-left, top-right, bottom-left, bottom-right selected
            for (const Case& k : {Case{SelectionMode::Replace, 255, 255, 0, 0}, Case{SelectionMode::Add, 255, 255, 255, 0},
                                  Case{SelectionMode::Subtract, 0, 0, 255, 0}, Case{SelectionMode::Intersect, 255, 0, 0, 0}}) {
                Channel ch = fresh();
                saveSelectionInto(ch, top, k.mode, depth, W, H);
                const AnyGray cov = channelCoverage(ch, depth);
                CHECK_EQ(at(cov, 5, 5), k.tl);
                CHECK_EQ(at(cov, 30, 5), k.tr);
                CHECK_EQ(at(cov, 5, 25), k.bl);
                CHECK_EQ(at(cov, 30, 25), k.br);
                CHECK(ch.image.sampleType() == depth);
            }
        }
        // No selection saved: nothing selected (Replace), or the channel as it was (Add).
        Channel c = makeAlphaChannel(d, "Alpha 1", left.coverage);
        saveSelectionInto(c, std::nullopt, SelectionMode::Add, depth, W, H);
        CHECK_EQ(at(c.image, 5, 5), 255);
        saveSelectionInto(c, std::nullopt, SelectionMode::Replace, depth, W, H);
        CHECK_EQ(at(c.image, 5, 5), 0);
    }
}

TEST_CASE(load_selection_modes_and_sources) {
    for (SampleType depth : {SampleType::U8, SampleType::U16}) {
        Document d = grayDocument(depth, 200);
        d.channels.push_back(makeAlphaChannel(d, "Left", rectSelection(Rect(0, 0, 20, H), depth).coverage));
        const Uuid id = d.channels[0].id;
        d.selection = rectSelection(Rect(0, 0, W, 15), depth);
        const SelectionSource source{SelectionSource::AlphaChannel, id};
        struct Case { SelectionMode mode; int tl, tr, bl, br; };
        for (const Case& k : {Case{SelectionMode::Replace, 255, 0, 255, 0}, Case{SelectionMode::Add, 255, 255, 255, 0},
                              Case{SelectionMode::Subtract, 0, 255, 0, 0}, Case{SelectionMode::Intersect, 255, 0, 0, 0}}) {
            auto s = loadSelectionFrom(d, source, false, k.mode, true);
            REQUIRE(s);
            CHECK_EQ(selected(s, 5, 5), k.tl);
            CHECK_EQ(selected(s, 30, 5), k.tr);
            CHECK_EQ(selected(s, 5, 25), k.bl);
            CHECK_EQ(selected(s, 30, 25), k.br);
            CHECK(s->coverage.sampleType() == depth);
        }
        auto inverted = loadSelectionFrom(d, source, true, SelectionMode::Replace, true);
        CHECK_EQ(selected(inverted, 5, 5), 0);
        CHECK_EQ(selected(inverted, 30, 5), 255);
        // Intersecting with no selection selects nothing (an empty selection, not "everything").
        Document none = d;
        none.selection.reset();
        auto empty = loadSelectionFrom(none, source, false, SelectionMode::Intersect, true);
        REQUIRE(empty);
        CHECK(empty->isEmpty());
        // The composite's channels, over white: a gray 200 layer is 200 in each; luminosity too.
        for (auto kind : {SelectionSource::Composite, SelectionSource::Red, SelectionSource::Green, SelectionSource::Blue}) {
            auto s = loadSelectionFrom(none, SelectionSource{kind, {}}, false, SelectionMode::Replace, true);
            CHECK(std::abs(selected(s, 3, 3) - 200) <= 1);
        }
        // A layer's transparency, and its mask.
        auto t = loadSelectionFrom(none, SelectionSource{SelectionSource::Transparency, d.layers[0].id}, false, SelectionMode::Replace, true);
        CHECK_EQ(selected(t, 3, 3), 255);
        std::string why;
        CHECK(!loadSelectionFrom(none, SelectionSource{SelectionSource::LayerMask, d.layers[0].id}, false, SelectionMode::Replace, true, &why));
        CHECK(!why.empty());
        CHECK(!loadSelectionFrom(none, SelectionSource{SelectionSource::AlphaChannel, "missing"}, false, SelectionMode::Replace, true));
    }
}

TEST_CASE(thumbnail_click_modifiers) {
    // Ctrl-click replaces, Ctrl+Shift adds, Ctrl+Alt subtracts, Ctrl+Shift+Alt intersects (Photoshop).
    CHECK(thumbnailClickMode(false, false) == SelectionMode::Replace);
    CHECK(thumbnailClickMode(true, false) == SelectionMode::Add);
    CHECK(thumbnailClickMode(false, true) == SelectionMode::Subtract);
    CHECK(thumbnailClickMode(true, true) == SelectionMode::Intersect);
}

TEST_CASE(channels_follow_the_canvas) {
    Document d = grayDocument();
    auto g = std::make_shared<GrayImage>(W, H, 0);
    g->at(3, 4) = 255;
    Channel c = makeAlphaChannel(d, "Alpha 1", GrayPtr(g));
    d.channels.push_back(c);
    Document cropped = d;
    cropped.width = 20; cropped.height = 20;
    cropChannels(cropped, 2, 2, 20, 20);
    CHECK_EQ(cropped.channels[0].image.width(), 20);
    CHECK_EQ(at(cropped.channels[0].image, 1, 2), 255);
    Document bigger = d;
    bigger.width = 50; bigger.height = 40;
    cropChannels(bigger, -5, -5, 50, 40);   // Canvas Size, centred: black where the old canvas did not reach
    CHECK_EQ(at(bigger.channels[0].image, 8, 9), 255);
    CHECK_EQ(at(bigger.channels[0].image, 0, 0), 0);
    Document flipped = d;
    flipChannels(flipped, true);
    CHECK_EQ(at(flipped.channels[0].image, W - 1 - 3, 4), 255);
    Document resized = d;
    resized.width = W * 2; resized.height = H * 2;
    resampleChannels(resized, W, H, Sampling::Nearest);
    CHECK_EQ(resized.channels[0].image.width(), W * 2);
    CHECK_EQ(at(resized.channels[0].image, 7, 9), 255);
}

TEST_CASE(single_channel_adjustment) {
    // Invert on the red channel alone: red inverted, green, blue and alpha untouched.
    auto image = std::make_shared<Image>(8, 8);
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) { uint8_t* p = image->pixel(x, y); p[0] = 40; p[1] = 90; p[2] = 160; p[3] = x < 4 ? 255 : 128; }
    // A half-transparent pixel holds premultiplied colour.
    for (int y = 0; y < 8; y++) for (int x = 4; x < 8; x++) { uint8_t* p = image->pixel(x, y); p[0] = 20; p[1] = 45; p[2] = 80; }
    AdjustmentSettings invert;
    invert.kind = AdjustmentKind::Invert;
    {
        Image edited = *image;
        REQUIRE(applyAdjustment(invert, edited, Rect(0, 0, 8, 8), 1));
        const AnyImage kept = keepColorChannels(ImagePtr(image), ImagePtr(std::make_shared<Image>(edited)), 0, 0, 1);
        const Image& k = *kept.u8();
        CHECK_EQ(int(k.pixel(1, 1)[0]), 215);
        CHECK_EQ(int(k.pixel(1, 1)[1]), 90);
        CHECK_EQ(int(k.pixel(1, 1)[2]), 160);
        CHECK_EQ(int(k.pixel(1, 1)[3]), 255);
        CHECK_EQ(int(k.pixel(5, 1)[0]), int(edited.pixel(5, 1)[0]));   // same alpha: the edit's value exactly
        CHECK_EQ(int(k.pixel(5, 1)[1]), 45);
        CHECK_EQ(int(k.pixel(5, 1)[3]), 128);
    }
    {
        auto deep = widenImage(*image);
        Image16 edited = *deep;
        REQUIRE(applyAdjustment(invert, edited, Rect(0, 0, 8, 8), 1));
        const AnyImage kept = keepColorChannels(Image16Ptr(deep), Image16Ptr(std::make_shared<Image16>(edited)), 0, 0, 4);
        const Image16& k = *kept.u16();
        CHECK_EQ(int(k.pixel(1, 1)[0]), int(deep->pixel(1, 1)[0]));
        CHECK_EQ(int(k.pixel(1, 1)[1]), int(deep->pixel(1, 1)[1]));
        CHECK_EQ(int(k.pixel(1, 1)[2]), int(edited.pixel(1, 1)[2]));
        CHECK(std::abs(int(narrow16(k.pixel(1, 1)[2])) - 95) <= 1);
        CHECK_EQ(int(k.pixel(1, 1)[3]), int(one16));
    }
}

TEST_CASE(single_channel_painting) {
    for (SampleType depth : {SampleType::U8, SampleType::U16}) {
        const Document before = grayDocument(depth, 100);
        BrushSettings s;
        s.diameter = 12; s.hardness = 1; s.opacity = 1;
        s.red = 1; s.green = 0; s.blue = 0;
        std::unique_ptr<BrushStroke> stroke = depth == SampleType::U16
            ? std::make_unique<BrushStroke>(before.layers[0], false, s, before.size(), SampleType::U16, nullptr)
            : std::make_unique<BrushStroke>(before.layers[0], false, s, before.size());
        REQUIRE(stroke->isValid());
        for (double x = 5; x < 35; x += 2) stroke->append(Point(x, 15));
        stroke->flush();
        BrushStroke::Commit done = stroke->commit();
        Document after = before;
        after.layers[0].asset = done.asset;
        after.layers[0].transform = done.transform;
        // Only green is active: red paint takes green to 0 where it lands; red, blue and alpha keep their values.
        CHECK(restrictToColorChannels(before, after, 2));
        const Layer& l = after.layers[0];
        REQUIRE(l.asset && l.asset->image);
        CHECK(l.transform == before.layers[0].transform);
        const AnyImage img = l.asset->image;
        CHECK(img.sampleType() == depth);
        auto px = [&](int x, int y, int c) { return img.u16() ? int(narrow16(img.u16()->pixel(x, y)[c])) : int(img.u8()->pixel(x, y)[c]); };
        CHECK_EQ(px(20, 15, 0), 100);
        CHECK_EQ(px(20, 15, 1), 0);
        CHECK_EQ(px(20, 15, 2), 100);
        CHECK_EQ(px(20, 15, 3), 255);
        CHECK_EQ(px(20, 2, 1), 100);   // away from the stroke
        // All channels active: the edit stands as it was made (the existing path).
        Document all = before;
        all.layers[0].asset = done.asset;
        all.layers[0].transform = done.transform;
        CHECK(!restrictToColorChannels(before, all, colorChannelsAll));
        CHECK(all.layers[0].asset->image == done.asset->image);
        // A blank layer painted in one channel stays blank: it has no alpha to carry the colour.
        Document blank = before;
        blank.layers[0] = Layer("Blank", Size(W, H));
        Document painted = blank;
        painted.layers[0].asset = done.asset;
        painted.layers[0].transform = done.transform;
        CHECK(restrictToColorChannels(blank, painted, 1));
        CHECK(!painted.layers[0].asset);
    }
}

TEST_CASE(channel_view) {
    Image frame(4, 2);
    for (int x = 0; x < 4; x++) for (int y = 0; y < 2; y++) { uint8_t* p = frame.pixel(x, y); p[0] = 200; p[1] = 100; p[2] = 50; p[3] = 255; }
    ChannelView view;
    CHECK(view.isDefault());
    Image same = frame;
    applyChannelView(same, Rect(0, 0, 4, 2), 1, view);
    CHECK(same == frame);   // the composite with nothing over it leaves the frame alone
    view.color = 2;          // green alone, in gray
    Image green = frame;
    applyChannelView(green, Rect(0, 0, 4, 2), 1, view);
    CHECK_EQ(int(green.pixel(1, 1)[0]), 100);
    CHECK_EQ(int(green.pixel(1, 1)[2]), 100);
    view.color = 5;          // red and blue: green left out
    Image two = frame;
    applyChannelView(two, Rect(0, 0, 4, 2), 1, view);
    CHECK_EQ(int(two.pixel(0, 0)[1]), 0);
    CHECK_EQ(int(two.pixel(0, 0)[0]), 200);
    // An alpha channel alone in gray, and as a 50% red overlay over its black.
    auto g = std::make_shared<GrayImage>(4, 2, 0);
    g->at(0, 0) = 255;
    view.color = 0;
    view.gray = GrayPtr(g);
    Image alone = frame;
    applyChannelView(alone, Rect(0, 0, 4, 2), 1, view);
    CHECK_EQ(int(alone.pixel(0, 0)[0]), 255);
    CHECK_EQ(int(alone.pixel(1, 0)[0]), 0);
    ChannelView overlay;
    overlay.overlays.push_back({GrayPtr(g), {1, 0, 0}, 0.5});
    Image tinted = frame;
    applyChannelView(tinted, Rect(0, 0, 4, 2), 1, overlay);
    CHECK_EQ(int(tinted.pixel(0, 0)[1]), 100);   // selected: untouched
    CHECK_EQ(int(tinted.pixel(1, 0)[1]), 50);    // masked: half red
    CHECK_EQ(int(tinted.pixel(1, 0)[0]), 228);
    // At half scale, each output pixel is the channel under its centre.
    Image half(2, 1);
    for (int x = 0; x < 2; x++) { uint8_t* p = half.pixel(x, 0); p[0] = p[1] = p[2] = 10; p[3] = 255; }
    applyChannelView(half, Rect(0, 0, 4, 2), 0.5, view);
    CHECK_EQ(int(half.pixel(0, 0)[0]), 0);   // the centre of (0, 0) falls on document pixel (1, 1)
}

namespace {

Document channelDocument(SampleType depth) {
    Document d = grayDocument(depth, 90);
    Channel a = makeAlphaChannel(d, "Alpha 1", rectSelection(Rect(0, 0, 20, H), depth).coverage);
    Channel b = makeAlphaChannel(d, "マスク範囲", rectSelection(Rect(5, 5, 10, 10), depth).coverage);
    b.color = {0, 0.5, 1};
    b.opacity = 0.3;
    setSelectedAreas(b, true);
    Channel spot = makeAlphaChannel(d, "PANTONE 185 C", rectSelection(Rect(0, 20, W, 10), depth).coverage);
    spot.kind = ChannelKind::Spot;
    spot.color = {0.9, 0.1, 0.2};
    spot.opacity = 0.8;
    d.channels = {a, b, spot};
    return d;
}

bool sameChannels(const std::vector<Channel>& x, const std::vector<Channel>& y, double colorTolerance) {
    if (x.size() != y.size()) return false;
    for (size_t i = 0; i < x.size(); i++) {
        if (x[i].name != y[i].name || x[i].kind != y[i].kind || x[i].selectedAreas != y[i].selectedAreas) return false;
        if (std::fabs(x[i].opacity - y[i].opacity) > 0.005) return false;
        for (size_t c = 0; c < 3; c++) if (std::fabs(x[i].color[c] - y[i].color[c]) > colorTolerance) return false;
        if (psdMaskHash(x[i].image, true) != psdMaskHash(y[i].image, true)) return false;
    }
    return true;
}

} // namespace

TEST_CASE(psd_round_trip) {
    for (SampleType depth : {SampleType::U8, SampleType::U16}) {
        const Document d = channelDocument(depth);
        std::string error;
        PsdExportSummary summary;
        const std::vector<uint8_t> file = encodePsd(d, PsdExportOptions(), &summary, &error);
        REQUIRE(!file.empty());
        auto imported = importPsdBytes(file, &error);
        REQUIRE(imported);
        CHECK(imported->document.sampleType == depth);
        CHECK(sameChannels(d.channels, imported->document.channels, 1.0 / 65535));
        CHECK_EQ(imported->document.layers.size(), size_t(1));
        // Written again untouched: the same file, byte for byte.
        const std::vector<uint8_t> again = encodePsd(imported->document, PsdExportOptions(), &summary, &error);
        CHECK(again == file);
        // A renamed channel changes only what names it.
        Document renamed = imported->document;
        renamed.channels[0].name = "Renamed";
        auto back = importPsdBytes(encodePsd(renamed, PsdExportOptions(), &summary, &error), &error);
        REQUIRE(back);
        CHECK_EQ(back->document.channels[0].name, std::string("Renamed"));
        CHECK(back->document.channels[1].psdCarry && back->document.channels[1].psdCarry->displayInfo == imported->document.channels[1].psdCarry->displayInfo);
    }
    // A 16-bit file's own samples (odd 16-bit values that 0..32768 cannot hold) come back as stored.
    Document deep = channelDocument(SampleType::U16);
    deep.channels.resize(1);
    PsdExportOptions raw;
    raw.compress = false;
    std::string error;
    std::vector<uint8_t> file = encodePsd(deep, raw, nullptr, &error);
    REQUIRE(file.size() > size_t(W * H * 2));
    for (size_t i = file.size() - size_t(W * H * 2); i < file.size(); i += 2) { file[i] = uint8_t(0x80 | (i % 7)); file[i + 1] = uint8_t(i % 251 | 1); }
    auto imported = importPsdBytes(file, &error);
    REQUIRE(imported);
    REQUIRE(imported->document.channels.size() == 1);
    CHECK(imported->document.channels[0].psdCarry && !imported->document.channels[0].psdCarry->plane16.empty());
    CHECK(encodePsd(imported->document, raw, nullptr, &error) == file);
    // Edited, the channel is written from its 15-bit values.
    Document edited = imported->document;
    saveSelectionInto(edited.channels[0], rectSelection(Rect(0, 0, 5, 5), SampleType::U16), SelectionMode::Replace, SampleType::U16, W, H);
    CHECK(encodePsd(edited, raw, nullptr, &error) != file);
    // A document without channels writes what it always did: four channels.
    const std::vector<uint8_t> plain = encodePsd(grayDocument(), PsdExportOptions(), nullptr, &error);
    REQUIRE(plain.size() > 14);
    CHECK_EQ(int(plain[12]) << 8 | plain[13], 4);
}

TEST_CASE(project_round_trip) {
    const fs::path dir = fs::temp_directory_path() / ("channels_tests_" + std::to_string(::getpid()));
    fs::create_directories(dir);
    for (SampleType depth : {SampleType::U8, SampleType::U16}) {
        Document d = channelDocument(depth);
        ProjectError error;
        const std::string path = (dir / (depth == SampleType::U16 ? "deep.comp" : "eight.comp")).string();
        REQUIRE(saveProject(d, std::nullopt, path, error));
        CHECK(fs::exists(fs::path(path) / "channels" / (d.channels[0].id + ".png")));
        auto loaded = loadProject(path, error);
        REQUIRE(loaded);
        CHECK(loaded->sampleType == depth);
        CHECK(sameChannels(d.channels, loaded->channels, 1e-12));
        CHECK_EQ(loaded->channels[1].id, d.channels[1].id);
        // A manifest with channels is version 8.
        const std::string manifest = manifestJson(d, std::nullopt);
        CHECK(manifest.find("\"version\": 8") != std::string::npos);
        CHECK(manifest.find("\"colorIndicates\": \"selected\"") != std::string::npos);
        // A channel whose gray is missing is a damaged project.
        fs::remove(fs::path(path) / "channels" / (d.channels[0].id + ".png"));
        CHECK(!loadProject(path, error));
    }
    // An older project, with no channels, loads as before; a version-7 manifest with channels is refused.
    Document plain = grayDocument();
    ProjectError error;
    const std::string manifest = manifestJson(plain, std::nullopt);
    CHECK(manifest.find("channels") == std::string::npos);
    auto parsed = parseManifest(manifest, error);
    REQUIRE(parsed);
    CHECK(parsed->channels.empty());
    std::string bad = manifest;
    const size_t at = bad.find("\"colorSpace\"");
    REQUIRE(at != std::string::npos);
    bad.insert(at, "\"channels\": [{\"id\": \"" + makeUuid() + "\", \"name\": \"A\"}], ");
    CHECK(!parseManifest(bad, error));
    fs::remove_all(dir);
}

TEST_MAIN()
