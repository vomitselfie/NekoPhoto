// Apple property lists, as far as brush files need them: the binary format (bplist00, as described in Apple's
// open-source CoreFoundation, CFBinaryPList.c) with NSKeyedArchiver's object graph resolved, and the strings,
// arrays and dictionaries of an XML plist. Internal to the core.
#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace compositor::plist {

struct Value {
    enum class Kind { Null, Bool, Integer, Real, String, Data, Uid, Array, Dict };
    Kind kind = Kind::Null;
    double number = 0;   // Bool, Integer, Real
    uint64_t uid = 0;
    std::string text;    // String, as UTF-8
    std::vector<uint8_t> data;
    std::vector<size_t> items;                   // Array: object indices
    std::vector<std::pair<size_t, size_t>> pairs; // Dict: key and value object indices
};

/// A binary plist's objects and its top object; nullopt when the bytes are not one.
struct Binary {
    std::vector<Value> objects;
    size_t top = 0;
    static std::optional<Binary> parse(const uint8_t* data, size_t size);
};

/// An NSKeyedArchiver archive's root object as a key to value map, each value followed through its UID into
/// the archive's objects (one level).
std::optional<std::map<std::string, Value>> keyedRoot(const Binary& archive);

/// An XML plist's top dictionary, keeping only string values and arrays of strings.
struct XmlDict { std::map<std::string, std::string> strings; std::map<std::string, std::vector<std::string>> arrays; };
std::optional<XmlDict> parseXmlDict(const std::string& xml);

} // namespace compositor::plist
