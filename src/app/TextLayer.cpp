#include "TextLayer.h"
#include "ImageConvert.h"
#include "compositor/smartobject.h"
#include "compositor/warpmesh.h"
#include <QBuffer>
#include <QFontDatabase>
#include <QImageReader>
#include <QFontInfo>
#include <QHash>
#include <QFontMetricsF>
#include <QImage>
#include <QGlyphRun>
#include <QPainter>
#include <QPainterPath>
#include <QRawFont>
#include <QTextLayout>
#include <algorithm>
#include <limits>
#include <cmath>

namespace app {

QString defaultTextFamily() { return QFontDatabase::systemFont(QFontDatabase::GeneralFont).family(); }

QFont fontFor(const compositor::LayerText& text) {
    QFont font(text.fontFamily.empty() ? defaultTextFamily() : QString::fromStdString(text.fontFamily));
    font.setPixelSize(std::max(1, int(std::lround(text.fontSize))));
    font.setBold(text.bold);
    font.setItalic(text.italic);
    font.setLetterSpacing(QFont::AbsoluteSpacing, text.letterSpacing);
    // Photoshop never hints glyphs (Patchy's calibration); hinting would move advances and stems off its layout.
    font.setHintingPreference(QFont::PreferNoHinting);
    return font;
}

namespace {

QFont runFont(const compositor::LayerText& text, const compositor::TextRun& run) {
    compositor::LayerText t = text;
    t.fontFamily = run.fontFamily; t.fontSize = run.fontSize; t.bold = run.bold; t.italic = run.italic; t.letterSpacing = run.letterSpacing;
    QFont font = fontFor(t);
    if (run.weight > 0) {
        font.setWeight(QFont::Weight(std::clamp(run.weight, 100, 900)));
        // A stand-in without the weight falls back to regular; a medium or heavier face reads closer as its bold.
        // (Qt reports the weight asked for, not the face's, so the family's faces are looked at.)
        if (run.weight >= 500) {
            const QString family = QFontInfo(font).family();
            bool has = false;
            for (const QString& style : QFontDatabase::styles(family)) has |= std::abs(QFontDatabase::weight(family, style) - run.weight) <= 50;
            if (!has) font.setWeight(QFont::Bold);
        }
    }
    if (run.caps != compositor::TextRun::Caps::Normal) font.setCapitalization(run.caps == compositor::TextRun::Caps::Small ? QFont::SmallCaps : QFont::AllUppercase);
    return font;
}

/// Text in several styles, or in a box: one QTextLayout a paragraph, each run its own format, and its lines placed
/// by Photoshop's rules.
struct RunLayout {
    std::vector<std::unique_ptr<QTextLayout>> paragraphs;
    struct Line { size_t paragraph; int index; double width, baseline, descent; bool shown; };
    std::vector<Line> lines;        // baselines below the text's top (the block's for point text, the box's for box text)
    double width = 0, firstAscent = 0, pitch = 0, bottom = 0;
    std::vector<QFont> fonts;       // each run's
};

RunLayout layoutRuns(const compositor::LayerText& text) {
    RunLayout out;
    const bool boxed = text.boxWidth > 0 && text.boxHeight > 0;
    const std::vector<compositor::TextRun> runs = compositor::textRuns(text);
    struct Span { int start, end; QTextCharFormat format; double leading, capHeight; };
    std::vector<Span> spans;
    int at = 0;
    for (const compositor::TextRun& run : runs) {
        const QFont font = runFont(text, run);
        out.fonts.push_back(font);
        QTextCharFormat f;
        f.setFont(font);
        f.setForeground(QColor::fromRgbF(float(std::clamp(run.red, 0.0, 1.0)), float(std::clamp(run.green, 0.0, 1.0)), float(std::clamp(run.blue, 0.0, 1.0))));
        f.setFontUnderline(run.underline);
        f.setFontStrikeOut(run.strikethrough);
        const QFontMetricsF metrics(font);
        if (run.baselineShift != 0 && metrics.height() > 0) f.setBaselineOffset(run.baselineShift / metrics.height() * 100);
        // Photoshop's leading, or the font's own line height as NekoPhoto spaces plain text.
        const double leading = run.leading > 0 ? run.leading : metrics.height() * std::clamp(text.lineSpacing, 0.1, 10.0);
        spans.push_back({at, at + run.length, f, leading, metrics.capHeight()});
        at += run.length;
    }
    const QFont base = out.fonts.empty() ? fontFor(text) : out.fonts.front();
    out.pitch = QFontMetricsF(base).height() * std::clamp(text.lineSpacing, 0.1, 10.0);
    const QString content = QString::fromStdString(text.text);
    const QStringList paragraphs = content.split('\n');
    int paragraphStart = 0;
    bool first = true;
    double baseline = 0;
    for (int p = 0; p < paragraphs.size(); p++) {
        const QString& para = paragraphs[p];
        auto layout = std::make_unique<QTextLayout>(para, base);
        QList<QTextLayout::FormatRange> formats;
        for (const Span& sp : spans) {
            const int s0 = std::max(sp.start, paragraphStart), s1 = std::min(sp.end, paragraphStart + int(para.size()));
            if (s1 > s0) formats.append({s0 - paragraphStart, s1 - s0, sp.format});
        }
        // An empty paragraph keeps the height of the style it sits in.
        if (para.isEmpty())
            for (const Span& sp : spans) if (sp.start <= paragraphStart && paragraphStart <= sp.end) { layout->setFont(sp.format.font()); break; }
        layout->setFormats(formats);
        QTextOption option;
        option.setWrapMode(boxed ? QTextOption::WrapAtWordBoundaryOrAnywhere : QTextOption::NoWrap);
        option.setTabStopDistance(36);   // Photoshop's default tab stops: every half inch, 36 points
        layout->setTextOption(option);
        layout->beginLayout();
        std::vector<QTextLine> made;
        for (;;) {
            QTextLine l = layout->createLine();
            if (!l.isValid()) break;
            l.setLineWidth(boxed ? text.boxWidth : 1e7);
            made.push_back(l);
        }
        layout->endLayout();
        for (QTextLine& l : made) {
            // The runs on this line: their largest leading spaces it from the line before (Photoshop's rule), and in
            // a box the first line's largest cap height sets the first baseline.
            const int from = paragraphStart + l.textStart(), to = from + l.textLength();
            double leading = 0, cap = 0;
            for (const Span& sp : spans) {
                const bool touches = l.textLength() == 0 ? (sp.start <= from && from <= sp.end) : (sp.start < to && sp.end > from);
                if (touches) { leading = std::max(leading, sp.leading); cap = std::max(cap, sp.capHeight); }
            }
            if (leading <= 0) leading = out.pitch;
            if (first) {
                baseline = boxed ? (cap > 0 ? cap : l.ascent()) : l.ascent();
                out.firstAscent = baseline;
                first = false;
            } else baseline += leading;
            // A box shows only the lines that fit it whole.
            const bool shown = !boxed || baseline + l.descent() <= text.boxHeight + 0.5;
            out.lines.push_back({out.paragraphs.size(), l.lineNumber(), l.naturalTextWidth(), baseline, l.descent(), shown});
            if (shown) { out.width = std::max(out.width, l.naturalTextWidth()); out.bottom = std::max(out.bottom, baseline + l.descent()); }
        }
        out.paragraphs.push_back(std::move(layout));
        paragraphStart += int(para.size()) + 1;
    }
    if (boxed) { out.width = text.boxWidth; out.bottom = text.boxHeight; }
    return out;
}

/// What text is painted into: 8 bits per channel, or Qt's 16 bits per channel for a 16-bit document (Qt composites
/// the glyphs and colours at 16 bits there; its glyph coverage is 8-bit either way).
QImage::Format textFormat(bool deep) { return deep ? QImage::Format_RGBA64_Premultiplied : QImage::Format_ARGB32_Premultiplied; }

QImage renderRuns(const compositor::LayerText& text, bool deep) {
    RunLayout laid = layoutRuns(text);
    const int w = std::max(1, int(std::ceil(laid.width)) + 2 * textPadding);
    const int h = std::max(1, int(std::ceil(laid.bottom)) + 2 * textPadding);
    if ((long long)w * h > compositor::Document::pixelBudget) return {};
    QImage image(w, h, textFormat(deep));
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    for (const RunLayout::Line& line : laid.lines) {
        QTextLine l = laid.paragraphs[line.paragraph]->lineAt(line.index);
        const double x = text.alignment == 1 ? (laid.width - line.width) / 2 : text.alignment == 2 ? laid.width - line.width : 0;
        // A line a box hides goes far out of sight.
        l.setPosition(line.shown ? QPointF(x, line.baseline - l.ascent()) : QPointF(0, -1e6));
    }
    for (auto& paragraph : laid.paragraphs) paragraph->draw(&painter, QPointF(textPadding, textPadding));
    painter.end();
    return image;
}

QImage renderUpright(const compositor::LayerText& text, bool deep);

} // namespace

TextCaretGeometry textCaretGeometry(const compositor::LayerText& text) {
    // The runs' layout, as renderRuns places it: plain text draws the same lines (one font, no wrap), so its caret
    // comes from the same layout.
    TextCaretGeometry out;
    const RunLayout laid = layoutRuns(text);
    const QString content = QString::fromStdString(text.text);
    const int length = int(content.size());
    out.caretX.assign(size_t(length) + 1, textPadding);
    out.lineOf.assign(size_t(length) + 1, 0);
    // Where each paragraph starts in the whole text.
    std::vector<int> paragraphStart;
    int start = 0;
    for (const QString& para : content.split('\n')) { paragraphStart.push_back(start); start += int(para.size()) + 1; }
    for (size_t i = 0; i < laid.lines.size(); i++) {
        const RunLayout::Line& line = laid.lines[i];
        const QTextLine l = laid.paragraphs[line.paragraph]->lineAt(line.index);
        const double x = textPadding + (text.alignment == 1 ? (laid.width - line.width) / 2 : text.alignment == 2 ? laid.width - line.width : 0);
        const int base = paragraphStart[line.paragraph];
        TextCaretGeometry::Line g;
        g.start = base + l.textStart();
        g.end = base + l.textStart() + l.textLength();
        // A wrapped line's trailing space stays on it; the next line starts after it.
        g.baseline = textPadding + line.baseline;
        g.top = g.baseline - l.ascent();
        g.bottom = g.baseline + l.descent();
        g.left = x;
        g.right = x + line.width;
        g.shown = line.shown;
        const int lineIndex = int(out.lines.size());
        out.lines.push_back(g);
        const bool lastOfParagraph = line.index == laid.paragraphs[line.paragraph]->lineCount() - 1;
        for (int p = g.start; p <= g.end && p <= length; p++) {
            // A soft wrap's end position belongs to the next line.
            if (p == g.end && !lastOfParagraph) break;
            out.caretX[size_t(p)] = x + l.cursorToX(p - base);
            out.lineOf[size_t(p)] = lineIndex;
        }
    }
    if (out.lines.empty()) out.lines.push_back({0, 0, double(textPadding), textPadding + text.fontSize * 0.8, textPadding + text.fontSize, double(textPadding), double(textPadding), true});
    return out;
}

int TextCaretGeometry::positionAt(QPointF raster) const {
    if (lines.empty()) return 0;
    // The line under the point (or the nearest shown one), then the nearest caret on it.
    int best = 0;
    double bestDistance = 1e300;
    for (int i = 0; i < int(lines.size()); i++) {
        if (!lines[size_t(i)].shown) continue;
        const Line& l = lines[size_t(i)];
        const double d = raster.y() < l.top ? l.top - raster.y() : raster.y() > l.bottom ? raster.y() - l.bottom : 0;
        if (d < bestDistance) { bestDistance = d; best = i; }
    }
    int position = lines[size_t(best)].start;
    double nearest = 1e300;
    for (int p = 0; p < int(caretX.size()); p++) {
        if (lineOf[size_t(p)] != best) continue;
        const double d = std::abs(caretX[size_t(p)] - raster.x());
        if (d < nearest) { nearest = d; position = p; }
    }
    return position;
}

std::pair<QPointF, QPointF> TextCaretGeometry::caret(int position) const {
    position = std::clamp(position, 0, int(caretX.size()) - 1);
    const Line& l = lines[size_t(std::clamp(lineOf[size_t(position)], 0, int(lines.size()) - 1))];
    const double x = caretX[size_t(position)];
    return {QPointF(x, l.top), QPointF(x, l.bottom)};
}

std::vector<QRectF> TextCaretGeometry::selection(int from, int to) const {
    std::vector<QRectF> rects;
    if (from > to) std::swap(from, to);
    from = std::clamp(from, 0, int(caretX.size()) - 1);
    to = std::clamp(to, 0, int(caretX.size()) - 1);
    if (from == to) return rects;
    for (int i = 0; i < int(lines.size()); i++) {
        const Line& l = lines[size_t(i)];
        if (!l.shown || to < l.start || from > l.end) continue;
        const int a = std::max(from, l.start), b = std::min(to, l.end);
        const double x0 = lineOf[size_t(a)] == i ? caretX[size_t(a)] : l.left;
        // A selection that runs on past the line's end covers a little of its break, as text editors show it.
        double x1 = lineOf[size_t(b)] == i ? caretX[size_t(b)] : l.right;
        if (to > l.end) x1 = std::max(x1, l.right + (l.bottom - l.top) * 0.25);
        if (x1 > x0) rects.emplace_back(QPointF(x0, l.top), QPointF(x1, l.bottom));
    }
    return rects;
}

int TextCaretGeometry::verticalMove(int position, int step, double x) const {
    position = std::clamp(position, 0, int(caretX.size()) - 1);
    const int line = lineOf[size_t(position)] + step;
    if (line < 0) return 0;
    if (line >= int(lines.size())) return int(caretX.size()) - 1;
    const Line& l = lines[size_t(line)];
    return positionAt(QPointF(x, (l.top + l.bottom) / 2));
}

namespace {

/// Warp Text over the upright raster, at either depth.
template <class Img>
std::shared_ptr<Img> bendText(const compositor::LayerText& text, std::shared_ptr<Img> upright, QPointF* warpOffset) {
    if (!upright || !text.warp.active()) return upright;
    // Warp Text: the preset bent over the layout box (the block's lines, or the frame), as Photoshop re-derives it
    // from its own layout; ink past the box follows the patch's extension.
    auto m = psdTextMetrics(text);
    if (!m) return upright;
    const bool boxed = text.boxWidth > 0 && text.boxHeight > 0;
    const compositor::Rect box(m->blockLeft, m->blockTop, boxed ? text.boxWidth : m->blockWidth, boxed ? text.boxHeight : m->lineHeight * std::max(1, m->lines));
    auto mesh = compositor::styleWarpMesh(text.warp.style, text.warp.bend, text.warp.verticalOrientation, box.width, box.height);
    if (!mesh) return upright;
    compositor::distortWarpMesh(*mesh, text.warp.horizontal, text.warp.vertical);
    auto bent = compositor::renderWarpedOverBox(*upright, *mesh, box);
    if (!bent) return upright;
    if (warpOffset) *warpOffset = QPointF(bent->transform.origin.x, bent->transform.origin.y);
    return std::const_pointer_cast<Img>(std::shared_ptr<const Img>(bent->image));
}

} // namespace

std::shared_ptr<compositor::Image> renderTextLayer(const compositor::LayerText& text, QPointF* warpOffset) {
    if (warpOffset) *warpOffset = QPointF(0, 0);
    const QImage upright = renderUpright(text, false);
    return bendText(text, upright.isNull() ? nullptr : fromQImage(upright), warpOffset);
}

std::shared_ptr<compositor::Image16> renderTextLayer16(const compositor::LayerText& text, QPointF* warpOffset) {
    if (warpOffset) *warpOffset = QPointF(0, 0);
    const QImage upright = renderUpright(text, true);
    return bendText(text, upright.isNull() ? nullptr : fromQImage16(upright), warpOffset);
}

compositor::AnyImage renderTextLayerAt(const compositor::LayerText& text, compositor::SampleType type, QPointF* warpOffset) {
    if (type == compositor::SampleType::U16) {
        auto image = renderTextLayer16(text, warpOffset);
        return image ? compositor::AnyImage(compositor::Image16Ptr(image)) : compositor::AnyImage();
    }
    auto image = renderTextLayer(text, warpOffset);
    return image ? compositor::AnyImage(compositor::ImagePtr(image)) : compositor::AnyImage();
}

namespace {

QImage renderUpright(const compositor::LayerText& text, bool deep) {
    if (!text.runs.empty() || (text.boxWidth > 0 && text.boxHeight > 0)) return renderRuns(text, deep);
    const QFont font = fontFor(text);
    const QFontMetricsF metrics(font);
    QString content = QString::fromStdString(text.text);
    if (content.isEmpty()) content = QStringLiteral(" ");
    const QStringList lines = content.split('\n');
    const double lineHeight = metrics.height() * std::clamp(text.lineSpacing, 0.1, 10.0);
    double width = 0;
    for (const QString& line : lines) width = std::max(width, metrics.horizontalAdvance(line));
    const int w = std::max(1, int(std::ceil(width)) + 2 * textPadding);
    const int h = std::max(1, int(std::ceil(lineHeight * lines.size())) + 2 * textPadding);
    if ((long long)w * h > compositor::Document::pixelBudget) return {};
    QImage image(w, h, textFormat(deep));
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setFont(font);
    painter.setPen(QColor::fromRgbF(float(std::clamp(text.red, 0.0, 1.0)), float(std::clamp(text.green, 0.0, 1.0)), float(std::clamp(text.blue, 0.0, 1.0))));
    for (int i = 0; i < lines.size(); i++) {
        const double lineWidth = metrics.horizontalAdvance(lines[i]);
        const double x = textPadding + (text.alignment == 1 ? (width - lineWidth) / 2 : text.alignment == 2 ? width - lineWidth : 0);
        const double baseline = textPadding + i * lineHeight + metrics.ascent();
        painter.drawText(QPointF(x, baseline), lines[i]);
    }
    painter.end();
    return image;
}

} // namespace

namespace {

/// A Qt outline as path subpaths: each closed figure a subpath of corner and curve knots, `group` given.
void appendOutline(const QPainterPath& outline, int32_t group, compositor::VectorPath& out) {
    compositor::VectorPath::Subpath current;
    auto flush = [&] {
        auto& k = current.knots;
        // A figure that returns to its start: the last knot is the first (its incoming handle goes to the first).
        if (k.size() > 1 && std::abs(k.back().x - k.front().x) < 1e-9 && std::abs(k.back().y - k.front().y) < 1e-9) {
            k.front().inX = k.back().inX; k.front().inY = k.back().inY;
            k.pop_back();
        }
        if (k.size() >= 2) { current.closed = true; current.op = compositor::VectorPath::Op::Add; current.group = group; out.subpaths.push_back(current); }
        current = {};
    };
    for (int i = 0; i < outline.elementCount(); i++) {
        const QPainterPath::Element e = outline.elementAt(i);
        if (e.isMoveTo()) { flush(); current.knots.push_back({e.x, e.y, e.x, e.y, e.x, e.y}); }
        else if (e.isLineTo()) current.knots.push_back({e.x, e.y, e.x, e.y, e.x, e.y});
        else if (e.isCurveTo() && i + 2 < outline.elementCount() && !current.knots.empty()) {
            const QPainterPath::Element c2 = outline.elementAt(i + 1), end = outline.elementAt(i + 2);
            current.knots.back().outX = e.x; current.knots.back().outY = e.y;
            current.knots.push_back({c2.x, c2.y, end.x, end.y, end.x, end.y});
            i += 2;
        }
    }
    flush();
}

} // namespace

std::optional<compositor::VectorPath> textLayerOutline(const compositor::Layer& layer, QString* error) {
    auto fail = [&](const QString& why) { if (error) *error = why; return std::nullopt; };
    if (!layer.isLiveText() || !layer.asset || !layer.asset->image) return fail(QObject::tr("The layer is not a text layer."));
    const compositor::LayerText& text = *layer.text;
    if (text.warp.active()) return fail(QObject::tr("Warped text cannot be made a path here; set its warp to None first."));
    // The glyphs where renderUpright puts them in the layer's raster, one shape group each (overlapping letters
    // add; a letter's own holes fill even-odd).
    std::vector<QPainterPath> glyphs;
    auto collect = [&](const QTextLayout& layout, QPointF offset) {
        for (const QGlyphRun& run : layout.glyphRuns()) {
            const QRawFont font = run.rawFont();
            const auto indexes = run.glyphIndexes();
            const auto positions = run.positions();
            for (int i = 0; i < indexes.size() && i < positions.size(); i++) {
                const QPointF at = positions[i] + layout.position() + offset;
                if (at.y() < -1e5) continue;   // a line the box hides
                QPainterPath g = font.pathForGlyph(indexes[i]);
                if (g.isEmpty()) continue;
                g.translate(at);
                glyphs.push_back(g);
            }
        }
    };
    if (!text.runs.empty() || (text.boxWidth > 0 && text.boxHeight > 0)) {
        RunLayout laid = layoutRuns(text);
        for (const RunLayout::Line& line : laid.lines) {
            QTextLine l = laid.paragraphs[line.paragraph]->lineAt(line.index);
            const double x = text.alignment == 1 ? (laid.width - line.width) / 2 : text.alignment == 2 ? laid.width - line.width : 0;
            l.setPosition(line.shown ? QPointF(x, line.baseline - l.ascent()) : QPointF(0, -1e6));
        }
        for (auto& paragraph : laid.paragraphs) collect(*paragraph, QPointF(textPadding, textPadding));
    } else {
        const QFont font = fontFor(text);
        const QFontMetricsF metrics(font);
        const QStringList lines = QString::fromStdString(text.text).split('\n');
        const double lineHeight = metrics.height() * std::clamp(text.lineSpacing, 0.1, 10.0);
        double width = 0;
        for (const QString& line : lines) width = std::max(width, metrics.horizontalAdvance(line));
        for (int i = 0; i < lines.size(); i++) {
            const double lineWidth = metrics.horizontalAdvance(lines[i]);
            const double x = textPadding + (text.alignment == 1 ? (width - lineWidth) / 2 : text.alignment == 2 ? width - lineWidth : 0);
            const double baseline = textPadding + i * lineHeight + metrics.ascent();
            // Shaped as drawText shapes it, glyph by glyph.
            QTextLayout layout(lines[i], font);
            layout.beginLayout();
            QTextLine l = layout.createLine();
            if (l.isValid()) l.setLineWidth(1e7);
            layout.endLayout();
            if (!l.isValid()) continue;
            l.setPosition(QPointF(0, 0));
            collect(layout, QPointF(x, baseline - l.ascent()));
        }
    }
    compositor::VectorPath path;
    for (size_t i = 0; i < glyphs.size(); i++) appendOutline(glyphs[i], int32_t(i), path);
    if (path.subpaths.empty()) return fail(QObject::tr("The text has no outlines (only spaces?)."));
    // From the raster to the document, through the layer's placement (moved, scaled or turned).
    const int w = layer.asset->image.width(), h = layer.asset->image.height();
    for (auto& s : path.subpaths)
        for (auto& k : s.knots) {
            auto map = [&](double& x, double& y) { const compositor::Point p = compositor::mapThroughTransform(layer.transform, w, h, x, y); x = p.x; y = p.y; };
            map(k.inX, k.inY); map(k.x, k.y); map(k.outX, k.outY);
        }
    return path;
}

namespace {

/// The face's PostScript name (name ID 6) from its 'name' table: Windows Unicode records first, then Mac Roman.
QString postScriptName(const QRawFont& raw) {
    const QByteArray table = raw.fontTable("name");
    auto u16 = [&](int at) { return at + 2 <= table.size() ? int(uint8_t(table[at])) << 8 | uint8_t(table[at + 1]) : -1; };
    const int count = u16(2), strings = u16(4);
    if (count < 0 || strings < 0) return {};
    QString mac;
    for (int i = 0; i < count; i++) {
        const int at = 6 + i * 12;
        const int platform = u16(at), nameId = u16(at + 6), length = u16(at + 8), offset = u16(at + 10);
        if (nameId != 6 || length <= 0 || strings + offset + length > table.size()) continue;
        const char* data = table.constData() + strings + offset;
        if (platform == 3 || platform == 0) {
            QString name;
            for (int k = 0; k + 1 < length; k += 2) name.append(QChar(char16_t(uint8_t(data[k]) << 8 | uint8_t(data[k + 1]))));
            if (!name.isEmpty()) return name;
        } else if (platform == 1 && mac.isEmpty()) mac = QString::fromLatin1(data, length);
    }
    return mac;
}

} // namespace

std::optional<compositor::PsdTextMetrics> psdTextMetrics(const compositor::LayerText& text) {
    if (!text.runs.empty() || (text.boxWidth > 0 && text.boxHeight > 0)) {
        if (text.text.empty()) return std::nullopt;
        const RunLayout laid = layoutRuns(text);
        compositor::PsdTextMetrics m;
        m.fontSize = laid.fonts.front().pixelSize();
        m.ascent = laid.firstAscent;
        // The block's height over its lines (the writer spaces the bounds by one pitch; each run keeps its own leading).
        m.lines = int(laid.lines.size());
        m.lineHeight = laid.lines.size() > 1 ? (laid.lines.back().baseline - laid.lines.front().baseline) / double(laid.lines.size() - 1) : laid.pitch;
        if (!(m.lineHeight > 0)) m.lineHeight = laid.pitch;
        m.blockWidth = laid.width;
        m.blockLeft = m.blockTop = textPadding;
        const std::vector<compositor::TextRun> runs = compositor::textRuns(text);
        for (size_t i = 0; i < runs.size(); i++) {
            const QRawFont raw = QRawFont::fromFont(laid.fonts[i]);
            compositor::PsdTextMetrics::RunFace face;
            face.postScriptName = postScriptName(raw).toStdString();
            if (face.postScriptName.empty()) face.postScriptName = QString(laid.fonts[i].family()).remove(' ').toStdString();
            face.fauxBold = runs[i].bold && runs[i].weight == 0 && raw.weight() < QFont::DemiBold;
            face.fauxItalic = runs[i].italic && raw.style() == QFont::StyleNormal;
            m.runs.push_back(face);
        }
        m.postScriptName = m.runs.front().postScriptName;
        m.fauxBold = m.runs.front().fauxBold;
        m.fauxItalic = m.runs.front().fauxItalic;
        return m;
    }
    // Exactly renderTextLayer's layout.
    const QFont font = fontFor(text);
    const QFontMetricsF metrics(font);
    QString content = QString::fromStdString(text.text);
    if (content.isEmpty()) return std::nullopt;
    const QStringList lines = content.split('\n');
    compositor::PsdTextMetrics m;
    m.fontSize = font.pixelSize();
    m.ascent = metrics.ascent();
    m.lineHeight = metrics.height() * std::clamp(text.lineSpacing, 0.1, 10.0);
    for (const QString& line : lines) m.blockWidth = std::max(m.blockWidth, metrics.horizontalAdvance(line));
    m.blockLeft = m.blockTop = textPadding;
    m.lines = int(lines.size());
    const QRawFont raw = QRawFont::fromFont(font);
    m.postScriptName = postScriptName(raw).toStdString();
    if (m.postScriptName.empty()) m.postScriptName = QString(font.family()).remove(' ').toStdString();
    // Bold or italic asked for, but the face found is neither: Qt synthesised it, as Photoshop's faux styles do.
    m.fauxBold = text.bold && raw.weight() < QFont::DemiBold;
    m.fauxItalic = text.italic && raw.style() == QFont::StyleNormal;
    return m;
}

namespace {

QString squashed(QString s) { return s.remove(' ').remove('-').remove('_').toLower(); }

/// The installed family a PostScript name belongs to: "ArialMT" is Arial, "NotoSans-Bold" is Noto Sans.
QString familyForPostScriptName(const QString& postScriptName) {
    static QHash<QString, QString> cache;
    if (auto it = cache.find(postScriptName); it != cache.end()) return *it;
    QString base = postScriptName.section('-', 0, 0);
    for (const char* tail : {"PSMT", "MT", "PS"}) if (base.endsWith(tail) && base.size() > int(strlen(tail)) + 2) { base.chop(int(strlen(tail))); break; }
    const QString wanted = squashed(base);
    QString found;
    for (const QString& family : QFontDatabase::families()) if (squashed(family) == wanted) { found = family; break; }
    if (found.isEmpty()) {
        // Not installed: the name spelled out ("TimesNewRoman" is Times New Roman), for fontconfig to match.
        for (int i = 0; i < base.size(); i++) {
            if (i > 0 && base[i].isUpper() && base[i - 1].isLower()) found += ' ';
            found += base[i];
        }
    }
    // Faces sold with Adobe's apps have free twins with their metrics: ask for those when the face is missing, the
    // twin that is installed first (fontconfig knows "Helvetica" but not "Helvetica Neue LT Std").
    static const std::pair<const char*, QStringList> twins[] = {
        {"helveticaneue", {"Helvetica", "TeX Gyre Heros", "Nimbus Sans", "Liberation Sans", "Arimo"}},
        {"helvetica", {"TeX Gyre Heros", "Nimbus Sans", "Liberation Sans", "Arimo"}},
        {"myriad", {"Source Sans 3", "Source Sans Pro", "PT Sans", "Noto Sans"}},
        {"verdana", {"DejaVu Sans", "Bitstream Vera Sans"}},
        {"minion", {"Crimson Pro", "Crimson Text", "Liberation Serif"}},
    };
    for (const auto& [prefix, list] : twins)
        if (wanted.startsWith(QLatin1String(prefix)) && !QFontDatabase::families().contains(found, Qt::CaseInsensitive) && QFont::substitutes(found).isEmpty()) { QFont::insertSubstitutions(found, list); break; }
    cache.insert(postScriptName, found);
    return found;
}

bool installed(const QString& family) {
    for (const QString& f : QFontDatabase::families()) if (f.compare(family, Qt::CaseInsensitive) == 0) return true;
    return false;
}

} // namespace

void finishPsdText(compositor::PsdImport& imported) {
    for (const compositor::PsdImportedText& t : imported.texts) {
        compositor::Layer* layer = imported.document.find(t.layer);
        if (!layer || !layer->text) continue;
        compositor::LayerText& text = *layer->text;
        const QString family = familyForPostScriptName(QString::fromStdString(t.postScriptName));
        text.fontFamily = family.toStdString();
        std::vector<std::string> missing;
        auto check = [&](const std::string& postScript, const QString& fam) {
            if (!fam.isEmpty() && !installed(fam) && std::find(missing.begin(), missing.end(), postScript) == missing.end()) missing.push_back(postScript);
        };
        check(t.postScriptName, family);
        for (size_t i = 0; i < text.runs.size() && i < t.runPostScriptNames.size(); i++) {
            const QString runFamily = familyForPostScriptName(QString::fromStdString(t.runPostScriptNames[i]));
            text.runs[i].fontFamily = runFamily.toStdString();
            check(t.runPostScriptNames[i], runFamily);
        }
        for (const std::string& postScript : missing)
            imported.notes.push_back(QCoreApplication::translate("app::TextLayer", "Layer \"%1\": the font %2 is not installed; the text keeps Photoshop's pixels until you edit it, then uses the closest match, %3.")
                                         .arg(QString::fromStdString(layer->name), QString::fromStdString(postScript), QFontInfo(fontFor(text)).family()).toStdString());
        const QFontMetricsF metrics(fontFor(text));
        const double leading = t.leading > 0 ? t.leading : t.autoLeading * text.fontSize;
        if (metrics.height() > 0) text.lineSpacing = std::clamp(leading / metrics.height(), 0.1, 10.0);
    }
}

void finishPendingText(compositor::PsdImport& imported) {
    for (const compositor::PsdImport::PendingText& p : imported.pendingTexts) {
        compositor::Layer* layer = imported.document.find(p.layer);
        if (!layer || !layer->text) continue;
        compositor::LayerText& text = *layer->text;
        std::vector<std::string> missing;
        auto check = [&](const std::string& family) {
            if (!family.empty() && !installed(QString::fromStdString(family)) && std::find(missing.begin(), missing.end(), family) == missing.end()) missing.push_back(family);
        };
        check(text.fontFamily);
        for (const compositor::TextRun& run : text.runs) check(run.fontFamily);
        for (const std::string& family : missing)
            imported.notes.push_back(QCoreApplication::translate("app::TextLayer", "Layer \"%1\": the font %2 is not installed; the closest match, %3, draws it until you install it.")
                                         .arg(QString::fromStdString(layer->name), QString::fromStdString(family), QFontInfo(QFont(QString::fromStdString(family))).family()).toStdString());
        const compositor::AnyImage image = renderTextLayerAt(text, imported.document.sampleType);
        if (!image) continue;
        const auto m = psdTextMetrics(text);
        const double blockTop = m ? m->blockTop : textPadding, ascent = m ? m->ascent : 0;
        double left = p.left - textPadding, top = p.top - blockTop;
        if (!p.boxed && p.baseline > 0) top = p.baseline - ascent - blockTop;
        if (!p.boxed && p.align > 0 && p.width > 0) {
            const double slack = p.width - (image.width() - 2 * textPadding);
            left += p.align == 1 ? slack / 2 : slack;
        }
        layer->asset = compositor::Asset::makeAny(image, layer->name);
        layer->textImage = image;
        const double w = image.width(), h = image.height();
        if (p.rotation == 0) {
            layer->transform = compositor::LayerTransform(compositor::Point(std::round(left), std::round(top)), compositor::Size(w, h));
        } else {
            // The upright raster's corner and centre turned into the document; the layer turns about its centre.
            const double a = p.rotation * M_PI / 180, c = std::cos(a), sn = std::sin(a);
            const double cx = left + w / 2, cy = top + h / 2;
            const compositor::Point centre(p.originX + cx * c - cy * sn, p.originY + cx * sn + cy * c);
            layer->transform = compositor::LayerTransform(compositor::Point(centre.x - w / 2, centre.y - h / 2), compositor::Size(w, h));
            layer->transform.rotation = p.rotation;
        }
    }
    imported.pendingTexts.clear();
}

compositor::PsdImportOptions psdImportOptions() {
    compositor::PsdImportOptions options;
    options.decodeImage = [](const std::vector<uint8_t>& bytes, const std::string&, const std::string&) -> compositor::ImagePtr {
        QImage image;
        if (bytes.size() > size_t(std::numeric_limits<int>::max()) || !image.loadFromData(bytes.data(), int(bytes.size()))) return nullptr;
        if ((long long)image.width() * image.height() > compositor::Document::pixelBudget) return nullptr;
        return fromQImage(image);
    };
    return options;
}

compositor::PsdImportOptions affinityImportOptions() {
    compositor::PsdImportOptions options;
    options.decodeImage = [](const std::vector<uint8_t>& bytes, const std::string&, const std::string&) -> compositor::ImagePtr {
        if (bytes.size() > size_t(std::numeric_limits<int>::max())) return nullptr;
        QByteArray data = QByteArray::fromRawData(reinterpret_cast<const char*>(bytes.data()), int(bytes.size()));
        QBuffer buffer(&data);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer);
        reader.setAutoTransform(false);
        const QImage image = reader.read();
        if (image.isNull() || (long long)image.width() * image.height() > compositor::Document::pixelBudget) return nullptr;
        return fromQImage(image);
    };
    return options;
}

compositor::PsdExportOptions psdExportOptions() {
    compositor::PsdExportOptions options;
    options.textMetrics = psdTextMetrics;
    return options;
}

} // namespace app
