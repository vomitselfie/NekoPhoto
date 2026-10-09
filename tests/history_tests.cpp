// Undo history's region patches: pixel edits confined to part of a buffer keep only that part's crops, at 8 and
// 16 bits and in CMYK, and undo and redo give back the same bytes, also on layers made or removed by steps still in
// the history, and over random sequences of steps, undos, redos, merges and trims.
#include "check.h"
#include "compositor/document.h"
#include "compositor/history.h"
#include <cstring>
#include <map>
#include <random>

using namespace compositor;

namespace {

template <SampleType S> using Color = ImageOf<S>;
template <SampleType S> using Gray = GrayOf<S>;

template <class T>
void noise(T& image, uint32_t seed) {
    std::mt19937 rng(seed);
    const size_t bytes = image.byteCount() / size_t(image.height());
    for (int y = 0; y < image.height(); y++) {
        auto* row = reinterpret_cast<uint8_t*>(image.row(y));
        for (size_t i = 0; i < bytes; i++) row[i] = uint8_t(rng() >> 24);
    }
}

/// A copy of `buffer` with the rectangle's pixels changed (a stroke, a fill or a filter confined to it).
template <class T>
std::shared_ptr<const T> edited(const std::shared_ptr<const T>& buffer, int x, int y, int w, int h, uint32_t seed) {
    auto copy = std::make_shared<T>(*buffer);
    const size_t pixel = copy->byteCount() / (size_t(copy->width()) * size_t(copy->height()));
    std::mt19937 rng(seed);
    for (int j = y; j < y + h; j++) {
        auto* row = reinterpret_cast<uint8_t*>(copy->row(j)) + size_t(x) * pixel;
        for (size_t i = 0; i < size_t(w) * pixel; i++) row[i] = uint8_t(row[i] ^ (1 + (rng() >> 25)));
    }
    return copy;
}

template <SampleType S> const std::shared_ptr<const Color<S>>& colorOf(const AnyImage& a) {
    if constexpr (S == SampleType::U8) return a.u8(); else return a.u16();
}
template <SampleType S> const std::shared_ptr<const Gray<S>>& grayOf(const AnyGray& a) {
    if constexpr (S == SampleType::U8) return a.u8(); else return a.u16();
}

/// The pixels of every layer, mask, channel and the selection, by value.
struct Pixels {
    std::vector<std::vector<uint8_t>> buffers;
    bool operator==(const Pixels&) const = default;
};

template <class Any>
void append(Pixels& out, const Any& any) {
    std::vector<uint8_t> bytes;
    any.visit([&](const auto& p) {
        if (!p) return;
        const size_t rowBytes = p->byteCount() / size_t(p->height());
        for (int y = 0; y < p->height(); y++) {
            auto* row = reinterpret_cast<const uint8_t*>(p->row(y));
            bytes.insert(bytes.end(), row, row + rowBytes);
        }
    });
    out.buffers.push_back(std::move(bytes));
}

Pixels pixelsOf(const Document& doc) {
    Pixels out;
    for (const Layer& l : doc.layers) {
        append(out, l.asset ? l.asset->image : AnyImage());
        append(out, l.mask ? l.mask->asset.image : AnyGray());
    }
    for (const Channel& c : doc.channels) append(out, c.image);
    append(out, doc.selection ? doc.selection->coverage : AnyGray());
    return out;
}

template <SampleType S>
Document makeDocument(int w, int h) {
    Document doc(w, h);
    doc.sampleType = S;
    auto image = std::make_shared<Color<S>>(w, h);
    noise(*image, 1);
    doc.layers.push_back(Layer(Asset::makeAny(AnyImage(std::shared_ptr<const Color<S>>(image)), "A"), Point(0, 0)));
    auto mask = std::make_shared<Gray<S>>(w, h);
    noise(*mask, 2);
    doc.layers[0].mask = LayerMask{MaskAsset::makeAny(AnyGray(std::shared_ptr<const Gray<S>>(mask)))};
    Channel channel;
    channel.id = makeUuid();
    channel.name = "Alpha 1";
    auto alpha = std::make_shared<Gray<S>>(w, h);
    noise(*alpha, 3);
    channel.image = std::shared_ptr<const Gray<S>>(alpha);
    doc.channels.push_back(channel);
    Selection selection;
    auto coverage = std::make_shared<Gray<S>>(w, h);
    noise(*coverage, 4);
    selection.coverage = std::shared_ptr<const Gray<S>>(coverage);
    doc.selection = selection;
    return doc;
}

constexpr size_t bytesPer(SampleType s) { return s == SampleType::U8 ? 1 : 2; }

template <SampleType S>
void regionPatches() {
    const int w = 200, h = 120;
    const size_t sample = bytesPer(S);
    DocumentHistory history;
    Document doc = makeDocument<S>(w, h);
    const Uuid id = doc.layers[0].id;
    std::vector<Pixels> states{pixelsOf(doc)};
    auto step = [&](const char* name, auto&& change) {
        history.begin(name, doc, id);
        change();
        history.end(doc, id);
        states.push_back(pixelsOf(doc));
    };

    // A stroke: a 10 x 12 area of the layer. Only its crops are kept.
    step("Brush", [&] { doc.layers[0].asset->image = edited(colorOf<S>(doc.layers[0].asset->image), 30, 40, 10, 12, 10); });
    CHECK_EQ(history.patchCount(), 1);
    CHECK_EQ(history.retainedBytes(doc), size_t(2 * 10 * 12) * 4 * sample);
    // A fill of the mask, a filter over part of the alpha channel, a selection nudge.
    step("Fill", [&] { doc.layers[0].mask->asset.image = edited(grayOf<S>(doc.layers[0].mask->asset.image), 0, 0, 50, 50, 11); });
    step("Filter", [&] { doc.channels[0].image = edited(grayOf<S>(doc.channels[0].image), 100, 60, 60, 40, 12); });
    step("Select", [&] { doc.selection->coverage = edited(grayOf<S>(doc.selection->coverage), 5, 5, 1, 1, 13); });
    CHECK_EQ(history.patchCount(), 4);
    size_t expected = size_t(2 * 10 * 12) * 4 * sample + size_t(2 * 50 * 50 + 2 * 60 * 40 + 2) * sample;
    CHECK_EQ(history.retainedBytes(doc), expected);
    // Over half of the layer: kept whole, as before.
    step("Big filter", [&] { doc.layers[0].asset->image = edited(colorOf<S>(doc.layers[0].asset->image), 0, 0, 150, 100, 14); });
    CHECK_EQ(history.patchCount(), 4);
    expected += size_t(w) * size_t(h) * 4 * sample;
    CHECK_EQ(history.retainedBytes(doc), expected);
    // Two edits in one step: the layer and the mask, each its own patch.
    step("Both", [&] {
        doc.layers[0].asset->image = edited(colorOf<S>(doc.layers[0].asset->image), 190, 110, 10, 10, 15);
        doc.layers[0].mask->asset.image = edited(grayOf<S>(doc.layers[0].mask->asset.image), 0, 119, 200, 1, 16);
    });
    CHECK_EQ(history.patchCount(), 6);

    // Undo to the start and redo to the end: every state byte for byte.
    for (size_t i = states.size() - 1; i-- > 0;) {
        auto back = history.undo();
        REQUIRE(back && back->document);
        doc = *back->document;
        CHECK(pixelsOf(doc) == states[i]);
    }
    CHECK(!history.canUndo());
    for (size_t i = 1; i < states.size(); i++) {
        auto again = history.redo();
        REQUIRE(again && again->document);
        doc = *again->document;
        CHECK(pixelsOf(doc) == states[i]);
    }
    // Mixed: undo two, branch with a new stroke (the redo steps go), then all the way back and forth again.
    for (int i = 0; i < 2; i++) { doc = *history.undo()->document; states.pop_back(); }
    step("Branch", [&] { doc.layers[0].asset->image = edited(colorOf<S>(doc.layers[0].asset->image), 70, 70, 20, 5, 17); });
    CHECK(!history.canRedo());
    for (size_t i = states.size() - 1; i-- > 0;) { doc = *history.undo()->document; CHECK(pixelsOf(doc) == states[i]); }
    for (size_t i = 1; i < states.size(); i++) { doc = *history.redo()->document; CHECK(pixelsOf(doc) == states[i]); }
    // The undone buffers are the live document's again, so nothing is counted twice.
    size_t patched = 0;
    for (int i = 0; i < 3; i++) doc = *history.undo()->document;
    patched = history.retainedBytes(doc);
    for (int i = 0; i < 3; i++) doc = *history.redo()->document;
    CHECK_EQ(history.retainedBytes(doc), patched);
}

} // namespace

TEST_CASE(history_patches_regions_at_8_bits) { regionPatches<SampleType::U8>(); }
TEST_CASE(history_patches_regions_at_16_bits) { regionPatches<SampleType::U16>(); }

// A change made outside any step before the next one: the step before keeps its buffers whole, so undoing
// through both still gives back each state exactly.
TEST_CASE(history_patches_survive_changes_outside_steps) {
    DocumentHistory history;
    Document doc = makeDocument<SampleType::U8>(64, 64);
    const Uuid id = doc.layers[0].id;
    const Pixels start = pixelsOf(doc);
    history.begin("One", doc, id);
    doc.layers[0].asset->image = edited(doc.layers[0].asset->image.u8(), 1, 1, 4, 4, 1);
    history.end(doc, id);
    const Pixels one = pixelsOf(doc);
    CHECK_EQ(history.patchCount(), 1);
    doc.layers[0].asset->image = edited(doc.layers[0].asset->image.u8(), 10, 10, 4, 4, 2);   // not recorded
    const Pixels outside = pixelsOf(doc);
    history.begin("Two", doc, id);
    doc.layers[0].asset->image = edited(doc.layers[0].asset->image.u8(), 20, 20, 4, 4, 3);
    history.end(doc, id);
    CHECK_EQ(history.patchCount(), 0);   // the first step was made whole, and the second does not start where it ended
    doc = *history.undo()->document;
    CHECK(pixelsOf(doc) == outside);
    doc = *history.undo()->document;
    CHECK(pixelsOf(doc) == start);
    doc = *history.redo()->document;
    CHECK(pixelsOf(doc) == one);
    // An undo after an unrecorded change still restores the recorded state.
    Document other = doc;
    other.layers[0].asset->image = edited(other.layers[0].asset->image.u8(), 30, 30, 4, 4, 4);
    doc = *history.redo()->document;
    doc = *history.undo()->document;
    CHECK(pixelsOf(doc) == outside);
}

// Merged steps (a group of edits undone as one) resolve their patches through the chain.
TEST_CASE(history_squash_keeps_patches_exact) {
    DocumentHistory history;
    Document doc = makeDocument<SampleType::U16>(80, 80);
    const Uuid id = doc.layers[0].id;
    history.begin("Before", doc, id);
    doc.layers[0].name = "B";
    history.end(doc, id);
    const uint64_t since = history.revision();
    const Pixels start = pixelsOf(doc);
    for (int i = 0; i < 4; i++) {
        history.begin("Dab", doc, id);
        doc.layers[0].asset->image = edited(doc.layers[0].asset->image.u16(), 5 * i, 3 * i, 6, 6, uint32_t(i));
        if (i == 2) doc.channels[0].image = edited(doc.channels[0].image.u16(), 70, 70, 5, 5, 9);
        history.end(doc, id);
    }
    const Pixels end = pixelsOf(doc);
    CHECK_EQ(history.squash(since, "Stroke"), 4);
    CHECK_EQ(history.patchCount(), 2);
    CHECK_EQ(history.retainedBytes(doc), size_t(2 * 21 * 15 * 4 * 2 + 2 * 5 * 5 * 2));
    doc = *history.undo()->document;
    CHECK(pixelsOf(doc) == start);
    doc = *history.redo()->document;
    CHECK(pixelsOf(doc) == end);
}

// Many small strokes on a large layer: the history's memory follows the strokes' areas, not the layer's.
TEST_CASE(history_many_small_strokes_stay_proportional) {
    DocumentHistory history(1000, size_t(256) * 1024 * 1024);
    Document doc = makeDocument<SampleType::U8>(1000, 1000);
    const Uuid id = doc.layers[0].id;
    const Pixels start = pixelsOf(doc);
    std::mt19937 rng(7);
    size_t expected = 0;
    for (int i = 0; i < 500; i++) {
        const int x = int(rng() % 980), y = int(rng() % 980), size = 4 + int(rng() % 16);
        history.begin("Brush", doc, id);
        doc.layers[0].asset->image = edited(doc.layers[0].asset->image.u8(), x, y, size, size, uint32_t(i));
        history.end(doc, id);
        expected += size_t(2 * size * size * 4);
    }
    CHECK_EQ(history.undoCount(), 500);
    CHECK_EQ(history.retainedBytes(doc), expected);
    CHECK(expected < size_t(1000 * 1000 * 4));   // under one copy of the layer for 500 steps
    const Pixels end = pixelsOf(doc);
    while (history.canUndo()) doc = *history.undo()->document;
    CHECK(pixelsOf(doc) == start);
    while (history.canRedo()) doc = *history.redo()->document;
    CHECK(pixelsOf(doc) == end);
}

// ---- Layers made, replaced and removed by steps still in the history ------------------------------------------------
// The step that makes a layer has no buffer before it, so a later stroke's patch can be handed down to it on its
// after side only (a one-sided inherited slot); likewise the step that removes a layer, in the redo list, on its
// before side. The history then keeps the strokes' crops, not the layer.

namespace {

/// A colour buffer at `S` in the document's mode (5 samples a pixel for CMYK), filled with noise.
template <SampleType S>
AnyImage colorBuffer(int w, int h, bool cmyk, uint32_t seed) {
    if constexpr (S == SampleType::U8) {
        if (cmyk) { auto p = std::make_shared<ImageC8>(w, h, 5); noise(*p, seed); return AnyImage(ImageC8Ptr(p)); }
        auto p = std::make_shared<Image>(w, h);
        noise(*p, seed);
        return AnyImage(ImagePtr(p));
    } else {
        auto p = std::make_shared<Image16>(w, h, cmyk ? 5 : 4);
        noise(*p, seed);
        return AnyImage(Image16Ptr(p));
    }
}

template <SampleType S>
AnyGray grayBuffer(int w, int h, uint32_t seed) {
    auto p = std::make_shared<Gray<S>>(w, h);
    noise(*p, seed);
    return AnyGray(std::shared_ptr<const Gray<S>>(p));
}

/// A copy of any buffer with the rectangle's bytes changed.
template <class Any>
Any editedAny(const Any& any, int x, int y, int w, int h, uint32_t seed) {
    Any out;
    any.visit([&](const auto& p) {
        if (!p) return;
        using T = std::remove_const_t<typename std::decay_t<decltype(p)>::element_type>;
        out = Any(std::shared_ptr<const T>(edited<T>(p, x, y, w, h, seed)));
    });
    return out;
}

/// Bytes per pixel of a buffer.
template <class Any>
size_t pixelBytes(const Any& any) { return any.byteCount() / (size_t(any.width()) * size_t(any.height())); }

/// A layer (no thumbnail: the history counts only what the test made) and, with `mask`, a layer mask.
template <SampleType S>
Layer makeLayer(int w, int h, bool cmyk, bool mask, uint32_t seed) {
    Asset asset;
    asset.image = colorBuffer<S>(w, h, cmyk, seed);
    asset.name = "Layer " + std::to_string(seed);
    Layer layer(std::move(asset), Point(0, 0));
    if (mask) layer.mask = LayerMask{MaskAsset{grayBuffer<S>(w, h, seed + 1), nullptr}};
    return layer;
}

/// What a document is, for comparing: the layers' ids, names and visibility, and every buffer's bytes.
struct Ref {
    std::vector<std::string> layers;
    Pixels pixels;
    bool operator==(const Ref&) const = default;
};

Ref refOf(const Document& doc);
/// No document: no layers and no buffers.
Ref refOf(const std::optional<Document>& doc) { return doc ? refOf(*doc) : Ref{{"no document"}, {}}; }

Ref refOf(const Document& doc) {
    Ref r;
    for (const Layer& l : doc.layers) r.layers.push_back(l.id + "/" + l.name + (l.visible ? "+" : "-"));
    r.pixels = pixelsOf(doc);
    return r;
}

template <SampleType S>
Document startDocument(int w, int h, bool cmyk) {
    Document doc(w, h);
    doc.sampleType = S;
    if (cmyk) doc.colorMode = ColorMode::CMYK;
    doc.layers.push_back(makeLayer<S>(w, h, cmyk, true, 1));
    Channel channel;
    channel.id = makeUuid();
    channel.name = "Alpha 1";
    channel.image = grayBuffer<S>(w, h, 3);
    doc.channels.push_back(channel);
    Selection selection;
    selection.coverage = grayBuffer<S>(w, h, 4);
    doc.selection = selection;
    return doc;
}

/// Runs steps on a document and its history, keeping each recorded state to compare undo and redo with.
struct Bench {
    DocumentHistory history;
    std::optional<Document> doc;
    std::map<uint64_t, Ref> states;
    explicit Bench(std::optional<Document> d, int entryLimit = 100, size_t byteLimit = size_t(256) * 1024 * 1024)
        : history(entryLimit, byteLimit), doc(std::move(d)) { states[history.revision()] = refOf(doc); }
    /// A step that may make the document (`change` takes the optional).
    template <class F> void stepWhole(const char* name, F&& change) {
        history.begin(name, doc, std::nullopt);
        change(doc);
        history.end(doc, std::nullopt);
        states[history.revision()] = refOf(doc);
    }
    template <class F> void step(const char* name, F&& change) {
        history.begin(name, doc, std::nullopt);
        change(*doc);
        history.end(doc, std::nullopt);
        states[history.revision()] = refOf(*doc);
    }
    bool undo() {
        auto s = history.undo();
        if (!s) return false;
        doc = s->document;
        return refOf(doc) == states.at(history.revision());
    }
    bool redo() {
        auto s = history.redo();
        if (!s) return false;
        doc = s->document;
        return refOf(doc) == states.at(history.revision());
    }
    size_t retained() const { return history.retainedBytes(doc); }
    /// Undoes everything and redoes everything, every state exact; false at the first that is not. (Under a byte
    /// limit, undoing may trim steps, so the redos need not come back to where it started.)
    bool roundTrip() {
        while (history.canUndo()) if (!undo()) return false;
        while (history.canRedo()) if (!redo()) return false;
        return true;
    }
};

template <SampleType S>
void madeLayerKeepsCrops(bool cmyk) {
    const int w = 240, h = 160;
    Bench b(startDocument<S>(w, h, cmyk));
    const size_t layerBytes = size_t(w) * size_t(h) * (cmyk ? 5 : 4) * bytesPer(S);
    const size_t pixel = (cmyk ? 5 : 4) * bytesPer(S);
    b.step("New Layer", [&](Document& d) { d.layers.push_back(makeLayer<S>(w, h, cmyk, false, 20)); });
    CHECK_EQ(b.retained(), size_t(0));
    b.step("Brush", [&](Document& d) { d.layers[1].asset->image = editedAny(d.layers[1].asset->image, 30, 40, 20, 10, 21); });
    const size_t crops = 2 * 20 * 10 * pixel;
    // The stroke's crops, not the made layer's first raster.
    CHECK_EQ(b.retained(), crops);
    CHECK_EQ(b.history.patchCount(), 1);
    b.step("Brush", [&](Document& d) { d.layers[1].asset->image = editedAny(d.layers[1].asset->image, 100, 100, 8, 8, 22); });
    CHECK_EQ(b.retained(), crops + 2 * 8 * 8 * pixel);
    REQUIRE(b.undo());
    CHECK_EQ(b.retained(), crops + 2 * 8 * 8 * pixel);
    REQUIRE(b.undo());
    CHECK_EQ(b.retained(), crops + 2 * 8 * 8 * pixel);
    // Undoing the layer's making: the document lets the raster go, so the step keeps it for its redo.
    REQUIRE(b.undo());
    CHECK_EQ(b.retained(), layerBytes + crops + 2 * 8 * 8 * pixel);
    REQUIRE(b.redo());
    CHECK_EQ(b.retained(), crops + 2 * 8 * 8 * pixel);
    REQUIRE(b.redo());
    CHECK_EQ(b.retained(), crops + 2 * 8 * 8 * pixel);
    REQUIRE(b.redo());
    CHECK_EQ(b.retained(), crops + 2 * 8 * 8 * pixel);
    CHECK(b.roundTrip());
    // A layer made with a mask, both painted in one step.
    b.step("New Layer", [&](Document& d) { d.layers.push_back(makeLayer<S>(w, h, cmyk, true, 30)); });
    const size_t before = b.retained();
    b.step("Brush", [&](Document& d) {
        d.layers[2].asset->image = editedAny(d.layers[2].asset->image, 0, 0, 5, 5, 31);
        d.layers[2].mask->asset.image = editedAny(d.layers[2].mask->asset.image, 200, 150, 6, 4, 32);
    });
    CHECK_EQ(b.retained() - before, 2 * 5 * 5 * pixel + 2 * 6 * 4 * bytesPer(S));
    CHECK(b.roundTrip());
}

template <SampleType S>
void removedLayerKeepsCrops(bool cmyk) {
    const int w = 200, h = 120;
    Bench b(startDocument<S>(w, h, cmyk));
    const size_t pixel = (cmyk ? 5 : 4) * bytesPer(S);
    const size_t layerBytes = size_t(w) * size_t(h) * pixel, maskBytes = size_t(w) * size_t(h) * bytesPer(S);
    b.step("Brush", [&](Document& d) { d.layers[0].asset->image = editedAny(d.layers[0].asset->image, 10, 10, 12, 12, 40); });
    const size_t crops = 2 * 12 * 12 * pixel;
    b.step("Delete Layer", [&](Document& d) { d.layers.clear(); });
    // The removal keeps the layer and its mask: the document no longer has them.
    CHECK_EQ(b.retained(), crops + layerBytes + maskBytes);
    REQUIRE(b.undo());
    CHECK_EQ(b.retained(), crops);
    // Undoing the stroke: the removal, now to redo, waits on the stroke's redo for the layer's pixels.
    REQUIRE(b.undo());
    CHECK_EQ(b.retained(), crops);
    REQUIRE(b.redo());
    CHECK_EQ(b.retained(), crops);
    REQUIRE(b.redo());
    CHECK_EQ(b.retained(), crops + layerBytes + maskBytes);
    CHECK(b.roundTrip());
}

template <SampleType S>
void replacedBufferKeepsOneCopy(bool cmyk) {
    const int w = 200, h = 120;
    Bench b(startDocument<S>(w, h, cmyk));
    const size_t pixel = (cmyk ? 5 : 4) * bytesPer(S);
    const size_t layerBytes = size_t(w) * size_t(h) * pixel;
    b.step("Filter", [&](Document& d) { d.layers[0].asset->image = editedAny(d.layers[0].asset->image, 0, 0, w, h, 50); });
    CHECK_EQ(b.retained(), layerBytes);
    b.step("Brush", [&](Document& d) { d.layers[0].asset->image = editedAny(d.layers[0].asset->image, 50, 50, 10, 10, 51); });
    // The filter's before (needed to undo it) and the stroke's crops; its after waits on the stroke.
    CHECK_EQ(b.retained(), layerBytes + 2 * 10 * 10 * pixel);
    REQUIRE(b.undo());
    REQUIRE(b.undo());
    CHECK_EQ(b.retained(), layerBytes + 2 * 10 * 10 * pixel);   // now the filter's after, for its redo
    REQUIRE(b.redo());
    REQUIRE(b.redo());
    CHECK_EQ(b.retained(), layerBytes + 2 * 10 * 10 * pixel);
    CHECK(b.roundTrip());
}

/// The work-counters self-test's document built as the app builds it, inside the history: the first import makes the
/// document (the step before it has none), then three paint layers and a blank layer, each its own step, then a short
/// stroke on the photo.
template <SampleType S>
void selfTestScenario(bool cmyk) {
    const int w = 400, h = 300;
    Bench b(std::nullopt);
    const uint64_t empty = b.history.revision();
    const size_t pixel = (cmyk ? 5 : 4) * bytesPer(S);
    const size_t layerBytes = size_t(w) * size_t(h) * pixel;
    b.stepWhole("Import Image", [&](std::optional<Document>& d) {
        d = Document(w, h);
        d->sampleType = S;
        if (cmyk) d->colorMode = ColorMode::CMYK;
        d->layers.push_back(makeLayer<S>(w, h, cmyk, false, 60));
    });
    for (uint32_t l = 1; l < 5; l++)
        b.step("Add Layer", [&](Document& d) { d.layers.push_back(makeLayer<S>(w, h, cmyk, false, 60 + l)); });
    CHECK_EQ(b.retained(), size_t(0));
    b.step("Brush", [&](Document& d) { d.layers[0].asset->image = editedAny(d.layers[0].asset->image, 150, 140, 29, 8, 70); });
    const size_t crops = 2 * 29 * 8 * pixel;
    CHECK_EQ(b.retained(), crops);
    CHECK(b.roundTrip());
    CHECK_EQ(b.retained(), crops);
    // All the way back to no document: the first import's step keeps the photo as the stroke's undo left it, the
    // others their layers.
    for (int i = 0; i < 6; i++) REQUIRE(b.undo());
    CHECK(!b.doc);
    CHECK_EQ(b.retained(), 5 * layerBytes + crops);
    for (int i = 0; i < 6; i++) REQUIRE(b.redo());
    CHECK_EQ(b.retained(), crops);
    // A stroke on another layer, then the whole run merged into one step from no document.
    b.step("Brush", [&](Document& d) { d.layers[3].asset->image = editedAny(d.layers[3].asset->image, 1, 2, 3, 4, 71); });
    CHECK_EQ(b.retained(), crops + 2 * 3 * 4 * pixel);
    CHECK_EQ(b.history.squash(empty, "Everything"), 7);
    CHECK_EQ(b.retained(), size_t(0));
    CHECK(b.roundTrip());
    REQUIRE(b.undo());
    CHECK_EQ(b.retained(), 5 * layerBytes);
    REQUIRE(b.redo());
    CHECK_EQ(b.retained(), size_t(0));
}

/// Squashing a run that made a layer, and a run after it.
template <SampleType S>
void squashAroundMadeLayer(bool cmyk) {
    const int w = 200, h = 120;
    Bench b(startDocument<S>(w, h, cmyk));
    const size_t pixel = (cmyk ? 5 : 4) * bytesPer(S);
    const uint64_t first = b.history.revision();
    b.step("New Layer", [&](Document& d) { d.layers.push_back(makeLayer<S>(w, h, cmyk, false, 80)); });
    const uint64_t made = b.history.revision();
    for (int i = 0; i < 3; i++)
        b.step("Dab", [&](Document& d) { d.layers[1].asset->image = editedAny(d.layers[1].asset->image, 10 + 4 * i, 10, 6, 6, uint32_t(81 + i)); });
    CHECK_EQ(b.history.squash(made, "Stroke"), 3);
    CHECK_EQ(b.retained(), 2 * 14 * 6 * pixel);
    CHECK(b.roundTrip());
    CHECK_EQ(b.retained(), 2 * 14 * 6 * pixel);
    b.step("Dab", [&](Document& d) { d.layers[1].asset->image = editedAny(d.layers[1].asset->image, 100, 100, 3, 3, 90); });
    CHECK_EQ(b.history.squash(first, "All"), 3);
    // The merged step made the layer: undoing it needs nothing of it, redoing it the layer as it stands.
    CHECK_EQ(b.retained(), size_t(0));
    CHECK(b.roundTrip());
    REQUIRE(b.undo());
    CHECK_EQ(b.retained(), size_t(w) * size_t(h) * pixel);
    REQUIRE(b.redo());
    CHECK_EQ(b.retained(), size_t(0));
}

// ---- Random sequences ----------------------------------------------------------------------------------------------

/// What a step may keep at most, by the list it is in: the undo list (`past`) or the redo list (`future`).
struct Cost { size_t past = 0, future = 0; size_t wholeIfMerged = 0; };

template <SampleType S>
void randomSequence(bool cmyk, uint32_t seed, int entryLimit, size_t byteLimit, int operations) {
    const int w = 96, h = 64;
    const size_t pixel = (cmyk ? 5 : 4) * bytesPer(S), gray = bytesPer(S);
    const size_t layerBytes = size_t(w) * size_t(h) * pixel, maskBytes = size_t(w) * size_t(h) * gray;
    Bench b(startDocument<S>(w, h, cmyk), entryLimit, byteLimit);
    std::mt19937 rng(seed);
    auto pick = [&](int n) { return int(rng() % uint32_t(n)); };
    // The model of the history's steps: their revisions and costs, oldest first; the redo list next-redo last.
    std::vector<std::pair<uint64_t, Cost>> past, future;
    std::map<uint64_t, Cost> costs;   // by the revision a step ends at
    const bool limited = byteLimit < size_t(256) * 1024 * 1024;
    uint32_t serial = 1000;
    int failures = 0;
    bool trimmed = false;   // the operation recorded, undid or redid a step, so the history was trimmed after it
    auto record = [&](const char* name, Cost cost, auto&& change) {
        const uint64_t was = b.history.revision();
        b.step(name, change);
        if (b.history.revision() == was) return;   // nothing changed: nothing recorded
        past.emplace_back(b.history.revision(), cost);
        costs[b.history.revision()] = cost;
        future.clear();
        trimmed = true;
    };
    for (int op = 0; op < operations && failures == 0; op++) {
        Document& d = *b.doc;
        const int kind = pick(100);
        trimmed = false;
        if (kind < 12) {   // a new layer, with a mask now and then, anywhere in the stack
            const bool mask = pick(3) == 0;
            const size_t at = size_t(pick(int(d.layers.size()) + 1));
            Cost c{0, layerBytes + (mask ? maskBytes : 0), 0};
            record("New Layer", c, [&](Document& x) { x.layers.insert(x.layers.begin() + std::ptrdiff_t(at), makeLayer<S>(w, h, cmyk, mask, serial++)); });
        } else if (kind < 18 && d.layers.size() > 1) {   // a layer removed
            const size_t at = size_t(pick(int(d.layers.size())));
            Cost c{layerBytes + (d.layers[at].mask ? maskBytes : 0), 0, 0};
            record("Delete Layer", c, [&](Document& x) { x.layers.erase(x.layers.begin() + std::ptrdiff_t(at)); });
        } else if (kind < 50 && !d.layers.empty()) {   // a stroke: the layer's pixels, its mask, the channel or the selection
            const int where = pick(6);
            const int rw = 1 + pick(w / 3), rh = 1 + pick(h / 3), rx = pick(w - rw + 1), ry = pick(h - rh + 1);
            const size_t at = size_t(pick(int(d.layers.size())));
            const uint32_t s = serial++;
            if (where == 3 && d.layers[at].mask) {
                Cost c{2 * size_t(rw) * size_t(rh) * gray, 2 * size_t(rw) * size_t(rh) * gray, 2 * maskBytes};
                record("Mask", c, [&](Document& x) { x.layers[at].mask->asset.image = editedAny(x.layers[at].mask->asset.image, rx, ry, rw, rh, s); });
            } else if (where == 4) {
                Cost c{2 * size_t(rw) * size_t(rh) * gray, 2 * size_t(rw) * size_t(rh) * gray, 2 * maskBytes};
                record("Channel", c, [&](Document& x) { x.channels[0].image = editedAny(x.channels[0].image, rx, ry, rw, rh, s); });
            } else if (where == 5) {
                Cost c{2 * size_t(rw) * size_t(rh) * gray, 2 * size_t(rw) * size_t(rh) * gray, 2 * maskBytes};
                record("Select", c, [&](Document& x) { x.selection->coverage = editedAny(x.selection->coverage, rx, ry, rw, rh, s); });
            } else {
                Cost c{2 * size_t(rw) * size_t(rh) * pixel, 2 * size_t(rw) * size_t(rh) * pixel, 2 * layerBytes};
                record("Brush", c, [&](Document& x) { x.layers[at].asset->image = editedAny(x.layers[at].asset->image, rx, ry, rw, rh, s); });
            }
        } else if (kind < 56 && !d.layers.empty()) {   // a filter over the whole layer
            const size_t at = size_t(pick(int(d.layers.size())));
            const uint32_t s = serial++;
            record("Filter", Cost{layerBytes, layerBytes, 0}, [&](Document& x) { x.layers[at].asset->image = editedAny(x.layers[at].asset->image, 0, 0, w, h, s); });
        } else if (kind < 60 && !d.layers.empty()) {   // no pixels: a rename or a visibility change
            const size_t at = size_t(pick(int(d.layers.size())));
            const bool rename = pick(2) == 0;
            const uint32_t s = serial++;
            record("Rename", Cost{}, [&](Document& x) { if (rename) x.layers[at].name = "L" + std::to_string(s); else x.layers[at].visible = !x.layers[at].visible; });
        } else if (kind < 78) {
            if (b.history.canUndo()) {
                if (!b.undo()) { failures++; std::fprintf(stderr, "  undo at operation %d gave the wrong document\n", op); }
                future.push_back(past.back());
                past.pop_back();
                trimmed = true;
            }
        } else if (kind < 94) {
            if (b.history.canRedo()) {
                if (!b.redo()) { failures++; std::fprintf(stderr, "  redo at operation %d gave the wrong document\n", op); }
                past.push_back(future.back());
                future.pop_back();
                trimmed = true;
            }
        } else if (past.size() >= 3) {   // a run of steps merged into one
            const size_t k = 1 + size_t(pick(int(past.size()) - 2));
            const uint64_t since = past[k - 1].first;
            Cost merged;
            for (size_t i = k; i < past.size(); i++) {
                merged.past += past[i].second.past + past[i].second.wholeIfMerged;
                merged.future += past[i].second.future + past[i].second.wholeIfMerged;
                merged.wholeIfMerged += past[i].second.wholeIfMerged;
            }
            const int count = int(past.size() - k);
            CHECK_EQ(b.history.squash(since, "Merged"), count);
            const uint64_t last = past.back().first;
            past.resize(k);
            past.emplace_back(last, merged);
            costs[last] = merged;
        }
        // Trimming drops the oldest steps (the redo list's furthest when the undo list is empty).
        while (past.size() > size_t(b.history.undoCount())) past.erase(past.begin());
        while (future.size() > b.history.futureNames().size()) future.erase(future.begin());
        std::vector<uint64_t> revisions;
        for (auto& p : past) revisions.push_back(p.first);
        if (revisions != b.history.pastRevisions()) { failures++; std::fprintf(stderr, "  the model lost the history at operation %d\n", op); break; }
        CHECK(int(past.size() + future.size()) <= entryLimit);
        // The bound: each step's own cost. A layer made by a step still in the history costs nothing while that step
        // is in the undo list (the bug kept the whole layer there once it was painted on).
        size_t bound = 0;
        for (auto& p : past) bound += p.second.past;
        for (auto& f : future) bound += f.second.future;
        if (b.retained() > bound) {
            failures++;
            std::fprintf(stderr, "  operation %d: the history keeps %zu bytes, over the bound %zu\n", op, b.retained(), bound);
        }
        if (limited && trimmed) CHECK(b.retained() <= byteLimit);
        if (op % 37 == 36) {
            if (!b.roundTrip()) { failures++; std::fprintf(stderr, "  the round trip at operation %d failed (seed %u)\n", op, seed); }
            // Everything redone: the model is the undo list as the history has it.
            past.clear();
            future.clear();
            for (uint64_t r : b.history.pastRevisions()) past.emplace_back(r, costs.at(r));
        }
    }
    CHECK_EQ(failures, 0);
    CHECK(b.roundTrip());
}

} // namespace

TEST_CASE(history_made_layer_keeps_crops) {
    madeLayerKeepsCrops<SampleType::U8>(false);
    madeLayerKeepsCrops<SampleType::U16>(false);
    madeLayerKeepsCrops<SampleType::U8>(true);
    madeLayerKeepsCrops<SampleType::U16>(true);
}

TEST_CASE(history_removed_layer_keeps_crops_in_the_redo_list) {
    removedLayerKeepsCrops<SampleType::U8>(false);
    removedLayerKeepsCrops<SampleType::U16>(false);
    removedLayerKeepsCrops<SampleType::U8>(true);
    removedLayerKeepsCrops<SampleType::U16>(true);
}

TEST_CASE(history_replaced_buffer_keeps_one_copy) {
    replacedBufferKeepsOneCopy<SampleType::U8>(false);
    replacedBufferKeepsOneCopy<SampleType::U16>(true);
}

TEST_CASE(history_self_test_document_built_in_the_history) {
    selfTestScenario<SampleType::U8>(false);
    selfTestScenario<SampleType::U16>(false);
    selfTestScenario<SampleType::U8>(true);
    selfTestScenario<SampleType::U16>(true);
}

TEST_CASE(history_squash_around_a_made_layer) {
    squashAroundMadeLayer<SampleType::U8>(false);
    squashAroundMadeLayer<SampleType::U16>(true);
}

// A change made outside any step after a layer was made and painted: the stroke's step is made whole, and the
// layer's making still undoes and redoes exactly.
TEST_CASE(history_made_layer_survives_changes_outside_steps) {
    for (const bool strokeUndone : {false, true}) {
        DocumentHistory history;
        std::optional<Document> doc = startDocument<SampleType::U16>(64, 64, true);
        auto step = [&](const char* name, auto&& change) { history.begin(name, doc, std::nullopt); change(*doc); history.end(doc, std::nullopt); };
        auto paint = [&](int x, uint32_t seed) { doc->layers[1].asset->image = editedAny(doc->layers[1].asset->image, x, x, 4, 4, seed); };
        auto undo = [&] { auto s = history.undo(); if (s) doc = s->document; return s && doc ? refOf(*doc) : Ref(); };
        auto redo = [&] { auto s = history.redo(); if (s) doc = s->document; return s && doc ? refOf(*doc) : Ref(); };
        const Ref start = refOf(*doc);
        step("New Layer", [&](Document& d) { d.layers.push_back(makeLayer<SampleType::U16>(64, 64, true, false, 7)); });
        const Ref made = refOf(*doc);
        step("Brush", [&](Document&) { paint(1, 8); });
        const Ref stroke = refOf(*doc);
        if (strokeUndone) CHECK(undo() == made);   // the made layer's step is the last one when the change comes
        paint(10, 9);   // not recorded
        const Ref outside = refOf(*doc);
        step("Brush", [&](Document&) { paint(20, 10); });
        const Ref last = refOf(*doc);
        CHECK(undo() == outside);
        if (!strokeUndone) CHECK(undo() == made);
        CHECK(undo() == start);
        CHECK(!history.canUndo());
        CHECK(redo() == made);
        if (!strokeUndone) CHECK(redo() == stroke);
        CHECK(redo() == last);
        CHECK(!history.canRedo());
    }
}

TEST_CASE(history_random_sequences) {
    for (uint32_t seed = 1; seed <= 12; seed++) {
        randomSequence<SampleType::U8>(false, seed, 100, size_t(256) * 1024 * 1024, 400);
        randomSequence<SampleType::U16>(false, seed + 100, 100, size_t(256) * 1024 * 1024, 400);
        randomSequence<SampleType::U8>(true, seed + 200, 100, size_t(256) * 1024 * 1024, 400);
        randomSequence<SampleType::U16>(true, seed + 300, 100, size_t(256) * 1024 * 1024, 400);
    }
}

TEST_CASE(history_random_sequences_trimmed) {
    for (uint32_t seed = 1; seed <= 8; seed++) {
        // By count: at most six steps kept.
        randomSequence<SampleType::U8>(false, seed + 400, 6, size_t(256) * 1024 * 1024, 400);
        randomSequence<SampleType::U16>(true, seed + 500, 6, size_t(256) * 1024 * 1024, 400);
        // By bytes: a few layers' worth.
        randomSequence<SampleType::U8>(true, seed + 600, 100, 96 * 64 * 5 * 3, 400);
        randomSequence<SampleType::U16>(false, seed + 700, 100, 96 * 64 * 8 * 3, 400);
    }
}

TEST_MAIN()
