// Ported from Patchy (MIT, src/third_party/patchy_psd/README.md): src/formats/af_tree.cpp.
#include "affinity_tree.h"
#include <cstring>
#include <utility>

namespace compositor::affinity {

namespace {

constexpr uint32_t docTag = 0x534BFF00u;   // doc.dat stream header

// Hostile-input caps. Real documents: hundreds to low thousands of classes, nesting a few dozen deep, arrays up
// to a few thousand tiles.
constexpr size_t maxClasses = 2'000'000;
constexpr int maxDepth = 512;
constexpr uint32_t maxArray = 1u << 24;
constexpr uint32_t maxString = 1u << 24;

[[noreturn]] void bad(const char* what) { throw std::runtime_error(what); }

class TreeReader {
public:
    TreeReader(std::span<const uint8_t> bytes, std::map<uint32_t, std::shared_ptr<Class>>& shared)
        : reader_(bytes, "Affinity document tree is truncated"), bytes_(bytes), shared_(shared) {}

    Tree parse() {
        Tree tree;
        if (reader_.u32() != docTag) bad("Affinity document tree has a bad header");
        const uint16_t fileVersion = reader_.u16();
        tree.rootType = reader_.u32();
        (void)reader_.u16();   // tag version
        if (fileVersion >= 2) tree.documentVersion = reader_.u32();
        tree.root = std::make_shared<Class>();
        tree.root->type = tree.rootType;
        readFields(*tree.root, true, 0);
        return tree;
    }

private:
    void countClass() { if (++classes_ > maxClasses) bad("Affinity document tree is implausibly large"); }

    void readFields(Class& parent, bool withTag, int depth) {
        if (depth > maxDepth) bad("Affinity document tree nests too deeply");
        for (;;) {
            uint8_t type = reader_.u8();
            const bool array = (type & 0x80u) != 0;
            type &= 0x7Fu;
            if (type == 0) return;
            if (type > 0x77) bad("Affinity document tree has an unknown field type");
            Field field;
            if (withTag) field.tag = reader_.u32();
            field.value = readValue(type, array, depth);
            parent.fields.push_back(std::move(field));
        }
    }

    Value readValue(uint8_t type, bool array, int depth) {
        switch (type) {
        case 0x01: return scalar<uint8_t>(array);
        case 0x02: return scalar<uint16_t>(array);
        case 0x03: return scalar<uint32_t>(array);
        case 0x04: return scalar<uint64_t>(array);
        case 0x05: return scalar<int8_t>(array);
        case 0x06: return scalar<int16_t>(array);
        case 0x07: return scalar<int32_t>(array);
        case 0x08: return scalar<int64_t>(array);
        case 0x09: return real<float>(array);
        case 0x0A: return real<double>(array);
        case 0x2F: case 0x34: return scalar<uint32_t>(array);
        case 0x29: return boolean(array);
        case 0x2A: return enumeration(array);
        case 0x2B: case 0x2E: return string(array);
        case 0x2C: return curve(array);
        case 0x2D: return binary(array);
        case 0x30: case 0x31: case 0x32: return classField(type, array, depth);
        case 0x33: return embedded(array);
        case 0x75: return flags(array);
        default: break;
        }
        if (type >= 0x15 && type <= 0x19) return intVector(type - 0x15 + 2, array);
        if (type >= 0x1F && type <= 0x23) return floatVector(type - 0x1F + 2, 4, array);
        if (type >= 0x24 && type <= 0x28) return floatVector(type - 0x24 + 2, 8, array);
        if (type >= 0x35 && type <= 0x74) return sizedStruct(size_t(type) - 0x34, array);
        bad("Affinity document tree has an unhandled field type");
    }

    template <typename T> T readInt() {
        if constexpr (sizeof(T) == 1) return static_cast<T>(reader_.u8());
        else if constexpr (sizeof(T) == 2) return static_cast<T>(reader_.u16());
        else if constexpr (sizeof(T) == 4) return static_cast<T>(reader_.u32());
        else return static_cast<T>(reader_.u64());
    }
    template <typename T> T readFloat() {
        if constexpr (sizeof(T) == 4) { const uint32_t bits = reader_.u32(); float v; std::memcpy(&v, &bits, 4); return v; }
        else { const uint64_t bits = reader_.u64(); double v; std::memcpy(&v, &bits, 8); return v; }
    }

    template <typename T> Value scalar(bool array) {
        if (!array) return int64_t(readInt<T>());
        const uint32_t count = readCount();
        std::vector<int64_t> values(count);
        for (auto& v : values) v = int64_t(readInt<T>());
        return values;
    }
    template <typename T> Value real(bool array) {
        if (!array) return double(readFloat<T>());
        const uint32_t count = readCount();
        std::vector<double> values(count);
        for (auto& v : values) v = double(readFloat<T>());
        return values;
    }
    Value boolean(bool array) {
        if (!array) return reader_.u8() != 0;
        const uint32_t count = readCount();
        std::vector<int64_t> values(count);
        uint32_t index = 0;
        for (uint32_t b = 0; b < count / 8; b++) {
            const uint8_t byte = reader_.u8();
            for (int bit = 0; bit < 8; bit++) values[index++] = (byte >> bit) & 1;
        }
        if (count % 8) {
            const uint8_t byte = reader_.u8();
            for (uint32_t bit = 0; bit < count % 8; bit++) values[index++] = (byte >> bit) & 1;
        }
        return values;
    }
    Value enumeration(bool array) {
        if (!array) { Enum e; e.id = reader_.u16(); e.version = reader_.u16(); return e; }
        const uint32_t count = readCount();
        (void)reader_.u16();   // shared version
        std::vector<int64_t> ids(count);
        for (auto& id : ids) id = reader_.u16();
        return ids;
    }
    Value string(bool array) {
        if (!array) return oneString();
        (void)reader_.u32();   // total size
        const uint32_t count = readCount();
        for (uint32_t i = 0; i < count; i++) (void)oneString();   // string arrays are consumed, not kept
        return Skipped{};
    }
    Value curve(bool array) {
        const uint32_t count = array ? readCount() : 1;
        const uint16_t size = reader_.u16();
        const uint64_t total = uint64_t(count) * size;
        if (total > 0 && total <= (1u << 22)) {
            CurveArray value;
            value.recordSize = size;
            value.bytes.resize(size_t(total));
            for (auto& byte : value.bytes) byte = reader_.u8();
            return value;
        }
        for (uint32_t i = 0; i < count; i++) reader_.skip(size);
        return Skipped{};
    }
    Value binary(bool array) {
        if (array) bad("Affinity document tree has an invalid binary array");
        const uint32_t size = readLength();
        std::vector<uint8_t> data(size);
        for (auto& byte : data) byte = reader_.u8();
        return data;
    }
    Value embedded(bool array) {
        if (array) bad("Affinity document tree has an invalid embedded array");
        Embedded value;
        value.tag = reader_.u32();
        const uint32_t size = readLength();
        value.data.resize(size);
        for (auto& c : value.data) c = char(reader_.u8());
        return value;
    }
    Value flags(bool array) {
        if (array) bad("Affinity document tree has an invalid flags array");
        (void)reader_.u16();   // version
        const uint8_t count = reader_.u8();
        if (count > 8) bad("Affinity document tree has an invalid flags count");
        uint64_t bits = 0;
        for (uint8_t i = 0; i < count; i++) bits |= uint64_t(reader_.u8()) << (8 * i);
        return int64_t(bits);
    }
    Value intVector(int components, bool array) {
        if (!array) {
            std::vector<int64_t> values(static_cast<size_t>(components));
            for (auto& v : values) v = readInt<int32_t>();
            return values;
        }
        const uint32_t count = readCount();
        for (uint32_t i = 0; i < count; i++) for (int c = 0; c < components; c++) (void)readInt<int32_t>();
        return Skipped{};
    }
    Value floatVector(int components, int elementSize, bool array) {
        const uint32_t count = array ? readCount() : 1;
        // Small arrays flatten into one vector (gradient stops, float2 pairs); implausibly large ones are skipped.
        const uint64_t total = uint64_t(count) * uint64_t(components);
        if (!array || total <= 4096) {
            std::vector<double> values(static_cast<size_t>(total));
            for (auto& v : values) v = elementSize == 4 ? double(readFloat<float>()) : readFloat<double>();
            return values;
        }
        reader_.skip(size_t(total) * size_t(elementSize));
        return Skipped{};
    }
    Value sizedStruct(size_t size, bool array) {
        if (!array && size >= 8 && size <= 64) {
            // Small structs (colours such as the spread background's RGBA float quad) keep their bytes.
            std::vector<uint8_t> data(size);
            for (auto& byte : data) byte = reader_.u8();
            return data;
        }
        const uint32_t count = array ? readCount() : 1;
        for (uint32_t i = 0; i < count; i++) reader_.skip(size);
        return Skipped{};
    }
    Value classField(uint8_t type, bool array, int depth) {
        uint32_t count = 1, arrayTag = 0;
        bool header = false;
        if (array) {
            count = readCount();
            if (type == 0x32) { arrayTag = reader_.u32(); (void)reader_.u16(); header = true; }
        }
        std::vector<std::shared_ptr<Class>> classes;
        classes.reserve(count <= 4096 ? count : 0);
        for (uint32_t i = 0; i < count; i++) classes.push_back(readClass(type, depth + 1, header, arrayTag));
        if (!array) return classes.empty() ? std::shared_ptr<Class>() : classes.front();
        return classes;
    }
    std::shared_ptr<Class> readClass(uint8_t type, int depth, bool header, uint32_t arrayTag) {
        if (depth > maxDepth) bad("Affinity document tree nests too deeply");
        if (type == 0x30) {
            countClass();
            auto cls = std::make_shared<Class>();
            readFields(*cls, false, depth);
            return cls;
        }
        const uint8_t flag = reader_.u8();
        if (flag == 0) return nullptr;
        if (type == 0x31) {
            if (flag == 2) {
                auto found = shared_.find(reader_.u32());
                return found != shared_.end() ? found->second : nullptr;
            }
            if (flag != 1) bad("Affinity document tree has an invalid shared class");
            countClass();
            auto cls = std::make_shared<Class>();
            cls->sharedId = reader_.u32();
            shared_[cls->sharedId] = cls;
            bool first = true;
            for (;;) {
                const uint8_t typeFlag = reader_.u8();
                if (typeFlag == 1) { const uint32_t t = reader_.u32(); if (first) cls->type = t; break; }
                if (typeFlag == 2) break;
                if (typeFlag == 0) {
                    const uint32_t t = reader_.u32();
                    (void)reader_.u16();   // ancestor version
                    if (first) { cls->type = t; first = false; }
                    Class ancestor;   // ancestor field lists, usually empty
                    readFields(ancestor, true, depth);
                    continue;
                }
                bad("Affinity document tree has an invalid class type flag");
            }
            readFields(*cls, true, depth);
            return cls;
        }
        if (flag != 1) bad("Affinity document tree has an invalid class");
        countClass();
        auto cls = std::make_shared<Class>();
        if (header) cls->type = arrayTag;
        else { cls->type = reader_.u32(); (void)reader_.u16(); }
        readFields(*cls, true, depth);
        return cls;
    }

    uint32_t readCount() { const uint32_t n = reader_.u32(); if (n > maxArray) bad("Affinity document tree has an implausible array"); return n; }
    uint32_t readLength() { const uint32_t n = reader_.u32(); if (n > bytes_.size()) bad("Affinity document tree has an implausible length"); return n; }
    std::string oneString() {
        const uint32_t length = reader_.u32();
        if (length > maxString || length > reader_.remaining()) bad("Affinity document tree has an implausible string");
        std::string value(length, '\0');
        for (auto& c : value) c = char(reader_.u8());
        return value;
    }

    Reader reader_;
    std::span<const uint8_t> bytes_;
    std::map<uint32_t, std::shared_ptr<Class>>& shared_;
    size_t classes_ = 0;
};

} // namespace

const Field* Class::field(uint32_t t) const {
    for (const auto& f : fields) if (f.tag == t) return &f;
    return nullptr;
}

const Class* Class::child(uint32_t t) const {
    const Field* f = field(t);
    if (!f) return nullptr;
    if (const auto* p = std::get_if<std::shared_ptr<Class>>(&f->value)) return p->get();
    return nullptr;
}

const std::vector<std::shared_ptr<Class>>* Class::list(uint32_t t) const {
    const Field* f = field(t);
    return f ? std::get_if<std::vector<std::shared_ptr<Class>>>(&f->value) : nullptr;
}

bool Class::boolean(uint32_t t, bool fallback) const {
    const Field* f = field(t);
    if (!f) return fallback;
    if (const auto* v = std::get_if<bool>(&f->value)) return *v;
    if (const auto* v = std::get_if<int64_t>(&f->value)) return *v != 0;
    return fallback;
}

double Class::number(uint32_t t, double fallback) const {
    const Field* f = field(t);
    if (!f) return fallback;
    if (const auto* v = std::get_if<double>(&f->value)) return *v;
    if (const auto* v = std::get_if<int64_t>(&f->value)) return double(*v);
    return fallback;
}

int64_t Class::integer(uint32_t t, int64_t fallback) const {
    const Field* f = field(t);
    if (!f) return fallback;
    if (const auto* v = std::get_if<int64_t>(&f->value)) return *v;
    if (const auto* v = std::get_if<double>(&f->value)) return int64_t(*v);
    return fallback;
}

std::string Class::string(uint32_t t) const {
    const Field* f = field(t);
    if (!f) return {};
    if (const auto* v = std::get_if<std::string>(&f->value)) return *v;
    return {};
}

std::vector<double> Class::vector(uint32_t t) const {
    const Field* f = field(t);
    if (!f) return {};
    if (const auto* v = std::get_if<std::vector<double>>(&f->value)) return *v;
    if (const auto* v = std::get_if<std::vector<int64_t>>(&f->value)) return std::vector<double>(v->begin(), v->end());
    return {};
}

Tree parseTree(std::span<const uint8_t> bytes) {
    std::map<uint32_t, std::shared_ptr<Class>> shared;
    return TreeReader(bytes, shared).parse();
}

} // namespace compositor::affinity
