#include "plist.h"
#include <cstring>

namespace compositor::plist {

namespace {

uint64_t bigEndian(const uint8_t* p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) v = (v << 8) | p[i];
    return v;
}

void appendUtf8(std::string& out, uint32_t c) {
    if (c < 0x80) out += char(c);
    else if (c < 0x800) { out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { out += char(0xE0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
    else { out += char(0xF0 | (c >> 18)); out += char(0x80 | ((c >> 12) & 0x3F)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F)); }
}

} // namespace

std::optional<Binary> Binary::parse(const uint8_t* data, size_t size) {
    if (size < 8 + 32 || std::memcmp(data, "bplist00", 8) != 0) return std::nullopt;
    const uint8_t* trailer = data + size - 32;
    const size_t offsetSize = trailer[6], refSize = trailer[7];
    const uint64_t count = bigEndian(trailer + 8, 8), top = bigEndian(trailer + 16, 8), table = bigEndian(trailer + 24, 8);
    if (offsetSize < 1 || offsetSize > 8 || refSize < 1 || refSize > 8 || count == 0 || count > size || top >= count
        || table > size - 32 || count * offsetSize > size - 32 - table) return std::nullopt;
    Binary out;
    out.top = size_t(top);
    out.objects.resize(size_t(count));
    const size_t limit = size - 32;
    for (uint64_t i = 0; i < count; i++) {
        const uint64_t at = bigEndian(data + table + i * offsetSize, offsetSize);
        if (at >= limit) return std::nullopt;
        Value& v = out.objects[size_t(i)];
        const uint8_t marker = data[at];
        const int high = marker >> 4, low = marker & 0xF;
        size_t p = size_t(at) + 1;
        // A length: the low nibble, or 0xF and an integer object after the marker.
        auto length = [&]() -> std::optional<uint64_t> {
            if (low != 0xF) return uint64_t(low);
            if (p >= limit || (data[p] >> 4) != 0x1) return std::nullopt;
            const size_t n = size_t(1) << (data[p] & 0xF);
            if (n > 8 || p + 1 + n > limit) return std::nullopt;
            const uint64_t value = bigEndian(data + p + 1, n);
            p += 1 + n;
            return value;
        };
        auto fits = [&](uint64_t bytes) { return bytes <= limit && p <= limit - bytes; };
        switch (high) {
        case 0x0:
            if (marker == 0x08 || marker == 0x09) { v.kind = Value::Kind::Bool; v.number = marker == 0x09; }
            break;
        case 0x1: {
            const size_t n = size_t(1) << low;
            if (n > 16 || !fits(n)) return std::nullopt;
            v.kind = Value::Kind::Integer;
            v.number = double(int64_t(bigEndian(data + p + (n > 8 ? n - 8 : 0), std::min<size_t>(n, 8))));
            break;
        }
        case 0x2: {
            const size_t n = size_t(1) << low;
            if ((n != 4 && n != 8) || !fits(n)) return std::nullopt;
            v.kind = Value::Kind::Real;
            if (n == 4) { uint32_t bits = uint32_t(bigEndian(data + p, 4)); float f; std::memcpy(&f, &bits, 4); v.number = f; }
            else { uint64_t bits = bigEndian(data + p, 8); double d; std::memcpy(&d, &bits, 8); v.number = d; }
            break;
        }
        case 0x4: case 0x5: case 0x6: {
            auto n = length();
            if (!n) return std::nullopt;
            const uint64_t bytes = high == 0x6 ? *n * 2 : *n;
            if (!fits(bytes)) return std::nullopt;
            if (high == 0x4) { v.kind = Value::Kind::Data; v.data.assign(data + p, data + p + bytes); }
            else if (high == 0x5) { v.kind = Value::Kind::String; v.text.assign(reinterpret_cast<const char*>(data + p), size_t(bytes)); }
            else {
                v.kind = Value::Kind::String;
                for (uint64_t k = 0; k < *n; k++) {
                    uint32_t c = uint32_t(bigEndian(data + p + 2 * k, 2));
                    if (c >= 0xD800 && c < 0xDC00 && k + 1 < *n) { const uint32_t lowSurrogate = uint32_t(bigEndian(data + p + 2 * (k + 1), 2)); c = 0x10000 + ((c - 0xD800) << 10) + (lowSurrogate - 0xDC00); k++; }
                    appendUtf8(v.text, c);
                }
            }
            break;
        }
        case 0x8:
            if (!fits(size_t(low) + 1)) return std::nullopt;
            v.kind = Value::Kind::Uid;
            v.uid = bigEndian(data + p, size_t(low) + 1);
            break;
        case 0xA: case 0xC: case 0xD: {
            auto n = length();
            if (!n || *n > count) return std::nullopt;
            const uint64_t refs = high == 0xD ? *n * 2 : *n;
            if (!fits(refs * refSize)) return std::nullopt;
            auto ref = [&](uint64_t k) { return size_t(bigEndian(data + p + k * refSize, refSize)); };
            for (uint64_t k = 0; k < refs; k++) if (ref(k) >= count) return std::nullopt;
            if (high == 0xD) { v.kind = Value::Kind::Dict; for (uint64_t k = 0; k < *n; k++) v.pairs.emplace_back(ref(k), ref(*n + k)); }
            else { v.kind = Value::Kind::Array; for (uint64_t k = 0; k < *n; k++) v.items.push_back(ref(k)); }
            break;
        }
        default: break;   // dates and anything else: left as Null, which no brush setting needs
        }
    }
    return out;
}

std::optional<std::map<std::string, Value>> keyedRoot(const Binary& archive) {
    const Value& top = archive.objects[archive.top];
    if (top.kind != Value::Kind::Dict) return std::nullopt;
    auto find = [&](const Value& dict, const std::string& key) -> const Value* {
        for (const auto& [k, v] : dict.pairs) if (archive.objects[k].kind == Value::Kind::String && archive.objects[k].text == key) return &archive.objects[v];
        return nullptr;
    };
    const Value* objectsList = find(top, "$objects");
    const Value* topDict = find(top, "$top");
    const Value* rootUid = topDict ? find(*topDict, "root") : nullptr;
    if (!objectsList || objectsList->kind != Value::Kind::Array || !rootUid || rootUid->kind != Value::Kind::Uid) return std::nullopt;
    auto object = [&](uint64_t uid) -> const Value* { return uid < objectsList->items.size() ? &archive.objects[objectsList->items[size_t(uid)]] : nullptr; };
    const Value* root = object(rootUid->uid);
    if (!root || root->kind != Value::Kind::Dict) return std::nullopt;
    std::map<std::string, Value> out;
    for (const auto& [k, v] : root->pairs) {
        const Value& key = archive.objects[k];
        if (key.kind != Value::Kind::String) continue;
        const Value* value = &archive.objects[v];
        if (value->kind == Value::Kind::Uid) value = object(value->uid);
        if (value) out[key.text] = *value;
    }
    return out;
}

std::optional<XmlDict> parseXmlDict(const std::string& xml) {
    // The top <dict>: <key>k</key> then <string>, or <array> of <string>s; anything else is skipped.
    XmlDict out;
    size_t p = xml.find("<dict>");
    if (p == std::string::npos) return std::nullopt;
    auto unescape = [](std::string s) {
        for (auto [from, to] : {std::pair{"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}})
            for (size_t at; (at = s.find(from)) != std::string::npos;) s.replace(at, std::strlen(from), to);
        return s;
    };
    auto between = [&](const std::string& open, const std::string& close, size_t from, size_t& end) -> std::optional<std::string> {
        const size_t a = xml.find(open, from);
        if (a == std::string::npos) return std::nullopt;
        const size_t b = xml.find(close, a + open.size());
        if (b == std::string::npos) return std::nullopt;
        end = b + close.size();
        return unescape(xml.substr(a + open.size(), b - a - open.size()));
    };
    while (true) {
        size_t afterKey = 0;
        auto key = between("<key>", "</key>", p, afterKey);
        if (!key) break;
        const size_t next = xml.find_first_not_of(" \t\r\n", afterKey);
        if (next == std::string::npos) break;
        size_t end = afterKey;
        if (xml.compare(next, 8, "<string>") == 0) {
            if (auto value = between("<string>", "</string>", next, end)) out.strings[*key] = *value;
        } else if (xml.compare(next, 7, "<array>") == 0) {
            const size_t close = xml.find("</array>", next);
            if (close == std::string::npos) break;
            std::vector<std::string> items;
            for (size_t q = next; ;) {
                size_t itemEnd = 0;
                auto item = between("<string>", "</string>", q, itemEnd);
                if (!item || itemEnd > close) break;
                items.push_back(*item);
                q = itemEnd;
            }
            out.arrays[*key] = std::move(items);
            end = close + 8;
        }
        p = std::max(end, afterKey);
    }
    return out;
}

} // namespace compositor::plist
