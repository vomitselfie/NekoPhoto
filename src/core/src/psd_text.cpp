// Photoshop type layers ('TySh') for NekoPhoto's text layers: point text, one style, one paragraph style,
// placed so that Photoshop's own layout lands on our pixels. The layout follows Patchy's writer
// (src/psd/psd_text_write.cpp there, MIT; see src/third_party/patchy_psd), with the rules it pinned against
// Photoshop: engine units are document pixels; fixed leading needs /AutoLeading false; tracking is an
// integer; numbers stay short; the block's length stays even without a pad after its end-anchored tail.
#include "compositor/psd.h"
#include "compositor/psd_writer.h"
#include "psd/psd_descriptor.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <variant>

namespace compositor {
namespace {

using patchy::psd::BigEndianWriter;

std::u16string utf16(const std::string& s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        uint32_t c = uint8_t(s[i]);
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (extra) c &= (0x3F >> extra);
        i++;
        for (int k = 0; k < extra && i < s.size(); k++, i++) c = (c << 6) | (uint8_t(s[i]) & 0x3F);
        if (c >= 0x10000) { c -= 0x10000; out.push_back(char16_t(0xD800 + (c >> 10))); out.push_back(char16_t(0xDC00 + (c & 0x3FF))); }
        else out.push_back(char16_t(c));
    }
    return out;
}

/// A PostScript-style string of UTF-16BE with a byte-order mark, as the engine data holds text.
std::string engineString(const std::string& utf8) {
    std::string out = "(";
    auto byte = [&](uint8_t b) { if (b == '(' || b == ')' || b == '\\') out.push_back('\\'); out.push_back(char(b)); };
    byte(0xFE); byte(0xFF);
    for (char16_t u : utf16(utf8)) { byte(uint8_t(u >> 8)); byte(uint8_t(u & 0xFF)); }
    out.push_back(')');
    return out;
}

/// At most six decimals, keeping ".0" (Photoshop's engine parser refuses long tokens).
std::string number(double v) {
    if (!std::isfinite(v)) v = 0;
    char buffer[48];
    std::snprintf(buffer, sizeof buffer, "%.6f", v);
    std::string t(buffer);
    while (t.size() > 1 && t.back() == '0' && t[t.size() - 2] != '.') t.pop_back();
    return t;
}

std::string paragraph(int justification, double leadingFraction) {
    return "<< /Justification " + std::to_string(justification) +
           " /FirstLineIndent 0.0 /StartIndent 0.0 /EndIndent 0.0 /SpaceBefore 0.0 /SpaceAfter 0.0"
           " /AutoHyphenate true /HyphenatedWordSize 6 /PreHyphen 2 /PostHyphen 2 /ConsecutiveHyphens 8"
           " /Zone 36.0 /WordSpacing [ 0.8 1.0 1.33 ] /LetterSpacing [ 0.0 0.0 0.0 ]"
           " /GlyphSpacing [ 1.0 1.0 1.0 ] /AutoLeading " + number(leadingFraction) +
           " /LeadingType 0 /Hanging false /Burasagari false /KinsokuOrder 0 /EveryLineComposer false >>";
}

std::string style(const TextRun& run, int font, bool fauxBold, bool fauxItalic, double lineHeight) {
    const double size = std::max(1.0, run.fontSize);
    std::string s = "<< /Font " + std::to_string(font) + " /FontSize " + number(size);
    s += std::string(" /FauxBold ") + (fauxBold ? "true" : "false");
    s += std::string(" /FauxItalic ") + (fauxItalic ? "true" : "false");
    s += " /AutoLeading false /Leading " + number(lineHeight);
    // Our spacing is extra pixels per glyph; Photoshop's tracking is thousandths of the size, an integer.
    const long tracking = std::lround(run.letterSpacing * 1000 / size);
    if (tracking) s += " /Tracking " + std::to_string(tracking);
    if (run.baselineShift != 0) s += " /BaselineShift " + number(run.baselineShift);
    if (run.caps != TextRun::Caps::Normal) s += std::string(" /FontCaps ") + (run.caps == TextRun::Caps::Small ? "1" : "2");
    if (run.underline) s += " /Underline true";
    if (run.strikethrough) s += " /Strikethrough true";
    s += " /AutoKerning true /Kerning 0 /FillColor << /Type 1 /Values [ 1.0 " + number(std::clamp(run.red, 0.0, 1.0)) + " " +
         number(std::clamp(run.green, 0.0, 1.0)) + " " + number(std::clamp(run.blue, 0.0, 1.0)) + " ] >> >>";
    return s;
}

std::vector<uint8_t> engineData(const LayerText& text, const std::string& engineText, int units, int justification, const PsdTextMetrics& m) {
    const std::string para = paragraph(justification, 1.2);
    // The runs, their faces (font 0 is Photoshop's invisible font), the last run taking the closing return.
    std::vector<TextRun> runs = textRuns(text);
    std::vector<std::string> fonts;
    std::vector<int> fontOf;
    std::string styles, lengths;
    int covered = 0;
    for (size_t i = 0; i < runs.size(); i++) {
        const PsdTextMetrics::RunFace face = i < m.runs.size() ? m.runs[i] : PsdTextMetrics::RunFace{m.postScriptName, m.fauxBold, m.fauxItalic};
        const std::string name = face.postScriptName.empty() ? "ArialMT" : face.postScriptName;
        auto at = std::find(fonts.begin(), fonts.end(), name);
        if (at == fonts.end()) { fonts.push_back(name); at = fonts.end() - 1; }
        const int font = int(at - fonts.begin()) + 1;
        fontOf.push_back(font);
        const int length = i + 1 == runs.size() ? units - covered : runs[i].length;
        covered += length;
        styles += " << /StyleSheet << /StyleSheetData " + style(runs[i], font, face.fauxBold, face.fauxItalic, runs[i].leading > 0 && !text.runs.empty() ? runs[i].leading : m.lineHeight) + " >> >>";
        lengths += " " + std::to_string(length);
    }
    std::string e = "<<\n/EngineDict <<\n/Editor << /Text " + engineString(engineText) + " >>\n";
    e += "/ParagraphRun << /DefaultRunData << /ParagraphSheet << /DefaultStyleSheet 0 /Properties << >> >> /Adjustments << /Axis [ 1.0 0.0 1.0 ] /XY [ 0.0 0.0 ] >> >>";
    e += " /RunArray [ << /ParagraphSheet << /DefaultStyleSheet 0 /Properties " + para + " >> /Adjustments << /Axis [ 1.0 0.0 1.0 ] /XY [ 0.0 0.0 ] >> >> ]";
    e += " /RunLengthArray [ " + std::to_string(units) + " ] /IsJoinable 1 >>\n";
    e += "/StyleRun << /DefaultRunData << /StyleSheet << /StyleSheetData << >> >> >> /RunArray [" + styles + " ] /RunLengthArray [" + lengths +
         " ] /IsJoinable 2 >>\n";
    e += "/GridInfo << /GridIsOn false /ShowGrid false /GridSize 18.0 /GridLeading 22.0 /GridColor << /Type 1 /Values [ 1.0 0.0 0.0 1.0 ] >>"
         " /GridLeadingFillColor << /Type 1 /Values [ 1.0 0.0 0.0 1.0 ] >> /AlignLineHeightToGridFlags false >>\n";
    e += "/AntiAlias 3 /UseFractionalGlyphWidths true\n";
    const bool boxed = text.boxWidth > 0 && text.boxHeight > 0;
    const std::string shape = boxed ? "1" : "0";
    const std::string frame = boxed ? " /BoxBounds [ 0.0 0.0 " + number(text.boxWidth) + " " + number(text.boxHeight) + " ]" : " /PointBase [ 0.0 0.0 ]";
    e += "/Rendered << /Version 1 /Shapes << /WritingDirection 0 /Children [ << /ShapeType " + shape + " /Procession 0 /Lines << /WritingDirection 0 /Children [ ] >>"
         " /Cookie << /Photoshop << /ShapeType " + shape + frame + " /Base << /ShapeType " + shape + " /TransformPoint0 [ 1.0 0.0 ]"
         " /TransformPoint1 [ 0.0 1.0 ] /TransformPoint2 [ 0.0 0.0 ] >> >> >> >> ] >> >>\n>>\n";
    const PsdTextMetrics::RunFace first = m.runs.empty() ? PsdTextMetrics::RunFace{m.postScriptName, m.fauxBold, m.fauxItalic} : m.runs.front();
    std::string fontSet = "<< /Name " + engineString("AdobeInvisFont") + " /Script 0 /FontType 0 /Synthetic 0 >>";
    for (const std::string& f : fonts) fontSet += " << /Name " + engineString(f) + " /Script 0 /FontType 1 /Synthetic 0 >>";
    std::string resources = "<< /KinsokuSet [ ] /MojiKumiSet [ ] /TheNormalStyleSheet 0 /TheNormalParagraphSheet 0 /ParagraphSheetSet [ << /Name " +
        engineString("Normal RGB") + " /DefaultStyleSheet 0 /Properties " + para + " >> ] /StyleSheetSet [ << /Name " + engineString("Normal RGB") +
        " /StyleSheetData " + style(runs.front(), fontOf.front(), first.fauxBold, first.fauxItalic, runs.front().leading > 0 && !text.runs.empty() ? runs.front().leading : m.lineHeight) + " >> ] /FontSet [ " + fontSet +
        " ] /SuperscriptSize 0.583 /SuperscriptPosition 0.333 /SubscriptSize 0.583 /SubscriptPosition 0.333 /SmallCapSize 0.7 >>";
    e += "/ResourceDict " + resources + "\n/DocumentResources " + resources + "\n>>";
    return std::vector<uint8_t>(e.begin(), e.end());
}

void header(BigEndianWriter& w, const char* key, const char* type) {
    patchy::psd::write_descriptor_id(w, key);
    for (int i = 0; i < 4; i++) w.write_u8(uint8_t(type[i]));
}

void enumItem(BigEndianWriter& w, const char* key, const char* type, const char* value) {
    header(w, key, "enum");
    patchy::psd::write_descriptor_id(w, type);
    patchy::psd::write_descriptor_id(w, value);
}

void boundsItem(BigEndianWriter& w, const char* key, double left, double top, double right, double bottom) {
    header(w, key, "Objc");
    patchy::psd::write_descriptor_unicode_string(w, "");
    patchy::psd::write_descriptor_id(w, "bounds");
    w.write_u32(4);
    const std::pair<const char*, double> sides[] = {{"Left", left}, {"Top ", top}, {"Rght", right}, {"Btom", bottom}};
    for (auto& [side, value] : sides) { header(w, side, "UntF"); for (char c : std::string("#Pnt")) w.write_u8(uint8_t(c)); patchy::psd::write_f64(w, value); }
}

} // namespace

std::optional<std::vector<uint8_t>> photoshopTypeBlock(const LayerText& text, const PsdTextMetrics& m, const LayerTransform& t,
                                                       int imageWidth, int imageHeight, const Rect& recordBounds) {
    if (t.flipX || t.flipY || imageWidth <= 0 || imageHeight <= 0 || !(m.fontSize > 0) || !(m.lineHeight > 0)) return std::nullopt;
    // Point text anchors at the first baseline, on the left edge, the centre or the right edge of the block.
    const int justification = text.alignment == 1 ? 2 : text.alignment == 2 ? 1 : 0;   // ours: left, centre, right
    // Box text anchors at its frame's top-left, the frame [0 0 w h] from there (a frame moved down moves Photoshop's
    // text by twice as much, Patchy found, so it never is).
    const bool boxed = text.boxWidth > 0 && text.boxHeight > 0;
    const double anchorX = boxed ? m.blockLeft : m.blockLeft + (text.alignment == 1 ? m.blockWidth / 2 : text.alignment == 2 ? m.blockWidth : 0);
    const double anchorY = boxed ? m.blockTop : m.blockTop + m.ascent;
    // Raster to document: scale to the placed size, then rotate clockwise about the centre.
    const double sx = t.size.width / imageWidth, sy = t.size.height / imageHeight;
    const double angle = t.rotation * M_PI / 180, c = std::cos(angle), s = std::sin(angle);
    const Point centre = t.center();
    const double px = t.origin.x + anchorX * sx - centre.x, py = t.origin.y + anchorY * sy - centre.y;
    const double tx = centre.x + px * c - py * s, ty = centre.y + px * s + py * c;
    const double matrix[6] = {sx * c, sx * s, -sy * s, sy * c, tx, ty};
    // The block around the anchor, in text units (raster pixels before the matrix).
    double left = m.blockLeft - anchorX, right = m.blockLeft + m.blockWidth - anchorX;
    double top = -m.ascent, bottom = m.lineHeight * std::max(1, m.lines) - m.ascent;
    if (boxed) { left = 0; top = 0; right = text.boxWidth; bottom = text.boxHeight; }

    std::string engineText = text.text;
    std::replace(engineText.begin(), engineText.end(), '\n', '\r');
    engineText.push_back('\r');   // Photoshop's text always ends in a return
    const int units = int(utf16(engineText).size());
    std::vector<uint8_t> engine = engineData(text, engineText, units, justification, m);

    auto build = [&](const std::vector<uint8_t>& engineBytes) {
        BigEndianWriter w;
        w.write_u16(1);
        for (double v : matrix) patchy::psd::write_f64(w, v);
        w.write_u16(50);
        w.write_u32(16);
        patchy::psd::write_descriptor_unicode_string(w, "");
        patchy::psd::write_descriptor_id(w, "TxLr");
        w.write_u32(8);
        header(w, "Txt ", "TEXT");
        patchy::psd::write_descriptor_unicode_string(w, engineText);
        enumItem(w, "textGridding", "textGridding", "None");
        enumItem(w, "Ornt", "Ornt", "Hrzn");
        enumItem(w, "AntA", "Annt", "AnCr");
        boundsItem(w, "bounds", left, top, right, bottom);
        boundsItem(w, "boundingBox", left, top, right, bottom);
        header(w, "EngineData", "tdta");
        w.write_u32(uint32_t(engineBytes.size()));
        w.write_bytes(engineBytes);
        header(w, "TextIndex", "long");
        w.write_u32(0);
        // No warp.
        w.write_u16(1);
        w.write_u32(16);
        patchy::psd::write_descriptor_unicode_string(w, "");
        patchy::psd::write_descriptor_id(w, "warp");
        w.write_u32(5);
        enumItem(w, "warpStyle", "warpStyle", "warpNone");
        for (const char* key : {"warpValue", "warpPerspective", "warpPerspectiveOther"}) { header(w, key, "doub"); patchy::psd::write_f64(w, 0); }
        enumItem(w, "warpRotate", "Ornt", "Hrzn");
        // The layer's bounds in the document, as four integers, at the very end.
        for (double v : {recordBounds.x, recordBounds.y, recordBounds.x + recordBounds.width, recordBounds.y + recordBounds.height})
            w.write_u32(uint32_t(int32_t(std::lround(v))));
        return w.bytes();
    };
    std::vector<uint8_t> block = build(engine);
    if (block.size() & 1) { engine.push_back('\n'); block = build(engine); }   // even, without a pad after the tail
    return block;
}

// ---- Reading ---------------------------------------------------------------------------------------

namespace {

struct EngineEntry;

/// A value of the engine data's PostScript-like syntax: dictionaries, arrays, names, strings, numbers.
struct EngineValue {
    enum Kind { Null, Number, Bool, Name, String, Array, Dict } kind = Null;
    double number = 0;
    bool boolean = false;
    std::string text;                                   // name, or the string's UTF-8
    std::vector<EngineValue> items;                     // array
    std::vector<EngineEntry> entries;                   // dictionary, in order

    const EngineValue* get(const std::string& key) const;
    const EngineValue* path(std::initializer_list<const char*> keys) const {
        const EngineValue* v = this;
        for (const char* k : keys) { if (!v || v->kind != Dict) return nullptr; v = v->get(k); }
        return v;
    }
    double num(const char* key, double fallback) const { auto v = get(key); return v && v->kind == Number ? v->number : fallback; }
    bool flag(const char* key, bool fallback) const { auto v = get(key); return v && v->kind == Bool ? v->boolean : fallback; }
};

struct EngineEntry {
    std::string key;
    EngineValue value;
};

const EngineValue* EngineValue::get(const std::string& key) const {
    for (auto& [k, v] : entries) if (k == key) return &v;
    return nullptr;
}

std::string utf8(const std::u16string& units) {
    std::string out;
    for (size_t i = 0; i < units.size(); i++) {
        uint32_t c = units[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < units.size()) { c = 0x10000 + ((c - 0xD800) << 10) + (units[i + 1] - 0xDC00); i++; }
        if (c < 0x80) out.push_back(char(c));
        else if (c < 0x800) { out.push_back(char(0xC0 | (c >> 6))); out.push_back(char(0x80 | (c & 0x3F))); }
        else if (c < 0x10000) { out.push_back(char(0xE0 | (c >> 12))); out.push_back(char(0x80 | ((c >> 6) & 0x3F))); out.push_back(char(0x80 | (c & 0x3F))); }
        else { out.push_back(char(0xF0 | (c >> 18))); out.push_back(char(0x80 | ((c >> 12) & 0x3F))); out.push_back(char(0x80 | ((c >> 6) & 0x3F))); out.push_back(char(0x80 | (c & 0x3F))); }
    }
    return out;
}

class EngineParser {
public:
    EngineParser(const uint8_t* data, size_t size) : p_(data), end_(data + size) {}
    bool parse(EngineValue& out) { return value(out, 0); }

private:
    const uint8_t* p_;
    const uint8_t* end_;

    void space() { while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\r' || *p_ == '\n' || *p_ == 0)) p_++; }
    bool delimiter(uint8_t c) const { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '/' || c == '[' || c == ']' || c == '<' || c == '>' || c == '(' || c == ')'; }
    std::string word() { const uint8_t* start = p_; while (p_ < end_ && !delimiter(*p_)) p_++; return std::string(start, p_); }

    bool value(EngineValue& v, int depth) {
        if (depth > 64) return false;
        space();
        if (p_ >= end_) return false;
        if (end_ - p_ >= 2 && p_[0] == '<' && p_[1] == '<') {
            p_ += 2;
            v.kind = EngineValue::Dict;
            for (;;) {
                space();
                if (p_ >= end_) return false;
                if (end_ - p_ >= 2 && p_[0] == '>' && p_[1] == '>') { p_ += 2; return true; }
                if (*p_ != '/') return false;
                p_++;
                std::string key = word();
                EngineValue item;
                if (!value(item, depth + 1)) return false;
                v.entries.push_back({std::move(key), std::move(item)});
            }
        }
        if (*p_ == '[') {
            p_++;
            v.kind = EngineValue::Array;
            for (;;) {
                space();
                if (p_ >= end_) return false;
                if (*p_ == ']') { p_++; return true; }
                EngineValue item;
                if (!value(item, depth + 1)) return false;
                v.items.push_back(std::move(item));
            }
        }
        if (*p_ == '/') { p_++; v.kind = EngineValue::Name; v.text = word(); return true; }
        if (*p_ == '(') {
            p_++;
            std::string bytes;
            while (p_ < end_ && *p_ != ')') {
                if (*p_ == '\\' && p_ + 1 < end_) p_++;
                bytes.push_back(char(*p_++));
            }
            if (p_ >= end_) return false;
            p_++;
            v.kind = EngineValue::String;
            if (bytes.size() >= 2 && uint8_t(bytes[0]) == 0xFE && uint8_t(bytes[1]) == 0xFF) {
                std::u16string units;
                for (size_t i = 2; i + 1 < bytes.size(); i += 2) units.push_back(char16_t(uint8_t(bytes[i]) << 8 | uint8_t(bytes[i + 1])));
                v.text = utf8(units);
            } else v.text = bytes;
            return true;
        }
        std::string token = word();
        if (token.empty()) return false;
        if (token == "true" || token == "false") { v.kind = EngineValue::Bool; v.boolean = token == "true"; return true; }
        char* stop = nullptr;
        v.number = std::strtod(token.c_str(), &stop);
        if (!stop || *stop) return false;
        v.kind = EngineValue::Number;
        return true;
    }
};

/// A style run's properties over the normal style sheet's (runs leave out what equals it).
EngineValue effective(const EngineValue* normal, const EngineValue* run) {
    EngineValue merged;
    merged.kind = EngineValue::Dict;
    if (normal && normal->kind == EngineValue::Dict) merged.entries = normal->entries;
    if (run && run->kind == EngineValue::Dict)
        for (auto& [k, v] : run->entries) {
            bool replaced = false;
            for (auto& [mk, mv] : merged.entries) if (mk == k) { mv = v; replaced = true; }
            if (!replaced) merged.entries.push_back({k, v});
        }
    return merged;
}

} // namespace

std::optional<PsdTypeLayer> readPhotoshopType(const uint8_t* data, size_t size, std::string* why) {
    auto no = [&](const char* reason) -> std::optional<PsdTypeLayer> { if (why) *why = reason; return std::nullopt; };
    try {
        patchy::psd::BigEndianReader r(std::span<const uint8_t>(data, size));
        if (r.read_u16() != 1) return no("an unknown type layer version");
        double m[6];
        for (double& v : m) v = patchy::psd::read_f64(r);
        if (r.read_u16() != 50 || r.read_u32() != 16) return no("an unknown text descriptor version");
        const auto descriptor = patchy::psd::read_descriptor(r);
        if (r.read_u16() != 1 || r.read_u32() != 16) return no("an unknown warp version");
        const auto warp = patchy::psd::read_descriptor(r);
        if (auto style = patchy::psd::descriptor_value(warp, "warpStyle"); style && style->enum_value != "warpNone") return no("warped");
        if (auto orientation = patchy::psd::descriptor_value(descriptor, "Ornt"); orientation && orientation->enum_value == "Vrtc") return no("vertical");
        // A rotation with an even scale: the text upright at that scale, the layer turned (clockwise, y down). A scale
        // a hair uneven (a transform nudged by hand: under 1.5%) reads as the vertical one, which sets the size and
        // the line pitch; the width then differs by less than that once the text is edited.
        const double scaleX = std::hypot(m[0], m[1]), scaleY = std::hypot(m[2], m[3]);
        if (!(scaleX > 0) || !(scaleY > 0) || std::abs(scaleX / scaleY - 1) > 0.015) return no("skewed or stretched");
        // Orthogonal and not mirrored: the second axis is the first turned a quarter.
        if (std::abs(m[0] * m[2] + m[1] * m[3]) > 1e-6 * scaleX * scaleY || m[0] * m[3] - m[1] * m[2] <= 0) return no("skewed or mirrored");
        const double scale = scaleY;
        const double rotation = std::atan2(m[1], m[0]) * 180 / M_PI;
        auto engineItem = patchy::psd::descriptor_value(descriptor, "EngineData");
        if (!engineItem || engineItem->raw_value.empty()) return no("without engine data");
        EngineValue engine;
        if (!EngineParser(engineItem->raw_value.data(), engineItem->raw_value.size()).parse(engine)) return no("engine data that could not be read");
        const EngineValue* dict = engine.get("EngineDict");
        const EngineValue* resources = engine.get("ResourceDict");
        if (!dict || !resources) return no("engine data that could not be read");
        // Box (paragraph) text: its frame, in text units from the transform's origin.
        std::optional<std::array<double, 4>> box;
        if (auto children = dict->path({"Rendered", "Shapes", "Children"}); children && children->kind == EngineValue::Array) {
            if (children->items.size() > 1) return no("in several frames");
            for (auto& shape : children->items) {
                if (shape.num("ShapeType", 0) == 0) continue;
                if (shape.num("ShapeType", 0) != 1) return no("on a path");
                const EngineValue* bounds = shape.path({"Cookie", "Photoshop", "BoxBounds"});
                if (!bounds || bounds->kind != EngineValue::Array || bounds->items.size() != 4) return no("box text without its frame");
                box = std::array<double, 4>{bounds->items[0].number, bounds->items[1].number, bounds->items[2].number, bounds->items[3].number};
                if (!((*box)[2] > (*box)[0]) || !((*box)[3] > (*box)[1])) return no("box text without its frame");
            }
        }
        const EngineValue* text = dict->path({"Editor", "Text"});
        if (!text || text->kind != EngineValue::String) return no("empty");

        // The normal style sheet, which runs only differ from.
        const EngineValue* normal = nullptr;
        if (auto sheets = resources->get("StyleSheetSet"); sheets && sheets->kind == EngineValue::Array && !sheets->items.empty()) {
            const size_t index = size_t(std::max(0.0, resources->num("TheNormalStyleSheet", 0)));
            normal = sheets->items[std::min(index, sheets->items.size() - 1)].get("StyleSheetData");
        }
        const EngineValue* runs = dict->path({"StyleRun", "RunArray"});
        const EngineValue* lengths = dict->path({"StyleRun", "RunLengthArray"});
        if (!runs || runs->kind != EngineValue::Array || runs->items.empty()) return no("without styles");
        if (!lengths || lengths->kind != EngineValue::Array || lengths->items.size() != runs->items.size()) return no("without styles");
        int justification = 0;
        if (auto paragraphs = dict->path({"ParagraphRun", "RunArray"}); paragraphs && paragraphs->kind == EngineValue::Array) {
            std::optional<int> seen;
            for (auto& p : paragraphs->items) {
                const EngineValue* props = p.path({"ParagraphSheet", "Properties"});
                const int j = props ? int(props->num("Justification", 0)) : 0;
                if (seen && *seen != j) return no("with paragraphs aligned differently");
                seen = j;
            }
            justification = seen.value_or(0);
        }
        if (justification > 2) return no("justified");

        PsdTypeLayer out;
        out.anchorX = m[4]; out.anchorY = m[5];
        out.rotation = std::abs(rotation) < 1e-6 ? 0 : rotation;
        if (box) {
            // Anchored at the frame's top-left.
            out.anchorX = m[4] + (*box)[0] * m[0] + (*box)[1] * m[2];
            out.anchorY = m[5] + (*box)[0] * m[1] + (*box)[1] * m[3];
            out.text.boxWidth = ((*box)[2] - (*box)[0]) * scaleX;
            out.text.boxHeight = ((*box)[3] - (*box)[1]) * scale;
        }
        std::string content = text->text;
        std::replace(content.begin(), content.end(), '\r', '\n');
        while (!content.empty() && content.back() == '\n') content.pop_back();
        out.text.text = content;
        const int contentLength = utf16Length(content);
        const EngineValue* fonts = resources->get("FontSet");
        double autoFraction = 1.2;
        if (auto paragraphs = dict->path({"ParagraphRun", "RunArray"}); paragraphs && paragraphs->kind == EngineValue::Array && !paragraphs->items.empty())
            if (const EngineValue* props = paragraphs->items[0].path({"ParagraphSheet", "Properties"})) autoFraction = props->num("AutoLeading", 1.2);
        double leading = 0;
        bool autoLeading = true;
        int covered = 0;
        for (size_t i = 0; i < runs->items.size(); i++) {
            const EngineValue style = effective(normal, runs->items[i].path({"StyleSheet", "StyleSheetData"}));
            if (std::abs(style.num("HorizontalScale", 1) - 1) > 1e-6 || std::abs(style.num("VerticalScale", 1) - 1) > 1e-6) return no("scaled horizontally or vertically");
            if (style.num("FontBaseline", 0) != 0) return no("superscript or subscript");
            TextRun run;
            run.length = int(lengths->items[i].number);
            const double fontSize = style.num("FontSize", 12) * scale;
            run.fontSize = fontSize;
            if (auto fill = style.get("FillColor")) {
                const EngineValue* values = fill->get("Values");
                if (fill->num("Type", 1) != 1 || !values || values->items.size() != 4) return no("filled with a non-RGB colour");
                run.red = values->items[1].number; run.green = values->items[2].number; run.blue = values->items[3].number;
            }
            run.letterSpacing = style.num("Tracking", 0) / 1000 * fontSize;
            run.baselineShift = style.num("BaselineShift", 0) * scale;
            const int caps = int(style.num("FontCaps", 0));
            run.caps = caps == 1 ? TextRun::Caps::Small : caps == 2 ? TextRun::Caps::All : TextRun::Caps::Normal;
            run.underline = style.flag("Underline", false);
            run.strikethrough = style.flag("Strikethrough", false);
            std::string postScript;
            const int fontIndex = int(style.num("Font", 0));
            if (fonts && fonts->kind == EngineValue::Array && fontIndex >= 0 && size_t(fontIndex) < fonts->items.size())
                if (auto name = fonts->items[size_t(fontIndex)].get("Name")) postScript = name->text;
            // Bold and italic from the face's name until the app finds the face; faux styles add to them.
            const std::string suffix = postScript.find('-') == std::string::npos ? "" : postScript.substr(postScript.find('-') + 1);
            auto has = [&](const char* w) { return suffix.find(w) != std::string::npos; };
            // The face's name after its family: words ("Medium", "BoldItalic") or Linotype's abbreviations ("Md",
            // "BdCn", "Th"), split where a capital starts a word.
            std::vector<std::string> words;
            for (char c : suffix) {
                if (std::isupper(uint8_t(c)) || words.empty()) words.emplace_back();
                words.back().push_back(c);
            }
            auto word = [&](std::initializer_list<const char*> any) {
                for (const std::string& w : words) for (const char* a : any) if (w == a) return true;
                return false;
            };
            run.bold = style.flag("FauxBold", false) || has("Bold") || has("Black") || has("Heavy") || word({"Bd", "Hv", "Blk"});
            run.italic = run.italic || word({"It", "Obl"});
            // The face's weight, when its name says one other than regular or bold.
            if (has("Hairline") || has("Thin") || word({"Th", "UltTh"})) run.weight = 100;
            else if (has("UltraLight") || has("ExtraLight") || word({"UltLt", "XLt"})) run.weight = 200;
            else if (has("Light") || word({"Lt"})) run.weight = 300;
            else if (has("Medium") || word({"Md"})) run.weight = 500;
            else if (has("SemiBold") || has("Semibold") || has("DemiBold") || has("Demi")) run.weight = 600;
            else if (has("ExtraBold") || has("UltraBold") || has("Heavy") || word({"Hv", "XBd"})) run.weight = 800;
            else if (has("Black") || word({"Blk"})) run.weight = 900;
            if (run.weight > 0 && style.flag("FauxBold", false)) run.weight = std::max(run.weight, 700);
            run.italic = style.flag("FauxItalic", false) || has("Italic") || has("Oblique");
            // Photoshop's line takes the largest leading on it.
            if (!style.flag("AutoLeading", true)) { autoLeading = false; leading = std::max(leading, style.num("Leading", 0) * scale); run.leading = style.num("Leading", 0) * scale; }
            else run.leading = autoFraction * fontSize;
            // Only what covers the text (Photoshop's closing return is not ours).
            run.length = std::clamp(run.length, 0, contentLength - covered);
            covered += run.length;
            if (run.length > 0 || out.text.runs.empty()) { out.text.runs.push_back(run); out.runPostScriptNames.push_back(postScript); }
        }
        if (!out.text.runs.empty()) out.postScriptName = out.runPostScriptNames.front();
        // Runs of the same style (and face) are one.
        for (size_t i = 1; i < out.text.runs.size();) {
            TextRun x = out.text.runs[i - 1], y = out.text.runs[i];
            x.length = y.length = 0;
            if (x == y && out.runPostScriptNames[i - 1] == out.runPostScriptNames[i]) {
                out.text.runs[i - 1].length += out.text.runs[i].length;
                out.text.runs.erase(out.text.runs.begin() + long(i));
                out.runPostScriptNames.erase(out.runPostScriptNames.begin() + long(i));
            } else i++;
        }
        settleTextRuns(out.text);
        if (out.text.runs.empty()) out.runPostScriptNames.resize(std::min<size_t>(out.runPostScriptNames.size(), 1));
        out.text.alignment = justification == 2 ? 1 : justification == 1 ? 2 : 0;
        if (!autoLeading) out.leading = leading;
        if (auto paragraphs = dict->path({"ParagraphRun", "RunArray"}); paragraphs && !paragraphs->items.empty())
            if (const EngineValue* props = paragraphs->items[0].path({"ParagraphSheet", "Properties"})) out.autoLeading = props->num("AutoLeading", 1.2);
        return out;
    } catch (std::exception&) {
        return no("a type layer that could not be read");
    }
}

} // namespace compositor
