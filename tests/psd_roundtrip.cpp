// Opens PSDs, exports them again and checks that what NekoPhoto carries came back byte for byte: every
// layer record's tagged blocks (but the ones we write ourselves), Blend If ranges and mask section, the
// image resources and the global blocks. `psd_roundtrip FILE_OR_DIR...`; exit 1 when anything was lost.
// Patchy's fixtures (Photoshop-saved) are the corpus: psd_roundtrip ../Patchy/test-fixtures/psd
#include "compositor/psd.h"
#include "compositor/psd_writer.h"
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
};

struct FileDump {
    std::map<int, Bytes> resources;
    std::map<std::string, Bytes> globals;
    std::vector<RecordDump> records;
    bool reduced = false;   // PSB or not 8-bit: masks are written anew, not carried
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
            in.skip(size_t(channels) * (psb ? 10 : 6));
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
bool check(const fs::path& path, int& carriedBlocks) {
    std::string error;
    auto imported = compositor::importPsd(path.string(), &error);
    if (!imported) { std::printf("SKIP %s: %s\n", path.filename().c_str(), error.c_str()); return true; }
    compositor::PsdExportSummary summary;
    // PSD_ROUNDTRIP_PSB=1: export as PSB instead (Photoshop's large format, 64-bit lengths).
    compositor::PsdExportOptions options;
    options.large = std::getenv("PSD_ROUNDTRIP_PSB") != nullptr;
    Bytes out = compositor::encodePsd(imported->document, options, &summary, &error);
    if (out.empty()) { std::printf("FAIL %s: export: %s\n", path.filename().c_str(), error.c_str()); return false; }
    FileDump a, b;
    try { a = dump(readFile(path)); } catch (std::exception& e) { std::printf("SKIP %s: unreadable here (%s)\n", path.filename().c_str(), e.what()); return true; }
    try { b = dump(out); } catch (std::exception& e) { std::printf("FAIL %s: our file does not parse: %s\n", path.filename().c_str(), e.what()); return false; }
    std::vector<std::string> problems;
    // Records: ours has the same folders and layers in the same order, unless the importer dropped some.
    const bool reduced = a.reduced;
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
    }
    static const std::set<int> droppedResources{1005, 1033, 1036, 1024, 1026, 1069, 1072, 1044, 1006, 1045, 1053, 1077, 1007, 1047, 1039, 1041, 1013, 1014, 1016, 1017, 1018};
    for (auto& [id, data] : a.resources) {
        if (droppedResources.count(id)) continue;
        auto it = b.resources.find(id);
        if (it == b.resources.end() || it->second != data) problems.push_back("resource " + std::to_string(id) + (it == b.resources.end() ? " lost" : " changed"));
    }
    static const std::set<std::string> droppedGlobals{"Lr16", "Lr32", "Layr", "LMsk", "Mt16", "Mt32", "Mtrn", "Alph"};
    for (auto& [key, data] : a.globals) {
        if (droppedGlobals.count(key)) continue;
        auto it = b.globals.find(key);
        if (it == b.globals.end() || it->second != data) problems.push_back("global " + key + (it == b.globals.end() ? " lost" : " changed"));
    }
    // Round trip once more: our own file must read back.
    const fs::path again = fs::temp_directory_path() / ("psd_roundtrip_" + std::to_string(::getpid()) + (options.large ? ".psb" : ".psd"));
    { std::ofstream o(again, std::ios::binary); o.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size())); }
    if (!compositor::importPsd(again.string(), &error)) problems.push_back("our file does not reopen: " + error);
    fs::remove(again);
    std::printf("%s %s", problems.empty() ? "ok  " : "FAIL", path.filename().c_str());
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
    int failed = 0, carried = 0;
    for (auto& f : files) if (!check(f, carried)) failed++;
    std::printf("%zu files, %d failed, %d carried blocks came back\n", files.size(), failed, carried);
    return failed ? 1 : 0;
}
