// A PSD's alpha and spot channels (psd_channels.h).
#include "psd_channels.h"
#include "compositor/depth.h"
#include "compositor/psd_carry.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <string>

namespace compositor {

namespace {

struct Bytes {
    const std::vector<uint8_t>& b;
    size_t at = 0;
    bool ok = true;
    bool need(size_t n) { if (!ok || n > b.size() - at) { ok = false; return false; } return true; }
    uint8_t u8() { return need(1) ? b[at++] : 0; }
    uint16_t u16() { if (!need(2)) return 0; const uint16_t v = uint16_t(b[at] << 8 | b[at + 1]); at += 2; return v; }
    uint32_t u32() { const uint32_t hi = u16(); return hi << 16 | u16(); }
    bool done() const { return !ok || at >= b.size(); }
};

void appendUtf8(std::string& out, uint32_t c) {
    if (c < 0x80) out += char(c);
    else if (c < 0x800) { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { out += char(0xE0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
    else { out += char(0xF0 | (c >> 18)); out += char(0x80 | ((c >> 12) & 0x3F)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
}

/// UTF-8 to code points (a malformed byte stands for itself).
std::vector<uint32_t> codePoints(const std::string& s) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        const uint8_t c = uint8_t(s[i]);
        const int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (n == 1 || i + size_t(n) > s.size()) { out.push_back(c); i++; continue; }
        uint32_t v = c & (0x7F >> n);
        for (int k = 1; k < n; k++) v = v << 6 | (uint8_t(s[i + size_t(k)]) & 0x3F);
        out.push_back(v);
        i += size_t(n);
    }
    return out;
}

std::vector<std::string> unicodeNames(const std::vector<uint8_t>& data) {
    std::vector<std::string> names;
    Bytes r{data};
    while (!r.done() && names.size() < 64) {
        const uint32_t count = r.u32();
        if (!r.ok || count > (data.size() - r.at) / 2) break;
        std::string name;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t c = r.u16();
            if (c >= 0xD800 && c < 0xDC00 && i + 1 < count) { const uint32_t low = r.u16(); i++; c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00); }
            if (c) appendUtf8(name, c);
        }
        names.push_back(name);
    }
    return names;
}

std::vector<std::string> pascalNames(const std::vector<uint8_t>& data) {
    std::vector<std::string> names;
    Bytes r{data};
    while (!r.done() && names.size() < 64) {
        const uint8_t n = r.u8();
        if (!r.need(n)) break;
        std::string name;
        for (uint8_t i = 0; i < n; i++) appendUtf8(name, r.b[r.at + i]);   // Mac Roman's ASCII half; the rest is rare here
        r.at += n;
        names.push_back(name);
    }
    return names;
}

/// A DisplayInfo record: its colour as sRGB, opacity 0..1, and kind (0 selected areas, 1 masked areas, 2 spot).
struct Display {
    std::array<double, 3> color{1, 0, 0};
    double opacity = 0.5;
    int kind = 1;
};

std::array<double, 3> hsbToRgb(double h, double s, double v) {
    h = std::fmod(h, 360.0) / 60.0;
    const int i = int(std::floor(h));
    const double f = h - i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    switch (i % 6) {
    case 0: return {v, t, p};
    case 1: return {q, v, p};
    case 2: return {p, v, t};
    case 3: return {p, q, v};
    case 4: return {t, p, v};
    default: return {v, p, q};
    }
}

std::array<double, 3> labToRgb(double L, double a, double b) {
    // CIELAB (D50) to XYZ, adapted to D65 by Bradford, to sRGB.
    const double fy = (L + 16) / 116, fx = fy + a / 500, fz = fy - b / 200;
    auto finv = [](double t) { return t > 6.0 / 29 ? t * t * t : 3 * (6.0 / 29) * (6.0 / 29) * (t - 4.0 / 29); };
    const double X = 0.9642 * finv(fx), Y = finv(fy), Z = 0.8249 * finv(fz);
    const double Xd = 0.9555766 * X - 0.0230393 * Y + 0.0631636 * Z, Yd = -0.0282895 * X + 1.0099416 * Y + 0.0210077 * Z,
                 Zd = 0.0122982 * X - 0.0204830 * Y + 1.3299098 * Z;
    const double lin[3] = {3.2404542 * Xd - 1.5371385 * Yd - 0.4985314 * Zd, -0.9692660 * Xd + 1.8760108 * Yd + 0.0415560 * Zd,
                           0.0556434 * Xd - 0.2040259 * Yd + 1.0572252 * Zd};
    std::array<double, 3> out{};
    for (int c = 0; c < 3; c++) {
        const double v = std::clamp(lin[c], 0.0, 1.0);
        out[size_t(c)] = v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1 / 2.4) - 0.055;
    }
    return out;
}

/// Reads one record: colour space, four colour words, opacity, kind (1077's 13 bytes; 1007's 14, with padding).
Display readDisplay(Bytes& r) {
    Display d;
    const uint16_t space = r.u16();
    uint16_t c[4];
    for (auto& v : c) v = r.u16();
    d.opacity = std::clamp(r.u16() / 100.0, 0.0, 1.0);
    d.kind = r.u8();
    switch (space) {
    case 0: d.color = {c[0] / 65535.0, c[1] / 65535.0, c[2] / 65535.0}; break;
    case 1: d.color = hsbToRgb(c[0] / 65535.0 * 360, c[1] / 65535.0, c[2] / 65535.0); break;
    case 2: {   // CMYK: 0 is full ink
        const double k = c[3] / 65535.0;
        d.color = {c[0] / 65535.0 * k, c[1] / 65535.0 * k, c[2] / 65535.0 * k};
        break;
    }
    case 7: d.color = labToRgb(c[0] / 100.0, int16_t(c[1]) / 100.0, int16_t(c[2]) / 100.0); break;
    case 8: { const double v = 1 - std::clamp(c[0] / 10000.0, 0.0, 1.0); d.color = {v, v, v}; break; }
    default: d.color = {0.5, 0.5, 0.5}; break;   // a custom (colour book) ink: its RGB is not stored
    }
    return d;
}

void put16(std::vector<uint8_t>& o, uint32_t v) { o.push_back(uint8_t(v >> 8)); o.push_back(uint8_t(v)); }
void put32(std::vector<uint8_t>& o, uint32_t v) { put16(o, v >> 16); put16(o, v & 0xFFFF); }

/// A channel's DisplayInfo record as the model says it (RGB).
std::vector<uint8_t> displayRecord(const Channel& c) {
    std::vector<uint8_t> o;
    put16(o, 0);
    for (int i = 0; i < 3; i++) put16(o, uint32_t(std::lround(std::clamp(c.color[size_t(i)], 0.0, 1.0) * 65535)));
    put16(o, 0);
    put16(o, uint32_t(std::lround(std::clamp(c.opacity, 0.0, 1.0) * 100)));
    o.push_back(c.kind == ChannelKind::Spot ? 2 : c.selectedAreas ? 0 : 1);
    return o;
}

/// Whether the record the file had still says what the channel is.
bool recordHolds(const std::vector<uint8_t>& record, const Channel& c) {
    if (record.size() != 13) return false;
    Bytes r{record};
    const Display d = readDisplay(r);
    const int kind = c.kind == ChannelKind::Spot ? 2 : c.selectedAreas ? 0 : 1;
    return r.ok && d.color == c.color && d.opacity == c.opacity && d.kind == kind;
}

} // namespace

std::vector<Channel> psdChannels(const PsdChannelResources& resources, std::vector<PsdExtraPlane>& planes, bool transparencyFirst,
                                 int width, int height, bool deep) {
    std::vector<Channel> channels;
    if (planes.empty()) return channels;
    std::vector<std::string> names = unicodeNames(resources.unicodeNames);
    if (names.empty()) names = pascalNames(resources.names);
    std::vector<Display> displays;
    std::vector<std::vector<uint8_t>> records;
    if (!resources.displayInfo.empty()) {
        Bytes r{resources.displayInfo};
        r.u32();   // version 1
        while (r.ok && resources.displayInfo.size() - r.at >= 13 && displays.size() < 64) {
            const size_t start = r.at;
            displays.push_back(readDisplay(r));
            records.emplace_back(resources.displayInfo.begin() + long(start), resources.displayInfo.begin() + long(r.at));
        }
    } else if (!resources.oldDisplayInfo.empty()) {
        Bytes r{resources.oldDisplayInfo};
        while (r.ok && resources.oldDisplayInfo.size() - r.at >= 14 && displays.size() < 64) { displays.push_back(readDisplay(r)); r.u8(); }
    }
    std::vector<uint32_t> identifiers;
    { Bytes r{resources.identifiers}; while (!r.done() && identifiers.size() < 64) { const uint32_t id = r.u32(); if (r.ok) identifiers.push_back(id); } }
    // Some writers name the merged transparency as a channel too: then the lists have one entry more than there are
    // channels, and the first is the transparency's.
    const size_t skip = transparencyFirst && names.size() == planes.size() + 1 ? 1 : 0;
    const size_t skipDisplay = transparencyFirst && displays.size() == planes.size() + 1 ? 1 : 0;
    const size_t skipIds = transparencyFirst && identifiers.size() == planes.size() + 1 ? 1 : 0;
    std::set<std::string> used;
    for (size_t i = 0; i < planes.size() && int(i) < Document::maxChannels; i++) {
        PsdExtraPlane& plane = planes[i];
        if (plane.eight.size() != size_t(width) * size_t(height)) continue;
        Channel c;
        c.id = makeUuid();
        c.name = i + skip < names.size() ? names[i + skip] : "Alpha " + std::to_string(i + 1);
        if (c.name.empty()) c.name = "Alpha " + std::to_string(i + 1);
        const Display d = i + skipDisplay < displays.size() ? displays[i + skipDisplay] : Display{};
        c.kind = d.kind == 2 ? ChannelKind::Spot : ChannelKind::Alpha;
        c.color = d.color;
        c.opacity = d.opacity;
        c.selectedAreas = d.kind == 0;
        if (deep && plane.wide.size() == plane.eight.size()) {
            auto g = std::make_shared<Gray16>(width, height);
            std::copy(plane.wide.begin(), plane.wide.end(), g->data());
            c.image = Gray16Ptr(g);
        } else {
            auto g = std::make_shared<GrayImage>(width, height);
            std::copy(plane.eight.begin(), plane.eight.end(), g->data());
            c.image = GrayPtr(g);
        }
        auto carry = std::make_shared<PsdChannelCarry>();
        if (i + skipDisplay < records.size()) carry->displayInfo = records[i + skipDisplay];
        if (i + skipIds < identifiers.size()) carry->identifier = identifiers[i + skipIds];
        if (deep && plane.raw16.size() == plane.eight.size() * 2) {
            carry->plane16 = std::move(plane.raw16);
            carry->planeHash = psdMaskHash(c.image, true);
        }
        c.psdCarry = carry;
        channels.push_back(std::move(c));
    }
    return channels;
}

std::vector<std::pair<uint16_t, std::vector<uint8_t>>> psdChannelResourceBlocks(const Document& document) {
    std::vector<std::pair<uint16_t, std::vector<uint8_t>>> blocks;
    if (document.channels.empty()) return blocks;
    std::vector<uint8_t> pascal, unicode, display, ids;
    put32(display, 1);
    std::set<uint32_t> taken;
    for (const Channel& c : document.channels) if (c.psdCarry && c.psdCarry->identifier) taken.insert(c.psdCarry->identifier);
    uint32_t next = taken.empty() ? 2 : *taken.rbegin() + 1;
    std::set<uint32_t> written;
    for (const Channel& c : document.channels) {
        const std::vector<uint32_t> points = codePoints(c.name);
        // 1006: Pascal strings; what Mac Roman's ASCII half cannot say becomes '?', as Photoshop writes it.
        std::string roman;
        for (uint32_t p : points) roman += p < 0x80 ? char(p) : '?';
        if (roman.size() > 255) roman.resize(255);
        pascal.push_back(uint8_t(roman.size()));
        pascal.insert(pascal.end(), roman.begin(), roman.end());
        // 1045: UTF-16 with a terminating zero, counted.
        std::vector<uint16_t> units;
        for (uint32_t p : points) {
            if (p >= 0x10000) { p -= 0x10000; units.push_back(uint16_t(0xD800 + (p >> 10))); units.push_back(uint16_t(0xDC00 + (p & 0x3FF))); }
            else units.push_back(uint16_t(p));
        }
        units.push_back(0);
        put32(unicode, uint32_t(units.size()));
        for (uint16_t u : units) put16(unicode, u);
        // 1077: the file's own record while it still holds.
        const std::vector<uint8_t> record = c.psdCarry && recordHolds(c.psdCarry->displayInfo, c) ? c.psdCarry->displayInfo : displayRecord(c);
        display.insert(display.end(), record.begin(), record.end());
        uint32_t id = c.psdCarry ? c.psdCarry->identifier : 0;
        if (!id || written.count(id)) { while (taken.count(next) || written.count(next)) next++; id = next++; }
        written.insert(id);
        put32(ids, id);
    }
    blocks.push_back({1006, std::move(pascal)});
    blocks.push_back({1045, std::move(unicode)});
    blocks.push_back({1077, std::move(display)});
    blocks.push_back({1053, std::move(ids)});
    return blocks;
}

std::vector<uint8_t> psdChannelPlane(const Channel& channel, bool deep, int width, int height) {
    const size_t n = size_t(width) * size_t(height);
    std::vector<uint8_t> out(n * (deep ? 2 : 1));
    const AnyGray gray = grayAtDepth(channel.image, deep ? SampleType::U16 : SampleType::U8);
    if (deep) {
        if (channel.psdCarry && channel.psdCarry->plane16.size() == out.size() && channel.psdCarry->planeHash == psdMaskHash(channel.image, true))
            return channel.psdCarry->plane16;
        if (const Gray16Ptr& g = gray.u16(); g && g->width() == width && g->height() == height)
            for (size_t i = 0; i < n; i++) { const uint16_t v = to65535(g->data()[i]); out[i * 2] = uint8_t(v >> 8); out[i * 2 + 1] = uint8_t(v); }
        return out;
    }
    if (const GrayPtr& g = gray.u8(); g && g->width() == width && g->height() == height) std::copy(g->data(), g->data() + n, out.begin());
    return out;
}

} // namespace compositor
