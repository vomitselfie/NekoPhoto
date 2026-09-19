// Golden image tests: known layer stacks rendered and compared with reference
// PNGs under tests/golden. Set COMPOSITOR_UPDATE_GOLDEN=1 to rewrite the
// references after an intentional rendering change. A small tolerance absorbs
// rounding differences between math paths.
#include "check.h"
#include "compositor/document.h"
#include "compositor/png.h"
#include "compositor/render.h"

#include <cstdlib>
#include <filesystem>
#include <string>

using namespace compositor;
namespace fs = std::filesystem;

namespace {

std::shared_ptr<Image> gradient(int w, int h) {
    auto img = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t* p = img->pixel(x, y);
            uint8_t a = uint8_t(255 * (x + 1) / w);
            p[0] = uint8_t(255 * x / (w - 1) * a / 255);
            p[1] = uint8_t(255 * y / (h - 1) * a / 255);
            p[2] = uint8_t(128 * a / 255);
            p[3] = a;
        }
    return img;
}

std::shared_ptr<Image> checker(int w, int h, int cell) {
    auto img = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            bool light = ((x / cell) + (y / cell)) % 2 == 0;
            uint8_t* p = img->pixel(x, y);
            p[0] = light ? 230 : 40; p[1] = light ? 230 : 40; p[2] = light ? 230 : 200; p[3] = 255;
        }
    return img;
}

Layer layerOf(const std::string& name, std::shared_ptr<Image> image, Point origin) { return Layer(Asset::make(image, name), origin); }

void compareGolden(const std::string& name, const Image& rendered) {
    fs::path dir(GOLDEN_DIR);
    fs::path file = dir / (name + ".png");
    bool update = std::getenv("COMPOSITOR_UPDATE_GOLDEN") != nullptr;
    std::string error;
    if (update || !fs::exists(file)) {
        fs::create_directories(dir);
        REQUIRE(writePngImage(file.string(), rendered, 0, &error));
        std::fprintf(stderr, "  wrote %s\n", file.string().c_str());
        return;
    }
    auto expected = readPngImage(file.string(), &error);
    REQUIRE(expected != nullptr);
    REQUIRE(expected->width() == rendered.width() && expected->height() == rendered.height());
    long worst = 0, over = 0;
    for (int y = 0; y < rendered.height(); y++)
        for (int x = 0; x < rendered.width(); x++)
            for (int c = 0; c < 4; c++) {
                long d = std::labs(long(rendered.pixel(x, y)[c]) - long(expected->pixel(x, y)[c]));
                worst = std::max(worst, d);
                if (d > 2) over++;
            }
    if (over > 0) check::fail(__FILE__, __LINE__, name + ": " + std::to_string(over) + " channel values differ by more than 2 (worst " + std::to_string(worst) + ")");
}

Document baseDocument() {
    Document doc(96, 64);
    doc.id = "00000000-0000-4000-8000-000000000001";
    doc.layers.push_back(layerOf("checker", checker(96, 64, 8), {0, 0}));
    return doc;
}

} // namespace

TEST_CASE(golden_normal_blend) {
    Document doc = baseDocument();
    doc.layers.push_back(layerOf("gradient", gradient(48, 32), {24, 16}));
    compareGolden("normal_blend", *renderFlattened(doc));
}

TEST_CASE(golden_blend_modes) {
    const BlendMode modes[] = {BlendMode::Multiply, BlendMode::Screen, BlendMode::Overlay, BlendMode::Darken, BlendMode::Lighten,
                               BlendMode::Difference, BlendMode::ColorDodge, BlendMode::ColorBurn, BlendMode::Hue, BlendMode::Saturation,
                               BlendMode::Color, BlendMode::Luminosity};
    for (BlendMode mode : modes) {
        Document doc = baseDocument();
        Layer top = layerOf("gradient", gradient(96, 64), {0, 0});
        top.blendMode = mode;
        top.opacity = 0.8;
        doc.layers.push_back(top);
        std::string name = blendModeName(mode);
        for (auto& ch : name) if (ch == ' ') ch = '_';
        for (auto& ch : name) ch = char(std::tolower(ch));
        compareGolden("blend_" + name, *renderFlattened(doc));
    }
}

TEST_CASE(golden_transformed_layer) {
    Document doc = baseDocument();
    Layer top = layerOf("gradient", gradient(32, 32), {20, 10});
    top.transform.size = {50, 40};
    top.transform.rotation = 25;
    top.transform.flipX = true;
    doc.layers.push_back(top);
    compareGolden("transformed_layer", *renderFlattened(doc));
    Layer nearest = top;
    nearest.transform.sampling = Sampling::Nearest;
    doc.layers.back() = nearest;
    compareGolden("transformed_layer_nearest", *renderFlattened(doc));
    Layer reduced = top;
    reduced.asset = Asset::make(gradient(256, 256), "big");
    reduced.transform.size = {30, 30};
    reduced.transform.rotation = 0;
    reduced.transform.flipX = false;
    doc.layers.back() = reduced;
    compareGolden("reduced_layer", *renderFlattened(doc));
}

TEST_CASE(golden_masked_layer) {
    Document doc = baseDocument();
    Layer top = layerOf("gradient", gradient(64, 48), {16, 8});
    auto mask = std::make_shared<GrayImage>(64, 48, 0);
    for (int y = 0; y < 48; y++) for (int x = 0; x < 64; x++) {
        double d = std::hypot(x - 32.0, y - 24.0);
        mask->at(x, y) = uint8_t(std::max(0.0, std::min(255.0, (28 - d) * 32)));
    }
    LayerMask m;
    m.asset = MaskAsset::make(mask);
    top.mask = m;
    doc.layers.push_back(top);
    compareGolden("masked_layer", *renderFlattened(doc));
    // The same mask placed apart from the layer.
    doc.layers.back().mask->placement = LayerTransform(Point(30, 16), Size(64, 48));
    compareGolden("placed_mask", *renderFlattened(doc));
}

TEST_CASE(golden_folder_mask_and_clipping) {
    Document doc = baseDocument();
    Layer group("Folder", doc.size());
    group.isGroup = true;
    auto fmask = std::make_shared<GrayImage>(96, 64, 0);
    for (int y = 0; y < 64; y++) for (int x = 0; x < 96; x++) fmask->at(x, y) = uint8_t(255 * x / 95);
    LayerMask fm;
    fm.asset = MaskAsset::make(fmask);
    group.mask = fm;
    Layer base = layerOf("base", gradient(40, 40), {10, 10});
    base.parentId = group.id;
    base.opacity = 0.9;
    Layer clipped = layerOf("clipped", checker(96, 64, 4), {0, 0});
    clipped.parentId = group.id;
    clipped.maskSourceId = base.id;
    clipped.blendMode = BlendMode::Screen;
    doc.layers.push_back(group);
    doc.layers.push_back(base);
    doc.layers.push_back(clipped);
    compareGolden("folder_mask_clipping", *renderFlattened(doc));
}

TEST_CASE(golden_zoomed_region) {
    Document doc = baseDocument();
    doc.layers.push_back(layerOf("gradient", gradient(48, 32), {24, 16}));
    Image out;
    RenderOptions options;
    options.region = {20, 12, 40, 30};
    options.scale = 3;
    render(doc, options, out);
    compareGolden("zoomed_region", out);
    options.region = {};
    options.scale = 0.25;
    render(doc, options, out);
    compareGolden("reduced_document", out);
}

TEST_MAIN()
