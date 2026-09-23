// Photoshop .abr brushes, from the format as documented publicly (Archive Team's file format wiki; GIMP's
// and Krita's readers) and checked against real files. Versions 1 and 2 list computed (round) and sampled
// brushes one after the other. Versions 6 to 10 are 8BIM sections: "samp" holds the sampled tips, each
// under a UUID, and "desc" is an action descriptor whose "Brsh" list holds the presets, naming their tip
// by that UUID and carrying the dynamics.
#include "brushformats.h"
#include "photoshop.h"
#include <algorithm>
#include <cmath>
#include <map>

namespace compositor {

using namespace photoshop;

namespace {

/// PackBits rows: the byte count of every row first, then the rows.
bool unpackRows(Reader& r, int width, int height, int bytesPerSample, std::vector<uint8_t>& out) {
    const size_t rowBytes = size_t(width) * size_t(bytesPerSample);
    out.assign(rowBytes * size_t(height), 0);
    std::vector<uint16_t> counts(static_cast<size_t>(height));
    for (int y = 0; y < height; y++) counts[size_t(y)] = r.u16();
    for (int y = 0; y < height; y++) {
        const size_t end = r.position() + counts[size_t(y)];
        uint8_t* row = out.data() + rowBytes * size_t(y);
        size_t x = 0;
        while (r.position() < end && x < rowBytes) {
            const int n = int8_t(r.u8());
            if (n >= 0) { for (int i = 0; i <= n && x < rowBytes; i++) row[x++] = r.u8(); }
            else if (n != -128) { const uint8_t v = r.u8(); for (int i = 0; i <= -n && x < rowBytes; i++) row[x++] = v; }
        }
        r.seek(end);
    }
    return true;
}

/// All the tips one file may decode, in pixels. PackBits lets a small file claim large tips; real sets hold a
/// few hundred tips of a few megapixels at most.
constexpr long long tipPixelBudget = 256LL * 1000 * 1000;

/// A sampled tip: its bounds, depth (8 or 16 bits, the high byte kept) and raw or PackBits data. `budget` is
/// what the file may still decode; the tip is refused when it would exceed it.
std::shared_ptr<GrayImage> readSampledTip(Reader& r, long long& budget) {
    const int32_t top = r.i32(), left = r.i32(), bottom = r.i32(), right = r.i32();
    const int depth = r.u16();
    const int compression = r.u8();
    const int64_t width = int64_t(right) - left, height = int64_t(bottom) - top;
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192 || (depth != 8 && depth != 16)) return nullptr;
    if (width * height > budget) return nullptr;
    budget -= width * height;
    const int bytes = depth / 8;
    std::vector<uint8_t> pixels;
    const int w = int(width), h = int(height);
    if (compression == 0) {
        const uint8_t* raw = r.bytes(size_t(w) * size_t(h) * size_t(bytes));
        pixels.assign(raw, raw + size_t(w) * size_t(h) * size_t(bytes));
    } else if (!unpackRows(r, w, h, bytes, pixels)) return nullptr;
    auto tip = std::make_shared<GrayImage>(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) tip->at(x, y) = pixels[(size_t(y) * size_t(w) + size_t(x)) * size_t(bytes)];
    return tip;
}

/// A computed (round) brush as an image: an ellipse of `diameter` with `hardness` 0..1 of its radius solid.
std::shared_ptr<GrayImage> roundTip(double diameter, double hardness, double roundness) {
    const int w = std::clamp(int(std::ceil(diameter)), 1, 1024), h = std::clamp(int(std::ceil(diameter * std::clamp(roundness, 0.01, 1.0))), 1, 1024);
    auto tip = std::make_shared<GrayImage>(w, h, 0);
    const double rx = w / 2.0, ry = h / 2.0, solid = std::clamp(hardness, 0.0, 1.0);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const double dx = (x + 0.5 - rx) / rx, dy = (y + 0.5 - ry) / ry, d = std::sqrt(dx * dx + dy * dy);
            double v = d >= 1 ? 0 : d <= solid ? 1 : 1 - (d - solid) / (1 - solid);
            v = v * v * (3 - 2 * v);   // smoothstep: the soft rim Photoshop draws
            tip->at(x, y) = uint8_t(std::lround(v * 255));
        }
    return tip;
}

// ---- Versions 1 and 2 ----------------------------------------------------------------------------

std::optional<BrushImport> readOld(Reader& r, int version, const std::string& name, std::string* error) {
    BrushImport import;
    import.set = name;
    long long budget = tipPixelBudget;
    const int count = r.u16();
    for (int i = 0; i < count; i++) {
        const int type = r.u16();
        const uint32_t size = r.u32();
        const size_t next = r.position() + size;
        if (next > r.position() + r.remaining()) break;
        TipPreset preset;
        preset.name = name + " " + std::to_string(i + 1);
        if (type == 1) {   // computed
            r.u32();
            const double spacing = r.u16();
            const double diameter = r.u16(), roundness = r.u16(), angle = r.i16(), hardness = r.u16();
            preset.tip.shape = roundTip(diameter, hardness / 100.0, roundness / 100.0);
            preset.tip.spacing = spacing > 0 ? spacing / 100.0 : 0.25;
            preset.tip.angle = angle;
            preset.diameter = std::max(1.0, diameter);
        } else if (type == 2) {   // sampled
            r.u32();
            const double spacing = r.u16();
            if (version == 2) { std::string title = r.unicode(); if (!title.empty()) preset.name = title; }
            r.u8();            // antialiasing
            r.skip(8);         // bounds as shorts; the long bounds follow
            preset.tip.shape = readSampledTip(r, budget);
            preset.tip.spacing = spacing > 0 ? spacing / 100.0 : 0.25;
            if (preset.tip.shape) preset.diameter = std::max(preset.tip.shape->width(), preset.tip.shape->height());
        }
        r.seek(next);
        if (preset.tip.shape && preset.tip.normalize()) import.brushes.push_back(std::move(preset));
    }
    if (import.brushes.empty()) { if (error) *error = "the file holds no brush this reader can use"; return std::nullopt; }
    return import;
}

// ---- Versions 6 to 10 ----------------------------------------------------------------------------

/// A dynamics setting (brVr): its jitter as 0..1, and whether pen pressure controls it.
struct Variation { double jitter = 0; bool pressure = false; double minimum = 0; };
Variation variation(const Descriptor* d) {
    Variation v;
    if (!d) return v;
    v.jitter = d->numberAt("jitter", 0) / 100.0;
    v.pressure = int(d->numberAt("bVTy", 0)) == 2;   // 0 off, 1 fade, 2 pen pressure, 3 tilt, 4 stylus wheel
    v.minimum = d->numberAt("Mnm ", 0) / 100.0;
    return v;
}

std::optional<BrushImport> readSections(Reader& r, const std::string& name, std::string* error) {
    BrushImport import;
    import.set = name;
    std::map<std::string, std::shared_ptr<GrayImage>> samples;
    std::vector<std::shared_ptr<GrayImage>> sampleOrder;
    std::optional<Descriptor> presets;
    long long budget = tipPixelBudget;
    while (r.remaining() >= 12) {
        if (r.chars(4) != "8BIM") break;
        const std::string key = r.chars(4);
        const uint32_t length = r.u32();
        const size_t end = r.position() + length;
        if (length > r.remaining()) break;
        if (key == "samp") {
            while (r.position() + 4 <= end) {
                const uint32_t size = r.u32();
                const size_t start = r.position();
                size_t next = start + size;
                next += (4 - next % 4) % 4;
                if (size == 0 || start + size > end) break;
                try {
                    const std::string id = r.pascal(1);
                    // After the UUID: fields whose meaning is not documented; the tip's bounds follow them. The
                    // two layouts in the wild put the bounds 301 or 47 bytes after the entry's start.
                    std::shared_ptr<GrayImage> tip;
                    for (size_t offset : {size_t(301), size_t(47)}) {
                        if (start + offset + 19 > start + size) continue;
                        r.seek(start + offset);
                        try { tip = readSampledTip(r, budget); } catch (Truncated&) { tip.reset(); }
                        if (tip) break;
                    }
                    if (tip) { samples[id] = tip; sampleOrder.push_back(tip); }
                } catch (Truncated&) {}
                r.seek(std::min(next, end));
            }
        } else if (key == "desc") {
            try { presets = readDescriptor(r); } catch (Truncated&) { import.notes.push_back("the presets' settings could not be read; the tips come in with default settings"); }
        }
        r.seek(end);
    }

    const Descriptor* list = presets ? presets->item("Brsh") : nullptr;
    std::vector<bool> used(sampleOrder.size(), false);
    std::map<std::string, int> ignored;   // settings tip brushes cannot express, counted for the notes
    if (list)
        for (const Descriptor& entry : list->list) {
            const Descriptor* brush = entry.item("Brsh");
            if (!brush) continue;
            TipPreset preset;
            if (const Descriptor* n = entry.item("Nm  ")) preset.name = n->text;
            BrushTip& tip = preset.tip;
            const double diameter = brush->numberAt("Dmtr", 30);
            tip.angle = brush->numberAt("Angl", 0);
            tip.roundness = brush->numberAt("Rndn", 100) / 100.0;
            const bool interval = brush->numberAt("Intr", 1) != 0;
            tip.spacing = interval ? brush->numberAt("Spcn", 25) / 100.0 : 0.25;
            tip.flipX = brush->numberAt("flipX", 0) != 0;
            tip.flipY = brush->numberAt("flipY", 0) != 0;
            if (brush->classId == "computedBrush") {
                tip.shape = roundTip(std::min(diameter, 1024.0), brush->numberAt("Hrdn", 100) / 100.0, 1);
            } else if (const Descriptor* id = brush->item("sampledData")) {
                auto it = samples.find(id->text);
                if (it == samples.end()) continue;
                tip.shape = it->second;
                for (size_t i = 0; i < sampleOrder.size(); i++) if (sampleOrder[i] == it->second) used[i] = true;
            }
            if (!tip.shape) continue;
            preset.diameter = std::clamp(diameter, 1.0, 2000.0);
            if (entry.numberAt("useTipDynamics", 0)) {
                const Variation size = variation(entry.item("szVr"));
                tip.sizeJitter = size.jitter;
                if (size.pressure) { tip.pressureSize = 1; tip.minimumSize = entry.numberAt("minimumDiameter", 0) / 100.0; }
                const Variation angle = variation(entry.item("angleDynamics"));
                tip.angleJitter = angle.jitter * 180;
                if (int(entry.item("angleDynamics") ? entry.item("angleDynamics")->numberAt("bVTy", 0) : 0) == 6) tip.followStroke = true;   // direction
                tip.randomFlipX = entry.numberAt("flipX", 0) != 0;
                tip.randomFlipY = entry.numberAt("flipY", 0) != 0;
            }
            if (entry.numberAt("useScatter", 0)) {
                tip.scatter = variation(entry.item("scatterDynamics")).jitter;
                tip.scatterBothAxes = entry.numberAt("bothAxes", 0) != 0;
                tip.count = int(std::lround(entry.numberAt("Cnt ", 1)));
            }
            if (entry.numberAt("usePaintDynamics", 0)) {
                const Variation flow = variation(entry.item("prVr")), opacity = variation(entry.item("opVr"));
                tip.flowJitter = std::max(flow.jitter, opacity.jitter);
                if (flow.pressure || opacity.pressure) tip.pressureFlow = 1;
            }
            for (const char* feature : {"useTexture", "useColorDynamics", "Wtdg", "Nose", "Rpt "})
                if (entry.numberAt(feature, 0)) ignored[feature]++;
            if (const Descriptor* dual = entry.item("dualBrush"); dual && dual->numberAt("useDualBrush", 0)) ignored["dualBrush"]++;
            if (preset.name.empty()) preset.name = name + " " + std::to_string(import.brushes.size() + 1);
            if (tip.normalize()) import.brushes.push_back(std::move(preset));
        }
    // Tips no preset uses (or every tip, when the presets could not be read) still come in, as plain brushes.
    for (size_t i = 0; i < sampleOrder.size(); i++) {
        if (used[i]) continue;
        TipPreset preset;
        preset.name = name + " tip " + std::to_string(i + 1);
        preset.tip.shape = sampleOrder[i];
        preset.diameter = std::clamp(double(std::max(sampleOrder[i]->width(), sampleOrder[i]->height())), 1.0, 2000.0);
        if (preset.tip.normalize()) import.brushes.push_back(std::move(preset));
    }
    static const std::map<std::string, std::string> names{
        {"useTexture", "texture"}, {"dualBrush", "dual brush"}, {"useColorDynamics", "colour dynamics"},
        {"Wtdg", "wet edges"}, {"Nose", "noise"}, {"Rpt ", "airbrush build-up"}};
    for (const auto& [feature, count] : ignored)
        import.notes.push_back(std::to_string(count) + (count == 1 ? " brush uses " : " brushes use ") + names.at(feature) + ", which is left out");
    if (import.brushes.empty()) { if (error) *error = "the file holds no brush this reader can use"; return std::nullopt; }
    return import;
}

} // namespace

std::optional<BrushImport> readAbr(const uint8_t* data, size_t size, const std::string& name, std::string* error) {
    try {
        Reader r(data, size);
        const int version = r.u16();
        if (version == 1 || version == 2) return readOld(r, version, name, error);
        if (version >= 6 && version <= 10) { r.u16(); return readSections(r, name, error); }
        if (error) *error = "Photoshop brush version " + std::to_string(version) + " is not supported";
    } catch (Truncated&) {
        if (error) *error = "the brush file ends early or is damaged";
    }
    return std::nullopt;
}

} // namespace compositor
