// Photoshop presets (presets.h): multi-stop gradient fills, and .grd, .pat and .asl files written and read back,
// with a document given the patterns a style uses.
#include "check.h"
#include "compositor/document.h"
#include "compositor/presets.h"
#include "compositor/psd_carry.h"
#include "compositor/shape.h"
#include <cmath>

using namespace compositor;

namespace {

/// A 101 x 1 linear fill from x = 0 to x = 100 over a transparent base: pixel x samples t = (x + 0.5) / 100.
Image ramp(const GradientStops& stops) {
    Image base(101, 1), out(101, 1);
    fillGradient(base, out, Affine::identity(), GradientShape::Linear, {0, 0}, {100, 0}, stops, 1, nullptr);
    return out;
}

GradientStops redGreenBlue() {
    GradientStops s;
    GradientColorStop red{0, {1, 0, 0}, 0.5f}, green{0.5f, {0, 1, 0}, 0.5f}, blue{1, {0, 0, 1}, 0.5f};
    s.colors = {red, green, blue};
    return s;
}

std::vector<uint8_t> checker(int w, int h) {
    std::vector<uint8_t> rgba(size_t(w) * size_t(h) * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t* p = &rgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
            p[0] = uint8_t(x * 40); p[1] = uint8_t(y * 60); p[2] = (x + y) % 2 ? 255 : 0; p[3] = x == 0 ? 128 : 255;
        }
    return rgba;
}

} // namespace

TEST_CASE(multi_stop_gradient_passes_through_each_stop) {
    const Image out = ramp(redGreenBlue());
    auto px = [&](int x) { return out.pixel(x, 0); };
    CHECK(px(0)[0] >= 250 && px(0)[1] <= 5);            // red end
    CHECK(px(50)[1] >= 245 && px(50)[0] <= 8 && px(50)[2] <= 8);   // green in the middle
    CHECK(px(100)[2] >= 250 && px(100)[1] <= 5);        // blue end
    CHECK_NEAR(px(25)[0], 127, 4);                      // half way red to green
    CHECK_NEAR(px(25)[1], 127, 4);
    CHECK_EQ(int(px(25)[2]), 0);
    for (int x = 0; x <= 100; x++) CHECK_EQ(int(px(x)[3]), 255);   // no opacity stops: opaque
}

TEST_CASE(gradient_midpoints_and_opacity_stops) {
    GradientStops s;
    s.colors = {GradientColorStop{0, {0, 0, 0}, 0.25f}, GradientColorStop{1, {1, 1, 1}, 0.5f}};
    s.alphas = {GradientAlphaStop{0, 1, 0.5f}, GradientAlphaStop{0.5f, 1, 0.5f}, GradientAlphaStop{1, 0, 0.5f}};
    float c[4];
    s.sample(0.25f, c);   // the midpoint: half way in colour
    CHECK_NEAR(c[0], 0.5, 1e-4);
    s.sample(0.625f, c);  // half the second run: 0.5 + 0.5 * (0.625 - 0.25) / 0.75 = 0.75
    CHECK_NEAR(c[0], 0.75, 1e-4);
    CHECK_NEAR(c[3], 0.75, 1e-4);   // opacity half way from 1 at 0.5 to 0 at 1
    s.sample(1, c);
    CHECK_NEAR(c[3], 0, 1e-6);
    // Reversed: the ends swap and the midpoint moves to mirror.
    GradientStops r = s;
    r.reverse();
    r.sample(0.75f, c);
    CHECK_NEAR(c[0], 0.5, 1e-4);
    r.sample(0, c);
    CHECK_NEAR(c[3], 0, 1e-6);
    CHECK_NEAR(c[0], 1, 1e-6);
    // Drawn: transparent at the far end over a transparent base.
    const Image out = ramp(s);
    CHECK_EQ(int(out.pixel(100, 0)[3]), 0);
    CHECK_EQ(int(out.pixel(10, 0)[3]), 255);
}

TEST_CASE(two_stop_api_is_unchanged) {
    GradientStops s;
    const float a[4] = {1, 0, 0, 1}, b[4] = {0, 0, 1, 0};
    for (int i = 0; i < 4; i++) { s.start[i] = a[i]; s.end[i] = b[i]; }
    float c[4];
    s.sample(0.3f, c);
    for (int i = 0; i < 4; i++) CHECK_NEAR(c[i], a[i] + (b[i] - a[i]) * 0.3f, 1e-6);
}

TEST_CASE(grd_round_trip_keeps_stops_and_colour_sources) {
    GradientPreset g;
    g.name = "Sunset";
    g.colors = {{0, {}, 0.5f, GradientPreset::Source::Foreground}, {0.4f, {255, 128, 0}, 0.3f, GradientPreset::Source::User},
                {1, {}, 0.5f, GradientPreset::Source::Background}};
    g.alphas = {{0, 1, 0.5f}, {1, 0.25f, 0.5f}};
    g.smoothness = 0.5f;
    GradientPreset plain;
    plain.name = "Black, White";
    plain.colors = {{0, {0, 0, 0}, 0.5f, GradientPreset::Source::User}, {1, {255, 255, 255}, 0.5f, GradientPreset::Source::User}};
    std::string error;
    std::vector<std::string> notes;
    auto read = readGrd(writeGrd({g, plain}), &error, &notes);
    REQUIRE(read.has_value());
    REQUIRE(read->size() == 2);
    const GradientPreset& back = (*read)[0];
    CHECK_EQ(back.name, std::string("Sunset"));
    REQUIRE(back.colors.size() == 3);
    CHECK(back.colors[0].source == GradientPreset::Source::Foreground);
    CHECK(back.colors[2].source == GradientPreset::Source::Background);
    CHECK_EQ(int(back.colors[1].color.g), 128);
    CHECK_NEAR(back.colors[1].location, 0.4, 1e-3);
    CHECK_NEAR(back.colors[1].midpoint, 0.3, 1e-3);
    CHECK_NEAR(back.alphas[1].opacity, 0.25, 1e-3);
    CHECK_NEAR(back.smoothness, 0.5, 1e-3);
    CHECK((*read)[1].alphas.size() == 2);   // stops given with none read back opaque
    // The foreground and background go in where the stops say.
    const float fg[3] = {0, 1, 0}, bg[3] = {0, 0, 1};
    const GradientStops stops = back.stops(fg, bg);
    float c[4];
    stops.sample(0, c);
    CHECK_NEAR(c[1], 1, 1e-6);
    stops.sample(1, c);
    CHECK_NEAR(c[2], 1, 1e-6);
    CHECK_NEAR(c[3], 0.25, 1e-3);
    // Not a gradients file.
    CHECK(!readGrd({'8', 'B', 'P', 'T', 0, 1}, &error));
    CHECK(!error.empty());
    CHECK(!readGrd({'8', 'B', 'G', 'R', 0, 3, 0, 0}, &error));   // Photoshop 5's version 3
}

TEST_CASE(pat_round_trip_keeps_pixels_and_ids) {
    const auto pixels = checker(5, 3);
    const PatternPreset made = makePattern("pattern-one", "Checks", 5, 3, pixels);
    std::vector<uint8_t> opaque(4 * 4 * 4, 200);
    for (size_t i = 3; i < opaque.size(); i += 4) opaque[i] = 255;
    const PatternPreset second = makePattern("pattern-two", "Grey", 4, 4, opaque);
    std::string error;
    auto read = readPat(writePat({made, second}), &error);
    REQUIRE(read.has_value());
    REQUIRE(read->size() == 2);
    CHECK_EQ((*read)[0].id, std::string("pattern-one"));
    CHECK_EQ((*read)[0].name, std::string("Checks"));
    CHECK_EQ((*read)[0].width, 5);
    CHECK_EQ((*read)[0].height, 3);
    CHECK((*read)[0].record == made.record);   // a .pat record and a 'Patt' record carry the same bytes
    auto tile = decodePattern((*read)[0]);
    REQUIRE(tile.has_value());
    CHECK_EQ(tile->width, 5);
    CHECK(tile->rgba == pixels);
    auto grey = decodePattern((*read)[1]);
    REQUIRE(grey.has_value());
    CHECK_EQ(int(grey->rgba[3]), 255);
    CHECK_EQ(int(grey->rgba[0]), 200);
    CHECK(!readPat({'8', 'B', 'P', 'T', 0, 1, 0, 0, 0, 0}, &error));   // no patterns
    CHECK(!readPat({1, 2, 3}, &error));
}

TEST_CASE(asl_round_trip_and_patterns_into_the_document) {
    StyleLibrary library;
    library.patterns = {makePattern("tex-1", "Texture", 4, 4, checker(4, 4))};
    StylePreset style;
    style.id = "style-id";
    style.name = "Shadowed";
    DropShadow shadow;
    shadow.color = {10, 20, 30};
    shadow.distance = 12;
    shadow.size = 7;
    style.style.dropShadows = {shadow};
    PatternOverlay overlay;
    overlay.patternId = "tex-1";
    overlay.opacity = 0.5f;
    style.style.patternOverlays = {overlay};
    StylePreset plain;
    plain.id = "plain-id";
    plain.name = "Red Stroke";
    Stroke stroke;
    stroke.color = {255, 0, 0};
    stroke.size = 4;
    plain.style.strokes = {stroke};
    library.styles = {style, plain};

    std::string error;
    std::vector<std::string> notes;
    auto read = readAsl(writeAsl(library), &error, &notes);
    REQUIRE(read.has_value());
    REQUIRE(read->styles.size() == 2);
    const StylePreset& back = read->styles[0];
    CHECK_EQ(back.name, std::string("Shadowed"));
    CHECK_EQ(back.id, std::string("style-id"));
    REQUIRE(back.style.dropShadows.size() == 1);
    CHECK_EQ(int(back.style.dropShadows[0].color.b), 30);
    CHECK_NEAR(back.style.dropShadows[0].distance, 12, 1e-3);
    CHECK_NEAR(back.style.dropShadows[0].size, 7, 1e-3);
    REQUIRE(back.style.patternOverlays.size() == 1);
    CHECK_EQ(back.style.patternOverlays[0].patternId, std::string("tex-1"));
    CHECK_NEAR(back.style.patternOverlays[0].opacity, 0.5, 1e-3);
    REQUIRE(read->styles[1].style.strokes.size() == 1);
    CHECK_NEAR(read->styles[1].style.strokes[0].size, 4, 1e-3);
    REQUIRE(read->patterns.size() == 1);
    CHECK_EQ(read->patterns[0].id, std::string("tex-1"));
    CHECK(stylePatternIds(back.style) == std::vector<std::string>{"tex-1"});

    // The document takes the style's pattern into a new 'Patt' block, once.
    Document doc(20, 20);
    CHECK(!doc.psdCarry);
    CHECK_EQ(addDocumentPatterns(doc, read->patterns), 1);
    REQUIRE(doc.psdCarry != nullptr);
    CHECK_EQ(doc.psdCarry->width, 20);
    auto patterns = documentPatterns(doc);
    REQUIRE(patterns && patterns->count("tex-1"));
    CHECK(patterns->at("tex-1").rgba == checker(4, 4));
    CHECK_EQ(addDocumentPatterns(doc, read->patterns), 0);
    const PatternPreset other = makePattern("tex-2", "Other", 2, 2, std::vector<uint8_t>(16, 255));
    CHECK_EQ(addDocumentPatterns(doc, {other}), 1);
    CHECK_EQ(documentPatterns(doc)->size(), size_t(2));
    int pattBlocks = 0;
    for (auto& g : doc.psdCarry->globals) pattBlocks += g.key == "Patt";
    CHECK_EQ(pattBlocks, 1);

    CHECK(!readAsl({0, 2, '8', 'B', 'S', 'X'}, &error));
}

TEST_MAIN()
