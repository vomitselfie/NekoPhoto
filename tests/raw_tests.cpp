// Camera RAW: which paths count as RAW, and a file that is not one is refused, not crashed on.
#include "check.h"
#include "compositor/raw.h"
#include <cstdio>
#include <filesystem>

using namespace compositor;

TEST_CASE(raw_extensions_are_recognised) {
    CHECK(isRawPath("/photos/IMG_0001.CR2"));
    CHECK(isRawPath("a.nef"));
    CHECK(isRawPath("b.Dng"));
    CHECK(!isRawPath("c.png"));
    CHECK(!isRawPath("no-extension"));
}

TEST_CASE(a_file_that_is_not_raw_is_refused) {
    const auto path = std::filesystem::temp_directory_path() / "nekophoto_not_raw.cr2";
    if (FILE* f = std::fopen(path.string().c_str(), "wb")) { std::fputs("Poser character, not a Canon RAW", f); std::fclose(f); }
    std::string error;
    CHECK(decodeRaw(path.string(), &error) == nullptr);
    CHECK(!error.empty());
    std::filesystem::remove(path);
}

TEST_MAIN()
