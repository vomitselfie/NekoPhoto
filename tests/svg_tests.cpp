// SVG import and export (svg.h): shapes become vector shape layers with their colours, transforms and fill rules;
// what the shape model cannot hold is handed on as raster parts; a document written to SVG reads back the same.
#include "check.h"
#include "compositor/render.h"
#include "compositor/svg.h"
#include <cmath>

using namespace compositor;

namespace {
std::optional<SvgImport> read(const std::string& text, std::string* error = nullptr) {
    return importSvg(std::vector<uint8_t>(text.begin(), text.end()), error);
}
bool near(double a, double b, double tolerance = 1e-6) { return std::abs(a - b) < tolerance; }
}

TEST_CASE(path_data_grammar) {
    auto p = parseSvgPathData("M10 20 L30 20 l0 10 h-20 z m50 0 c10 0 10 10 0 10 s-10 -10 0 -10 Q70 0 80 20 T100 20 A10 10 0 0 1 120 20");
    REQUIRE(p.has_value());
    REQUIRE(p->subpaths.size() == 2);
    CHECK(p->subpaths[0].closed);
    CHECK_EQ(int(p->subpaths[0].knots.size()), 4);
    CHECK(near(p->subpaths[0].knots[2].x, 30) && near(p->subpaths[0].knots[2].y, 30));
    CHECK(!p->subpaths[1].closed);
    CHECK(near(p->subpaths[1].knots.front().x, 60) && near(p->subpaths[1].knots.front().y, 20));
    CHECK(near(p->subpaths[1].knots.back().x, 120) && near(p->subpaths[1].knots.back().y, 20));
    CHECK(!parseSvgPathData("L10 10").has_value());
    CHECK(!parseSvgPathData("M0 0 C1 2").has_value());
}

TEST_CASE(shapes_become_vector_layers) {
    auto svg = read(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
        <rect id="box" x="10" y="10" width="50" height="30" fill="#ff0000" stroke="blue" stroke-width="4"/>
        <circle cx="120" cy="50" r="20" style="fill: rgb(0, 128, 0); opacity: 0.5"/>
        <line x1="0" y1="90" x2="200" y2="90" stroke="black" stroke-width="2" stroke-dasharray="4 2"/>
      </svg>)svg");
    REQUIRE(svg.has_value());
    const Document& doc = svg->document;
    CHECK_EQ(doc.width, 200); CHECK_EQ(doc.height, 100);
    REQUIRE(doc.layers.size() == 3);
    CHECK_EQ(svg->shapeLayers, 3);
    CHECK(svg->rasterParts.empty());
    CHECK_EQ(doc.layers[0].name, std::string("box"));
    auto rect = vectorShapeOf(doc.layers[0], doc);
    REQUIRE(rect.has_value());
    CHECK(rect->fill && rect->r == 255 && rect->g == 0 && rect->b == 0);
    CHECK(rect->stroke.enabled && near(rect->stroke.width, 4, 1e-3) && rect->stroke.b == 255 && rect->stroke.align == VectorStroke::Align::Center);
    const Rect box = pathBounds(rect->path);
    CHECK(near(box.x, 10, 1e-3) && near(box.y, 10, 1e-3) && near(box.width, 50, 1e-3) && near(box.height, 30, 1e-3));
    auto circle = vectorShapeOf(doc.layers[1], doc);
    REQUIRE(circle.has_value());
    CHECK(circle->g == 128 && !circle->stroke.enabled);
    CHECK(near(doc.layers[1].opacity, 0.5));
    auto line = vectorShapeOf(doc.layers[2], doc);
    REQUIRE(line.has_value());
    CHECK(!line->fill && line->stroke.enabled);
    REQUIRE(line->stroke.dashes.size() == 2);
    CHECK(near(line->stroke.dashes[0], 2, 1e-3) && near(line->stroke.dashes[1], 1, 1e-3));   // in stroke widths
    // It draws: red inside the box, blue on its edge, green (half) in the circle.
    auto out = renderFlattened(doc);
    CHECK(out->pixel(35, 25)[0] > 240 && out->pixel(35, 25)[3] == 255);
    CHECK(out->pixel(35, 10)[2] > 200);
    CHECK(out->pixel(120, 50)[3] > 110 && out->pixel(120, 50)[3] < 145);
}

TEST_CASE(transforms_viewbox_and_groups) {
    auto svg = read(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="400" height="200" viewBox="0 0 200 100">
        <g id="outer" transform="translate(10 5)" opacity="0.5">
          <rect x="0" y="0" width="10" height="10" transform="rotate(90 5 5)" fill="#00f"/>
          <g display="none"><rect width="5" height="5"/></g>
        </g>
      </svg>)svg");
    REQUIRE(svg.has_value());
    const Document& doc = svg->document;
    CHECK_EQ(doc.width, 400);
    REQUIRE(doc.layers.size() == 4);
    CHECK(doc.layers[0].isGroup && doc.layers[0].name == "outer" && near(doc.layers[0].opacity, 0.5) && !doc.layers[0].passThrough);
    CHECK(doc.layers[1].parentId == doc.layers[0].id);
    CHECK(doc.layers[2].isGroup && !doc.layers[2].visible && doc.layers[3].parentId == doc.layers[2].id);
    auto rect = vectorShapeOf(doc.layers[1], doc);
    REQUIRE(rect.has_value());
    // viewBox doubles; the rotation about its centre keeps the 10x10 square in place: (10,5)..(20,15) -> (20,10)..(40,30).
    const Rect b = pathBounds(rect->path);
    CHECK(near(b.x, 20, 1e-3) && near(b.y, 10, 1e-3) && near(b.width, 20, 1e-3) && near(b.height, 20, 1e-3));
}

TEST_CASE(fill_rules) {
    // Two nested squares wound the same way: non-zero fills the hole, even-odd leaves it.
    const std::string d = "M0 0H100V100H0Z M25 25H75V75H25Z";
    for (bool evenOdd : {false, true}) {
        auto svg = read(std::string(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"><path fill="black" fill-rule=")svg") + (evenOdd ? "evenodd" : "nonzero") + "\" d=\"" + d + "\"/></svg>");
        REQUIRE(svg.has_value());
        auto out = renderFlattened(svg->document);
        CHECK_EQ(int(out->pixel(50, 50)[3]), evenOdd ? 0 : 255);
        CHECK_EQ(int(out->pixel(10, 10)[3]), 255);
    }
    // Wound against its outline, the inner square is a hole under non-zero too, and an island in it fills.
    auto svg = read(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"><path d="M0 0H100V100H0Z M20 20V80H80V20Z M40 40H60V60H40Z"/></svg>)svg");
    REQUIRE(svg.has_value());
    auto out = renderFlattened(svg->document);
    CHECK_EQ(int(out->pixel(30, 30)[3]), 0);
    CHECK_EQ(int(out->pixel(50, 50)[3]), 255);
    CHECK_EQ(int(out->pixel(10, 50)[3]), 255);
}

TEST_CASE(css_classes_and_currentcolor) {
    auto svg = read(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="10" height="10">
        <style>.st0{fill:#123456} #special { fill: currentColor }</style>
        <rect class="st0" width="5" height="5"/>
        <rect id="special" color="#abcdef" width="5" height="5"/>
      </svg>)svg");
    REQUIRE(svg.has_value());
    auto a = vectorShapeOf(svg->document.layers[0], svg->document);
    auto b = vectorShapeOf(svg->document.layers[1], svg->document);
    REQUIRE(a && b);
    CHECK(a->r == 0x12 && a->g == 0x34 && a->b == 0x56);
    CHECK(b->r == 0xab && b->b == 0xef);
}

TEST_CASE(unsupported_content_becomes_raster_parts) {
    auto svg = read(R"svg(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="100" height="100">
        <defs><linearGradient id="g"><stop offset="0" stop-color="red"/><stop offset="1" stop-color="blue"/></linearGradient></defs>
        <g fill="green" transform="translate(5 5)">
          <rect width="10" height="10" fill="url(#g)"/>
          <text x="0" y="50">Hi &amp; bye</text>
          <rect x="50" width="10" height="10"/>
        </g>
      </svg>)svg");
    REQUIRE(svg.has_value());
    const Document& doc = svg->document;
    // The folder, one raster layer standing for the gradient rect and the text (neighbours merge), the plain rect.
    REQUIRE(doc.layers.size() == 3);
    REQUIRE(svg->rasterParts.size() == 1);
    CHECK(svg->rasterParts[0].layer == doc.layers[1].id);
    const std::string& part = svg->rasterParts[0].svg;
    CHECK(part.find("<linearGradient id=\"g\"") != std::string::npos);
    CHECK(part.find("fill=\"url(#g)\"") != std::string::npos);
    CHECK(part.find("Hi &amp; bye") != std::string::npos);
    CHECK(part.find("matrix(1 0 0 1 5 5)") != std::string::npos);
    CHECK(part.find("fill:green") != std::string::npos);   // the inherited fill restated
    CHECK(isVectorShapeLayer(doc.layers[2]));
    CHECK(!svg->notes.empty());
}

TEST_CASE(not_an_svg) {
    std::string error;
    CHECK(!read("<html><body/></html>", &error).has_value());
    CHECK(!error.empty());
    CHECK(!read("<svg", &error).has_value());
}

TEST_CASE(export_and_read_back) {
    Document doc(120, 80);
    auto pixels = std::make_shared<Image>(10, 10);
    pixels->fill(0, 0, 255, 255);
    doc.layers.push_back(Layer(Asset::make(pixels, "Pixels"), Point(100, 60)));
    Layer folder("Folder", doc.size());
    folder.isGroup = true;
    folder.opacity = 0.75;
    folder.passThrough = false;
    doc.layers.push_back(folder);
    VectorShape shape;
    shape.path = ellipsePath(Rect(10, 10, 40, 30));
    shape.r = 200; shape.g = 100; shape.b = 50;
    shape.stroke.enabled = true;
    shape.stroke.width = 3;
    shape.stroke.align = VectorStroke::Align::Inside;
    shape.stroke.r = 10;
    Layer ellipse("Ellipse & co", Size(1, 1));
    setVectorShape(ellipse, doc, shape);
    ellipse.parentId = folder.id;
    doc.layers.push_back(ellipse);
    VectorShape outlined;
    outlined.path = rectanglePath(Rect(60, 10, 30, 30));
    outlined.fill = false;
    outlined.stroke.enabled = true;
    outlined.stroke.fillEnabled = false;
    outlined.stroke.width = 2;
    outlined.stroke.align = VectorStroke::Align::Outside;
    Layer square("Square", Size(1, 1));
    setVectorShape(square, doc, outlined);
    square.parentId = folder.id;
    square.opacity = 0.5;
    doc.layers.push_back(square);
    Layer hidden("Hidden", Size(1, 1));
    setVectorShape(hidden, doc, outlined);
    hidden.visible = false;
    doc.layers.push_back(hidden);

    SvgExportSummary summary;
    const std::string text = writeSvg(doc, &summary);
    CHECK_EQ(summary.shapes, 2);
    CHECK_EQ(summary.images, 1);
    CHECK_EQ(summary.groups, 1);
    CHECK(text.find("Ellipse___co") != std::string::npos);
    CHECK(text.find("data:image/png;base64,") != std::string::npos);
    CHECK(text.find("Hidden") == std::string::npos);

    auto back = read(text);
    REQUIRE(back.has_value());
    const Document& d = back->document;
    CHECK_EQ(d.width, 120); CHECK_EQ(d.height, 80);
    REQUIRE(d.layers.size() == 4);
    CHECK_EQ(back->rasterParts.size(), size_t(1));   // the embedded PNG
    CHECK(d.layers[1].isGroup && near(d.layers[1].opacity, 0.75));
    auto e = vectorShapeOf(d.layers[2], d);
    REQUIRE(e.has_value());
    CHECK(e->r == 200 && e->g == 100 && e->b == 50);
    CHECK(e->stroke.enabled && e->stroke.align == VectorStroke::Align::Inside && near(e->stroke.width, 3, 1e-3) && e->stroke.r == 10);
    const Rect eb = pathBounds(e->path), ob = pathBounds(shape.path);
    CHECK(near(eb.x, ob.x, 1e-3) && near(eb.width, ob.width, 1e-3) && near(eb.height, ob.height, 1e-3));
    auto s = vectorShapeOf(d.layers[3], d);
    REQUIRE(s.has_value());
    CHECK(!s->fill && s->stroke.align == VectorStroke::Align::Outside && near(s->stroke.width, 2, 1e-3));
    CHECK(near(d.layers[3].opacity, 0.5));
    // Drawn the same: compare the vector parts of the two documents.
    Document original = doc, reread = d;
    original.layers.erase(original.layers.begin());
    reread.layers.erase(reread.layers.begin());
    auto a = renderFlattened(original), b = renderFlattened(reread);
    int worst = 0;
    for (int y = 0; y < a->height(); y++)
        for (int x = 0; x < a->width() * 4; x++) worst = std::max(worst, std::abs(int(a->row(y)[x]) - int(b->row(y)[x])));
    CHECK(worst <= 2);
}

TEST_MAIN()
