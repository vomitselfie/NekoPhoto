// A very small test harness: no dependencies, one executable per test file,
// registered with CTest. Failures print file:line and the expression.
#pragma once
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <type_traits>
#include <string>
#include <vector>

namespace check {

struct Registry {
    struct Case { const char* name; std::function<void()> body; };
    std::vector<Case> cases;
    int failures = 0;
    static Registry& get() { static Registry r; return r; }
};

struct Adder {
    Adder(const char* name, std::function<void()> body) { Registry::get().cases.push_back({name, std::move(body)}); }
};

inline void fail(const char* file, int line, const std::string& what) {
    std::fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, what.c_str());
    Registry::get().failures++;
}

template <typename T> std::string str(const T& v) { if constexpr (std::is_convertible_v<T, std::string>) return std::string(v); else return std::to_string(v); }

inline int run(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int ran = 0;
    for (auto& c : Registry::get().cases) {
        if (filter && std::string(c.name).find(filter) == std::string::npos) continue;
        int before = Registry::get().failures;
        std::fprintf(stderr, "[ %s ]\n", c.name);
        c.body();
        ran++;
        if (Registry::get().failures != before) std::fprintf(stderr, "  -> failed\n");
    }
    std::fprintf(stderr, "%d test(s), %d failure(s)\n", ran, Registry::get().failures);
    return Registry::get().failures == 0 ? 0 : 1;
}

} // namespace check

#define TEST_CASE(name) \
    static void name##_body(); \
    static check::Adder name##_adder(#name, name##_body); \
    static void name##_body()

#define CHECK(expr) do { if (!(expr)) check::fail(__FILE__, __LINE__, #expr); } while (0)
#define CHECK_EQ(a, b) do { auto va_ = (a); auto vb_ = (b); if (!(va_ == vb_)) check::fail(__FILE__, __LINE__, std::string(#a " == " #b) + " (" + check::str(va_) + " vs " + check::str(vb_) + ")"); } while (0)
#define CHECK_NEAR(a, b, eps) do { double va_ = (a); double vb_ = (b); if (std::fabs(va_ - vb_) > (eps)) check::fail(__FILE__, __LINE__, std::string(#a " ~= " #b) + " (" + std::to_string(va_) + " vs " + std::to_string(vb_) + ")"); } while (0)
#define REQUIRE(expr) do { if (!(expr)) { check::fail(__FILE__, __LINE__, #expr); return; } } while (0)

#define TEST_MAIN() int main(int argc, char** argv) { return check::run(argc, argv); }
