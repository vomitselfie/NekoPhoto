// Photoshop type layers ('TySh') for NekoPhoto's text layers: point text, one style, one paragraph style,
// placed so that Photoshop's own layout lands on our pixels. The layout follows Patchy's writer
// (src/psd/psd_text_write.cpp there, MIT; see src/third_party/patchy_psd), with the rules it pinned against
// Photoshop: engine units are document pixels; fixed leading needs /AutoLeading false; tracking is an
// integer; numbers stay short; the block's length stays even without a pad after its end-anchored tail.
#include "compositor/psd.h"
#include "compositor/psd_writer.h"
#include "psd/psd_descriptor.hpp"
#include <algorithm>
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

std::string style(const LayerText& text, const PsdTextMetrics& m) {
    const double size = std::max(1.0, m.fontSize);
    std::string s = "<< /Font 1 /FontSize " + number(size);
    s += std::string(" /FauxBold ") + (m.fauxBold ? "true" : "false");
    s += std::string(" /FauxItalic ") + (m.fauxItalic ? "true" : "false");
    s += " /AutoLeading false /Leading " + number(m.lineHeight);
    // Our spacing is extra pixels per glyph; Photoshop's tracking is thousandths of the size, an integer.
    const long tracking = std::lround(text.letterSpacing * 1000 / size);
    if (tracking) s += " /Tracking " + std::to_string(tracking);
    s += " /AutoKerning true /Kerning 0 /FillColor << /Type 1 /Values [ 1.0 " + number(std::clamp(text.red, 0.0, 1.0)) + " " +
         number(std::clamp(text.green, 0.0, 1.0)) + " " + number(std::clamp(text.blue, 0.0, 1.0)) + " ] >> >>";
    return s;
}

std::vector<uint8_t> engineData(const LayerText& text, const std::string& engineText, int units, int justification, const PsdTextMetrics& m) {
    const double leadingFraction = m.lineHeight / std::max(1.0, m.fontSize);
    const std::string para = paragraph(justification, 1.2);
    std::string e = "<<\n/EngineDict <<\n/Editor << /Text " + engineString(engineText) + " >>\n";
    e += "/ParagraphRun << /DefaultRunData << /ParagraphSheet << /DefaultStyleSheet 0 /Properties << >> >> /Adjustments << /Axis [ 1.0 0.0 1.0 ] /XY [ 0.0 0.0 ] >> >>";
    e += " /RunArray [ << /ParagraphSheet << /DefaultStyleSheet 0 /Properties " + para + " >> /Adjustments << /Axis [ 1.0 0.0 1.0 ] /XY [ 0.0 0.0 ] >> >> ]";
    e += " /RunLengthArray [ " + std::to_string(units) + " ] /IsJoinable 1 >>\n";
    e += "/StyleRun << /DefaultRunData << /StyleSheet << /StyleSheetData << >> >> >> /RunArray [ << /StyleSheet << /StyleSheetData " + style(text, m) +
         " >> >> ] /RunLengthArray [ " + std::to_string(units) + " ] /IsJoinable 2 >>\n";
    e += "/GridInfo << /GridIsOn false /ShowGrid false /GridSize 18.0 /GridLeading 22.0 /GridColor << /Type 1 /Values [ 1.0 0.0 0.0 1.0 ] >>"
         " /GridLeadingFillColor << /Type 1 /Values [ 1.0 0.0 0.0 1.0 ] >> /AlignLineHeightToGridFlags false >>\n";
    e += "/AntiAlias 3 /UseFractionalGlyphWidths true\n";
    e += "/Rendered << /Version 1 /Shapes << /WritingDirection 0 /Children [ << /ShapeType 0 /Procession 0 /Lines << /WritingDirection 0 /Children [ ] >>"
         " /Cookie << /Photoshop << /ShapeType 0 /PointBase [ 0.0 0.0 ] /Base << /ShapeType 0 /TransformPoint0 [ 1.0 0.0 ]"
         " /TransformPoint1 [ 0.0 1.0 ] /TransformPoint2 [ 0.0 0.0 ] >> >> >> >> ] >> >>\n>>\n";
    (void)leadingFraction;
    std::string resources = "<< /KinsokuSet [ ] /MojiKumiSet [ ] /TheNormalStyleSheet 0 /TheNormalParagraphSheet 0 /ParagraphSheetSet [ << /Name " +
        engineString("Normal RGB") + " /DefaultStyleSheet 0 /Properties " + para + " >> ] /StyleSheetSet [ << /Name " + engineString("Normal RGB") +
        " /StyleSheetData " + style(text, m) + " >> ] /FontSet [ << /Name " + engineString("AdobeInvisFont") +
        " /Script 0 /FontType 0 /Synthetic 0 >> << /Name " + engineString(m.postScriptName.empty() ? "ArialMT" : m.postScriptName) +
        " /Script 0 /FontType 1 /Synthetic 0 >> ] /SuperscriptSize 0.583 /SuperscriptPosition 0.333 /SubscriptSize 0.583"
        " /SubscriptPosition 0.333 /SmallCapSize 0.7 >>";
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
    const double anchorX = m.blockLeft + (text.alignment == 1 ? m.blockWidth / 2 : text.alignment == 2 ? m.blockWidth : 0);
    const double anchorY = m.blockTop + m.ascent;
    // Raster to document: scale to the placed size, then rotate clockwise about the centre.
    const double sx = t.size.width / imageWidth, sy = t.size.height / imageHeight;
    const double angle = t.rotation * M_PI / 180, c = std::cos(angle), s = std::sin(angle);
    const Point centre = t.center();
    const double px = t.origin.x + anchorX * sx - centre.x, py = t.origin.y + anchorY * sy - centre.y;
    const double tx = centre.x + px * c - py * s, ty = centre.y + px * s + py * c;
    const double matrix[6] = {sx * c, sx * s, -sy * s, sy * c, tx, ty};
    // The block around the anchor, in text units (raster pixels before the matrix).
    const double left = m.blockLeft - anchorX, right = m.blockLeft + m.blockWidth - anchorX;
    const double top = -m.ascent, bottom = m.lineHeight * std::max(1, m.lines) - m.ascent;

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

bool sameValue(const EngineValue& a, const EngineValue& b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
    case EngineValue::Number: return std::abs(a.number - b.number) < 1e-6;
    case EngineValue::Bool: return a.boolean == b.boolean;
    case EngineValue::Name: case EngineValue::String: return a.text == b.text;
    case EngineValue::Array:
        if (a.items.size() != b.items.size()) return false;
        for (size_t i = 0; i < a.items.size(); i++) if (!sameValue(a.items[i], b.items[i])) return false;
        return true;
    case EngineValue::Dict:
        if (a.entries.size() != b.entries.size()) return false;
        for (auto& [k, v] : a.entries) { auto o = b.get(k); if (!o || !sameValue(v, *o)) return false; }
        return true;
    default: return true;
    }
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
        if (std::abs(m[1]) > 1e-9 || std::abs(m[2]) > 1e-9 || !(m[0] > 0) || std::abs(m[0] - m[3]) > 1e-6) return no("rotated, skewed or stretched");
        const double scale = m[0];
        auto engineItem = patchy::psd::descriptor_value(descriptor, "EngineData");
        if (!engineItem || engineItem->raw_value.empty()) return no("without engine data");
        EngineValue engine;
        if (!EngineParser(engineItem->raw_value.data(), engineItem->raw_value.size()).parse(engine)) return no("engine data that could not be read");
        const EngineValue* dict = engine.get("EngineDict");
        const EngineValue* resources = engine.get("ResourceDict");
        if (!dict || !resources) return no("engine data that could not be read");
        // Box text wraps in Photoshop's own way; only point text keeps its lines here.
        if (auto children = dict->path({"Rendered", "Shapes", "Children"}); children && children->kind == EngineValue::Array)
            for (auto& shape : children->items) if (shape.num("ShapeType", 0) != 0) return no("box (paragraph) text");
        const EngineValue* text = dict->path({"Editor", "Text"});
        if (!text || text->kind != EngineValue::String) return no("empty");

        // The normal style sheet, which runs only differ from.
        const EngineValue* normal = nullptr;
        if (auto sheets = resources->get("StyleSheetSet"); sheets && sheets->kind == EngineValue::Array && !sheets->items.empty()) {
            const size_t index = size_t(std::max(0.0, resources->num("TheNormalStyleSheet", 0)));
            normal = sheets->items[std::min(index, sheets->items.size() - 1)].get("StyleSheetData");
        }
        const EngineValue* runs = dict->path({"StyleRun", "RunArray"});
        if (!runs || runs->kind != EngineValue::Array || runs->items.empty()) return no("without styles");
        std::optional<EngineValue> style;
        for (auto& run : runs->items) {
            EngineValue e = effective(normal, run.path({"StyleSheet", "StyleSheetData"}));
            if (style && !sameValue(*style, e)) return no("in more than one style");
            if (!style) style = e;
        }
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
        if (std::abs(style->num("HorizontalScale", 1) - 1) > 1e-6 || std::abs(style->num("VerticalScale", 1) - 1) > 1e-6) return no("scaled horizontally or vertically");
        if (std::abs(style->num("BaselineShift", 0)) > 1e-6 || style->num("FontCaps", 0) != 0 || style->num("FontBaseline", 0) != 0
            || style->flag("Underline", false) || style->flag("Strikethrough", false)) return no("with baseline shift, caps, underline or strikethrough");

        PsdTypeLayer out;
        out.anchorX = m[4]; out.anchorY = m[5];
        std::string content = text->text;
        std::replace(content.begin(), content.end(), '\r', '\n');
        while (!content.empty() && content.back() == '\n') content.pop_back();
        out.text.text = content;
        const double fontSize = style->num("FontSize", 12) * scale;
        out.text.fontSize = fontSize;
        if (auto fill = style->get("FillColor")) {
            const EngineValue* values = fill->get("Values");
            if (fill->num("Type", 1) != 1 || !values || values->items.size() != 4) return no("filled with a non-RGB colour");
            out.text.red = values->items[1].number; out.text.green = values->items[2].number; out.text.blue = values->items[3].number;
        }
        out.text.alignment = justification == 2 ? 1 : justification == 1 ? 2 : 0;
        out.text.letterSpacing = style->num("Tracking", 0) / 1000 * fontSize;
        const int fontIndex = int(style->num("Font", 0));
        if (auto fonts = resources->get("FontSet"); fonts && fonts->kind == EngineValue::Array && fontIndex >= 0 && size_t(fontIndex) < fonts->items.size())
            if (auto name = fonts->items[size_t(fontIndex)].get("Name")) out.postScriptName = name->text;
        // Bold and italic from the face's name until the app finds the face; faux styles add to them.
        const std::string& ps = out.postScriptName;
        const std::string suffix = ps.find('-') == std::string::npos ? "" : ps.substr(ps.find('-') + 1);
        auto has = [&](const char* w) { return suffix.find(w) != std::string::npos; };
        out.text.bold = style->flag("FauxBold", false) || has("Bold") || has("Black") || has("Heavy");
        out.text.italic = style->flag("FauxItalic", false) || has("Italic") || has("Oblique");
        if (!style->flag("AutoLeading", true)) out.leading = style->num("Leading", 0) * scale;
        if (auto paragraphs = dict->path({"ParagraphRun", "RunArray"}); paragraphs && !paragraphs->items.empty())
            if (const EngineValue* props = paragraphs->items[0].path({"ParagraphSheet", "Properties"})) out.autoLeading = props->num("AutoLeading", 1.2);
        return out;
    } catch (std::exception&) {
        return no("a type layer that could not be read");
    }
}

} // namespace compositor
