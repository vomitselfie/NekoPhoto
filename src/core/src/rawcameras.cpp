// Camera Raw's exposure offset for a RAW file that does not carry one (raw.h's cameraExposureOffset).
//
// The bodies are RAWmakase's (https://github.com/pch/rawmakase, MIT, LICENSES/RAWmakase-MIT.txt) data/cameras.toml at
// commit 7c19ab8d78fc91ebc18dd9514b4f83e405c27953: the rows its `source` marks `fitted`, each fitted there to Camera Raw
// 18.7's renders of unedited photos from that body (raw.pixls.us samples and RAWmakase's own; the median midtone
// offset). The table's other rows, read from DNGs that Adobe's software wrote (`adobe-dng`) or from Adobe's profiles
// (`adobe-profile`), are Adobe's own data and are left out, so the make and overall medians below are of the fitted
// rows alone. The Fujifilm rule (rows at DR100, the maker notes' exposure midpoint shift added, the X-Trans III bodies
// that ignore it) is RAWmakase's observation of Camera Raw's renders (its docs/cameras.md).
#include "nekophoto/raw.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <vector>

namespace nekophoto {

namespace {

struct CameraRow {
    const char* make;
    const char* model;
    const char* alias;   // another name for the body, or empty
    double ev;
};

// Fitted to Camera Raw 18.7 (RAWmakase, checked 2026-09-26 to 2026-10-05). Fujifilm rows hold at DR100 and base ISO.
constexpr CameraRow kCameras[] = {
    {"Canon", "PowerShot G5 X Mark II", "", 0.05},
    {"Fujifilm", "X-H2", "", -0.35},
    {"Fujifilm", "X-H2S", "", -0.05},
    {"Fujifilm", "X-S10", "", -0.1},
    {"Fujifilm", "X-S20", "", -0.05},
    {"Fujifilm", "X-T30 II", "", -0.2},
    {"Fujifilm", "X-T5", "", -0.1},
    {"Fujifilm", "X100VI", "", -0.15},
    {"Nikon", "D500", "", 0.2},
    {"Nikon", "D5600", "", 0.1},
    {"Nikon", "D850", "", 0.2},
    {"Nikon", "Z 7", "", 0.25},
    {"Olympus", "PEN-F", "", 0.55},
    {"Pentax", "K-70", "", 0.35},
    {"Sony", "DSC-RX100M7", "", -0.4},
    {"Sony", "ILCE-7CR", "A7CR", 0.3},
    {"Sony", "ILCE-7M5", "A7 V", 0.5},
};

// Fujifilm bodies whose Camera Raw value stays the same whatever the exposure midpoint shift (the X-Trans III bodies).
constexpr std::array<const char*, 6> kFujifilmShiftIgnored{"X-T2", "X-E3", "X-Pro2", "X-T20", "X100F", "X-H1"};

/// Lower case without spaces, hyphens and underscores: "ILCE-7M5" and "ilce 7m5" match.
std::string key(const std::string& name) {
    std::string out;
    for (char c : name)
        if (c != ' ' && c != '-' && c != '_') out += char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const size_t n = values.size();
    return n == 0 ? 0 : n % 2 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2;
}

} // namespace

CameraExposureOffset cameraExposureOffset(const std::string& make, const std::string& model, double fujifilmShift, bool xTrans) {
    const std::string makeKey = key(make), modelKey = key(model);
    CameraExposureOffset out;
    std::vector<double> sameMake, all;
    bool found = false;
    for (const CameraRow& row : kCameras) {
        all.push_back(row.ev);
        if (key(row.make) != makeKey) continue;
        sameMake.push_back(row.ev);
        if (!found && (key(row.model) == modelKey || (*row.alias && key(row.alias) == modelKey))) {
            out.ev = row.ev;
            out.from = CameraExposureOffset::From::Body;
            found = true;
        }
    }
    if (!found) {
        out.from = sameMake.empty() ? CameraExposureOffset::From::AllBodies : CameraExposureOffset::From::Make;
        out.ev = median(sameMake.empty() ? all : sameMake);
    }
    // Fujifilm: Camera Raw's value is the body's at DR100 less the exposure midpoint shift the maker notes record beyond
    // the sensor's usual one at DR100 (about -0.72 EV on X-Trans, -0.49 on GFX, 0 on the small Bayer bodies), so DR200
    // and DR400 develop a stop and two brighter. An unknown shift (LibRaw's -999) leaves the DR100 value.
    if (makeKey == "fujifilm" && std::isfinite(fujifilmShift) && std::fabs(fujifilmShift) < 10) {
        const bool ignored = std::any_of(kFujifilmShiftIgnored.begin(), kFujifilmShiftIgnored.end(), [&](const char* m) { return key(m) == modelKey; });
        const double usual = modelKey.rfind("gfx", 0) == 0 ? -0.49 : xTrans ? -0.72 : 0.0;
        if (!ignored) out.ev += usual - fujifilmShift;
    }
    return out;
}

} // namespace nekophoto
