// Undo history's region patches: pixel edits confined to part of a buffer keep only that part's crops, at 8 and
// 16 bits, and undo and redo give back the same bytes.
#include "check.h"
#include "compositor/document.h"
#include "compositor/history.h"
#include <cstring>
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

TEST_MAIN()
