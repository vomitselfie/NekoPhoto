#include "compositor/uuid.h"
#include <cctype>
#include <cstdio>
#include <random>

namespace compositor {

Uuid makeUuid() {
    static std::mt19937_64 engine{std::random_device{}()};
    uint64_t hi = engine(), lo = engine();
    // Version 4, variant 1.
    hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "%08llX-%04llX-%04llX-%04llX-%012llX",
                  (unsigned long long)(hi >> 32), (unsigned long long)((hi >> 16) & 0xFFFF), (unsigned long long)(hi & 0xFFFF),
                  (unsigned long long)(lo >> 48), (unsigned long long)(lo & 0xFFFFFFFFFFFFULL));
    return buffer;
}

bool parseUuid(const std::string& text, Uuid& out) {
    if (text.size() != 36) return false;
    std::string result;
    result.reserve(36);
    for (size_t i = 0; i < 36; i++) {
        char c = text[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return false; result.push_back('-'); continue; }
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
        result.push_back(char(std::toupper(static_cast<unsigned char>(c))));
    }
    out = result;
    return true;
}

} // namespace compositor
