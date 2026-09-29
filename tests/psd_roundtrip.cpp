// Opens PSDs, exports them again and checks that what NekoPhoto carries came back byte for byte: every
// layer record's tagged blocks (but the ones we write ourselves), Blend If ranges and mask section, the
// image resources and the global blocks. `psd_roundtrip FILE_OR_DIR...`; exit 1 when anything was lost.
// Patchy's fixtures (Photoshop-saved) are the corpus: psd_roundtrip ../Patchy/test-fixtures/psd
// A 16-bit file must also come back with every layer's channel data byte for byte (the carried planes, psd_carry.h),
// and so must a CMYK or Lab file at either depth, which must also reopen in its own colour mode with its profile.
// PSD_ROUNDTRIP_16=1 or PSD_ROUNDTRIP_32=1 convert each file to 16 or 32 bits first (Image > Mode).
#include "compositor/psd.h"
#include "compositor/psd_writer.h"
#include "compositor/psd_carry.h"
#include "psd/psd_binary.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using patchy::psd::BigEndianReader;
using Bytes = std::vector<uint8_t>;

namespace {

struct RecordDump {
    std::string name;
    std::map<std::string, Bytes> blocks;
    Bytes ranges, mask;
    int section = 0;
    std::string blend;
    uint8_t flags = 0, opacity = 255, clipping = 0;
    std::vector<std::pair<int, uint64_t>> channelLengths;
    std::map<int, Bytes> channels;   // a 16-bit file's channel data as stored
};

struct FileDump {
    std::map<int, Bytes> resources;
    std::map<std::string, Bytes> globals;
    std::vector<RecordDump> records;
    bool reduced = false;   // PSB or not 8-bit: masks are written anew, not carried
    int depth = 8;
    int mode = 3;           // the header's colour mode: 3 RGB, 4 CMYK, 9 Lab
};

std::string key4(BigEndianReader& r) { auto s = r.read_span(4); return std::string(s.begin(), s.end()); }

bool longKey(const std::string& k) {
    static const std::set<std::string> keys{"LMsk", "Lr16", "Lr32", "Layr", "Mt16", "Mt32", "Mtrn", "Alph", "FMsk", "lnk2", "FEid", "FXid", "PxSD", "cinf"};
    return keys.count(k) > 0;
}

FileDump dump(const Bytes& file) {
    FileDump d;
    BigEndianReader r(file);
    auto header = patchy::psd::read_header(r);
    const bool psb = header.large_document;
    d.reduced = psb || header.depth != 8;
    d.depth = header.depth;
    d.mode = int(header.color_mode);
    r.skip(r.read_u32());
    const size_t resourcesEnd = r.position() + r.read_u32() + 4;
    while (r.position() + 12 <= resourcesEnd) {
        if (key4(r) != "8BIM") break;
        const int id = r.read_u16();
        const size_t nameLen = r.read_u8();
        r.skip(nameLen + ((nameLen + 1) & 1));
        const uint32_t len = r.read_u32();
        d.resources[id] = r.read_bytes(len);
        if (len & 1) r.skip(1);
    }
    auto length = [&](BigEndianReader& in) -> uint64_t { return psb ? in.read_u64() : in.read_u32(); };
    const uint64_t layerMaskLen = length(r);
    const size_t layerMaskEnd = r.position() + layerMaskLen;
    if (!layerMaskLen) return d;
    const uint64_t infoLen = length(r);
    const size_t infoEnd = r.position() + infoLen;
    auto readRecords = [&](BigEndianReader& in) {
        int count = int16_t(in.read_u16());
        count = count < 0 ? -count : count;
        for (int i = 0; i < count; i++) {
            RecordDump rec;
            in.skip(16);
            const int channels = in.read_u16();
            for (int c = 0; c < channels; c++) {
                const int id = int16_t(in.read_u16());
                rec.channelLengths.push_back({id, psb ? in.read_u64() : in.read_u32()});
            }
            in.skip(4);
            rec.blend = key4(in);
            rec.opacity = in.read_u8(); rec.clipping = in.read_u8(); rec.flags = in.read_u8(); in.skip(1);
            const uint32_t extra = in.read_u32();
            const size_t extraEnd = in.position() + extra;
            rec.mask = in.read_bytes(in.read_u32());
            rec.ranges = in.read_bytes(in.read_u32());
            const size_t nameLen = in.read_u8();
            auto name = in.read_span(nameLen);
            rec.name.assign(name.begin(), name.end());
            for (size_t used = 1 + nameLen; used % 4; used++) in.skip(1);
            while (in.position() + 12 <= extraEnd) {
                const std::string sig = key4(in);
                if (sig != "8BIM" && sig != "8B64") break;
                const std::string key = key4(in);
                const uint64_t len = psb && longKey(key) ? in.read_u64() : in.read_u32();
                rec.blocks[key] = in.read_bytes(size_t(len));
                if (key == "lsct" && len >= 4) rec.section = int(uint32_t(rec.blocks[key][0]) << 24 | uint32_t(rec.blocks[key][1]) << 16 | uint32_t(rec.blocks[key][2]) << 8 | rec.blocks[key][3]);
            }
            in.skip(extraEnd - in.position());
            d.records.push_back(std::move(rec));
        }
        if (d.depth == 16 || d.mode == 4 || d.mode == 9)
            for (RecordDump& rec : d.records)
                for (auto& [id, length] : rec.channelLengths) rec.channels[id] = in.read_bytes(size_t(length));
    };
    if (infoLen) readRecords(r);
    r.skip(infoEnd - r.position());
    if (r.position() + 4 <= layerMaskEnd) r.skip(r.read_u32());
    while (r.position() + 12 <= layerMaskEnd) {
        const std::string sig = key4(r);
        if (sig != "8BIM" && sig != "8B64") break;
        const std::string key = key4(r);
        const uint64_t len = psb && longKey(key) ? r.read_u64() : r.read_u32();
        const size_t start = r.position();
        if (d.records.empty() && (key == "Lr16" || key == "Lr32")) { readRecords(r); r.skip(start + len - r.position()); }
        else d.globals[key] = r.read_bytes(size_t(len));
        for (uint64_t n = len; n % 4; n++) r.skip(1);
    }
    return d;
}

Bytes readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// Checks one file; prints what was lost. Returns false when anything carried did not come back.
bool check(const fs::path& path, int& carriedBlocks, int& channelsBack) {
    std::string error;
    auto imported = compositor::importPsd(path.string(), &error);
    if (!imported) { std::printf("SKIP %s: %s\n", path.filename().string().c_str(), error.c_str()); return true; }
    compositor::PsdExportSummary summary;
    // PSD_ROUNDTRIP_PSB=1: export as PSB instead (Photoshop's large format, 64-bit lengths).
    compositor::PsdExportOptions options;
    options.large = std::getenv("PSD_ROUNDTRIP_PSB") != nullptr;
    // PSD_ROUNDTRIP_16=1: convert to 16 bits first (Image > Mode), so the 16-bit writer carries the same blocks.
    const bool sixteen = std::getenv("PSD_ROUNDTRIP_16") != nullptr;
    if (sixteen && !compositor::convertSampleType(imported->document, compositor::SampleType::U16, &error)) { std::printf("SKIP %s: %s\n", path.filename().string().c_str(), error.c_str()); return true; }
    // PSD_ROUNDTRIP_32=1: convert to 32 bits first, so the 32-bit writer ('Lr32', float channels) carries the same blocks.
    const bool thirtyTwo = std::getenv("PSD_ROUNDTRIP_32") != nullptr;
    if (thirtyTwo && !compositor::convertSampleType(imported->document, compositor::SampleType::F32, &error)) { std::printf("SKIP %s: %s\n", path.filename().string().c_str(), error.c_str()); return true; }
    Bytes out = compositor::encodePsd(imported->document, options, &summary, &error);
    if (out.empty()) { std::printf("FAIL %s: export: %s\n", path.filename().string().c_str(), error.c_str()); return false; }
    FileDump a, b;
    try { a = dump(readFile(path)); } catch (std::exception& e) { std::printf("SKIP %s: unreadable here (%s)\n", path.filename().string().c_str(), e.what()); return true; }
    try { b = dump(out); } catch (std::exception& e) { std::printf("FAIL %s: our file does not parse: %s\n", path.filename().string().c_str(), e.what()); return false; }
    std::vector<std::string> problems;
    // Records: ours has the same folders and layers in the same order, unless the importer dropped some.
    const bool reduced = a.reduced || sixteen || thirtyTwo;
    static const std::set<std::string> ours{"luni", "lsct", "lsdk", "lyid", "iOpa", "levl", "curv", "hue2", "expA", "grdm"};
    if (a.records.empty()) {}   // a flat file opens as one layer
    else if (a.records.size() != b.records.size()) problems.push_back("record count " + std::to_string(a.records.size()) + " -> " + std::to_string(b.records.size()));
    else for (size_t i = 0; i < a.records.size(); i++) {
        const RecordDump &x = a.records[i], &y = b.records[i];
        for (auto& [key, data] : x.blocks) {
            if (ours.count(key)) continue;
            auto it = y.blocks.find(key);
            if (it == y.blocks.end()) problems.push_back("\"" + x.name + "\": block " + key + " lost");
            else if (it->second != data) problems.push_back("\"" + x.name + "\": block " + key + " changed");
            else carriedBlocks++;
        }
        if (x.section != 3 && x.blend != y.blend) problems.push_back("\"" + x.name + "\": blend " + x.blend + " -> " + y.blend);
        if (x.section != y.section) problems.push_back("\"" + x.name + "\": section " + std::to_string(x.section) + " -> " + std::to_string(y.section));
        if (x.section != 3 && (x.flags & 0x13) != (y.flags & 0x13)) problems.push_back("\"" + x.name + "\": flags " + std::to_string(x.flags) + " -> " + std::to_string(y.flags));
        if (x.section != 3 && (x.opacity != y.opacity || x.clipping != y.clipping)) problems.push_back("\"" + x.name + "\": opacity or clipping changed");
        if (x.section != 3 && x.ranges != y.ranges) problems.push_back("\"" + x.name + "\": Blend If ranges changed");
        if (x.section != 3 && !reduced && !x.mask.empty() && x.mask != y.mask) problems.push_back("\"" + x.name + "\": mask section changed");
        if (x.blocks.count("iOpa") && x.blocks.at("iOpa") != (y.blocks.count("iOpa") ? y.blocks.at("iOpa") : Bytes{255, 0, 0, 0})) problems.push_back("\"" + x.name + "\": Fill changed");
        // A 16-bit layer's pixels, and a CMYK or Lab layer's: exactly the bytes it was stored with.
        if ((a.depth == 16 || ((a.mode == 4 || a.mode == 9) && !sixteen)) && x.section != 3)
            for (auto& [id, data] : x.channels) {
                if (id < -1) continue;   // masks: the mask section's check
                if (!y.channels.count(id) || y.channels.at(id) != data) problems.push_back("\"" + x.name + "\": " + (a.depth == 16 ? "16-bit " : "") + "channel " + std::to_string(id) + " changed");
            }
    }
    static const std::set<int> droppedResources{1005, 1033, 1036, 1024, 1026, 1069, 1072, 1044, 1006, 1045, 1053, 1077, 1007, 1047, 1039, 1041, 1013, 1014, 1016, 1017, 1018};
    for (auto& [id, data] : a.resources) {
        if (droppedResources.count(id)) continue;
        auto it = b.resources.find(id);
        if (it == b.resources.end() || it->second != data) problems.push_back("resource " + std::to_string(id) + (it == b.resources.end() ? " lost" : " changed"));
    }
    static const std::set<std::string> droppedGlobals{"Lr16", "Lr32", "Layr", "LMsk", "Mt16", "Mt32", "Mtrn", "Alph"};
    for (auto& [key, data] : a.globals) {
        if (droppedGlobals.count(key) || ((sixteen || thirtyTwo) && (key == "FEid" || key == "FXid"))) continue;   // 8-bit filter caches stay out of a deep file
        auto it = b.globals.find(key);
        if (it == b.globals.end() || it->second != data) problems.push_back("global " + key + (it == b.globals.end() ? " lost" : " changed"));
    }
    // Round trip once more: our own file must read back.
    const fs::path again = fs::temp_directory_path() / ("psd_roundtrip_" + std::to_string(::getpid()) + (options.large ? ".psb" : ".psd"));
    { std::ofstream o(again, std::ios::binary); o.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size())); }
    auto reopened = compositor::importPsd(again.string(), &error);
    if (!reopened) problems.push_back("our file does not reopen: " + error);
    else if (sixteen && reopened->document.sampleType != compositor::SampleType::U16) problems.push_back("our file does not reopen at 16 bits");
    else if (thirtyTwo && reopened->document.sampleType != compositor::SampleType::F32) problems.push_back("our file does not reopen at 32 bits");
    if (reopened && reopened->document.colorMode != imported->document.colorMode) problems.push_back("our file does not reopen in the same colour mode");
    if (b.mode != a.mode && (a.mode == 3 || a.mode == 4 || a.mode == 9)) problems.push_back("colour mode " + std::to_string(a.mode) + " -> " + std::to_string(b.mode));
    // A CMYK file's profile (1039) is the document's and comes back byte for byte.
    if (a.mode == 4 && a.resources.count(1039) && (!b.resources.count(1039) || b.resources.at(1039) != a.resources.at(1039))) problems.push_back("CMYK profile (1039) changed");
    // Alpha and spot channels: the same names, kinds, display and pixels (a 16-bit file's samples as stored), and
    // the resources that describe them byte for byte.
    if (reopened) {
        const auto& was = imported->document.channels;
        const auto& now = reopened->document.channels;
        if (was.size() != now.size()) problems.push_back("channel count " + std::to_string(was.size()) + " -> " + std::to_string(now.size()));
        else for (size_t i = 0; i < was.size(); i++) {
            const compositor::Channel &x = was[i], &y = now[i];
            const std::string label = "channel \"" + x.name + "\": ";
            if (x.name != y.name) problems.push_back(label + "renamed \"" + y.name + "\"");
            if (x.kind != y.kind || x.selectedAreas != y.selectedAreas || x.color != y.color || x.opacity != y.opacity) problems.push_back(label + "display changed");
            if (compositor::psdMaskHash(x.image, true) != compositor::psdMaskHash(y.image, true)) problems.push_back(label + "pixels changed");
            if (x.psdCarry && y.psdCarry && (x.psdCarry->displayInfo != y.psdCarry->displayInfo || x.psdCarry->identifier != y.psdCarry->identifier || (!x.psdCarry->plane16.empty() && x.psdCarry->plane16 != y.psdCarry->plane16)))
                problems.push_back(label + "stored record or samples changed");
            else if (problems.empty()) channelsBack++;
        }
        if (!was.empty())
            for (int id : {1006, 1045, 1077, 1053})
                if (a.resources.count(id) && (!b.resources.count(id) || a.resources.at(id) != b.resources.at(id))) problems.push_back("channel resource " + std::to_string(id) + " changed");
    }
    fs::remove(again);
    std::printf("%s %s", problems.empty() ? "ok  " : "FAIL", path.filename().string().c_str());
    if (!imported->texts.empty()) std::printf("  [%zu text layer(s) opened as text]", imported->texts.size());
    if (std::getenv("PSD_ROUNDTRIP_NOTES")) for (auto& n : imported->notes) if (std::string(std::getenv("PSD_ROUNDTRIP_NOTES")) == "all" || n.find("text") != std::string::npos) std::printf("\n     note: %s", n.c_str());
    for (auto& p : problems) std::printf("\n     %s", p.c_str());
    std::printf("\n");
    return problems.empty();
}

} // namespace

int main(int argc, char** argv) {
    std::vector<fs::path> files;
    for (int i = 1; i < argc; i++) {
        if (fs::is_directory(argv[i])) { for (auto& e : fs::directory_iterator(argv[i])) if (e.path().extension() == ".psd" || e.path().extension() == ".psb") files.push_back(e.path()); }
        else files.push_back(argv[i]);
    }
    std::sort(files.begin(), files.end());
    int failed = 0, carried = 0, channels = 0;
    for (auto& f : files) if (!check(f, carried, channels)) failed++;
    std::printf("%zu files, %d failed, %d carried blocks came back\n", files.size(), failed, carried);
    std::printf("%d alpha and spot channels came back\n", channels);
    return failed ? 1 : 0;
}
