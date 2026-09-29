#include "compositor/whitebalance.h"
#include <algorithm>
#include <cmath>

namespace compositor {

namespace {

/// Robertson's isotemperature lines (Wyszecki & Stiles, Color Science, 2nd ed., table 1(3.11)): reciprocal
/// temperature in mired, the Planckian locus point (u, v) in CIE 1960 UCS, and the slope t of the isotemperature
/// line through it (v - v_i = t (u - u_i)).
struct Isotemperature { double mired, u, v, t; };
constexpr Isotemperature kLines[] = {
    {0, 0.18006, 0.26352, -0.24341},   {10, 0.18066, 0.26589, -0.25479},  {20, 0.18133, 0.26846, -0.26876},
    {30, 0.18208, 0.27119, -0.28539},  {40, 0.18293, 0.27407, -0.30470},  {50, 0.18388, 0.27709, -0.32675},
    {60, 0.18494, 0.28021, -0.35156},  {70, 0.18611, 0.28342, -0.37915},  {80, 0.18740, 0.28668, -0.40955},
    {90, 0.18880, 0.28997, -0.44278},  {100, 0.19032, 0.29326, -0.47888}, {125, 0.19462, 0.30141, -0.58204},
    {150, 0.19962, 0.30921, -0.70471}, {175, 0.20525, 0.31647, -0.84901}, {200, 0.21142, 0.32312, -1.0182},
    {225, 0.21807, 0.32909, -1.2168},  {250, 0.22511, 0.33439, -1.4512},  {275, 0.23247, 0.33904, -1.7298},
    {300, 0.24010, 0.34308, -2.0637},  {325, 0.24792, 0.34655, -2.4681},  {350, 0.25591, 0.34951, -2.9641},
    {375, 0.26400, 0.35200, -3.5814},  {400, 0.27218, 0.35407, -4.3633},  {425, 0.28039, 0.35577, -5.3762},
    {450, 0.28863, 0.35714, -6.7262},  {475, 0.29685, 0.35823, -8.5955},  {500, 0.30505, 0.35907, -11.324},
    {525, 0.31320, 0.35968, -15.628},  {550, 0.32129, 0.36011, -23.325},  {575, 0.32931, 0.36038, -40.770},
    {600, 0.33724, 0.36051, -116.45},
};
constexpr int kLineCount = int(sizeof(kLines) / sizeof(kLines[0]));
/// The hottest temperature reported: the table's 0 mired end is infinitely hot.
constexpr double kMinMired = 1;

struct Vec { double u, v; };

/// Unit vector along line i, pointing to the magenta side of the locus (increasing u; every slope is negative).
Vec direction(int i) {
    const double n = std::sqrt(1 + kLines[i].t * kLines[i].t);
    return {1 / n, kLines[i].t / n};
}

/// The locus point and the isotemperature direction a fraction `f` of the way from line i to line i + 1.
void lineAt(int i, double f, Vec& point, Vec& dir) {
    const Isotemperature& a = kLines[i];
    const Isotemperature& b = kLines[i + 1];
    point = {a.u + f * (b.u - a.u), a.v + f * (b.v - a.v)};
    const Vec da = direction(i), db = direction(i + 1);
    dir = {da.u + f * (db.u - da.u), da.v + f * (db.v - da.v)};
    const double n = std::hypot(dir.u, dir.v);
    dir = {dir.u / n, dir.v / n};
}

/// Signed distance of (u, v) from line i (Robertson's d_i): positive before it (the hotter side).
double distance(int i, double u, double v) {
    const Isotemperature& l = kLines[i];
    return ((v - l.v) - l.t * (u - l.u)) / std::sqrt(1 + l.t * l.t);
}

Vec uvFromXy(Chromaticity xy) {
    const double d = -2 * xy.x + 12 * xy.y + 3;
    return {4 * xy.x / d, 6 * xy.y / d};
}

Chromaticity xyFromUv(Vec uv) {
    const double d = 2 * uv.u - 8 * uv.v + 4;
    return {3 * uv.u / d, 2 * uv.v / d};
}

} // namespace

TemperatureTint temperatureTintFromXy(Chromaticity xy) {
    const Vec q = uvFromXy(xy);
    // Robertson: the pair of adjacent lines whose distances change sign brackets the white point.
    int segment = -1;
    double previous = distance(0, q.u, q.v);
    for (int i = 1; i < kLineCount; i++) {
        const double d = distance(i, q.u, q.v);
        if ((previous >= 0) != (d >= 0) || d == 0) { segment = i - 1; break; }
        previous = d;
    }
    double f = 0;
    if (segment < 0) {
        // Beyond the table: the end it lies past.
        segment = previous < 0 ? 0 : kLineCount - 2;
        f = previous < 0 ? 0 : 1;
    } else {
        // The fraction whose interpolated line passes through the white point: the cross product of (q - locus) with
        // the line's direction changes sign across the segment (it is -d_i at one end, -d_{i+1} at the other).
        auto cross = [&](double t) {
            Vec p, dir;
            lineAt(segment, t, p, dir);
            return (q.u - p.u) * dir.v - (q.v - p.v) * dir.u;
        };
        double lo = 0, hi = 1;
        const bool rising = cross(0) < cross(1);
        for (int k = 0; k < 64; k++) {
            const double mid = 0.5 * (lo + hi);
            if ((cross(mid) < 0) == rising) lo = mid; else hi = mid;
        }
        f = 0.5 * (lo + hi);
    }
    Vec p, dir;
    lineAt(segment, f, p, dir);
    const double mired = std::max(kMinMired, kLines[segment].mired + f * (kLines[segment + 1].mired - kLines[segment].mired));
    const double along = (q.u - p.u) * dir.u + (q.v - p.v) * dir.v;   // toward magenta
    return {1e6 / mired, -along * kTintScale};
}

Chromaticity xyFromTemperatureTint(double temperature, double tint) {
    const double mired = std::clamp(1e6 / std::max(temperature, 1.0), kMinMired, kLines[kLineCount - 1].mired);
    int segment = 0;
    while (segment < kLineCount - 2 && mired > kLines[segment + 1].mired) segment++;
    const double f = (mired - kLines[segment].mired) / (kLines[segment + 1].mired - kLines[segment].mired);
    Vec p, dir;
    lineAt(segment, f, p, dir);
    const double along = -tint / kTintScale;
    return xyFromUv({p.u + along * dir.u, p.v + along * dir.v});
}

double lightSourceTemperature(int code) {
    // EXIF LightSource / DNG CalibrationIlluminant codes and the temperatures the DNG specification assigns them.
    switch (code) {
    case 1: case 4: case 9: return 5500;   // daylight, flash, fine weather
    case 2: case 14: return 4150;          // fluorescent, cool white fluorescent
    case 3: return 2850;                   // tungsten
    case 10: return 6500;                  // cloudy
    case 11: return 7500;                  // shade
    case 12: return 6430;                  // daylight fluorescent
    case 13: return 5000;                  // day white fluorescent
    case 15: return 3450;                  // white fluorescent
    case 16: return 2940;                  // warm white fluorescent
    case 17: return 2856;                  // standard light A
    case 18: return 4874;                  // standard light B
    case 19: return 6774;                  // standard light C
    case 20: return 5503;                  // D55
    case 21: return 6504;                  // D65
    case 22: return 7504;                  // D75
    case 23: return 5003;                  // D50
    case 24: return 3200;                  // ISO studio tungsten
    default: return 0;
    }
}

std::optional<Matrix3> invert(const Matrix3& m) {
    const double c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1];
    const double c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2];
    const double c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
    const double det = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
    if (!(std::fabs(det) > 1e-12) || !std::isfinite(det)) return std::nullopt;
    Matrix3 r;
    r[0] = {c00 / det, (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det, (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det};
    r[1] = {c01 / det, (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det, (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det};
    r[2] = {c02 / det, (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det, (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det};
    return r;
}

namespace {

std::array<double, 3> apply(const Matrix3& m, const std::array<double, 3>& v) {
    return {m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2], m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
            m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2]};
}

} // namespace

CameraWhiteModel CameraWhiteModel::single(const Matrix3& xyzToCamera) {
    CameraWhiteModel model;
    model.xyzToCamera1 = xyzToCamera;
    model.valid = invert(xyzToCamera).has_value();
    return model;
}

CameraWhiteModel CameraWhiteModel::dual(const Matrix3& m1, double t1, const Matrix3& m2, double t2) {
    if (!(t1 > 0 && t2 > 0 && t1 != t2)) return single(t2 > 0 ? m2 : m1);
    CameraWhiteModel model;
    model.xyzToCamera1 = m1;
    model.temperature1 = t1;
    model.xyzToCamera2 = m2;
    model.temperature2 = t2;
    model.valid = invert(m1).has_value() && invert(m2).has_value();
    return model;
}

Matrix3 CameraWhiteModel::matrixAt(double t) const {
    if (!isDual()) return xyzToCamera1;
    // The DNG specification's interpolation: linear in reciprocal temperature, clamped to the two illuminants.
    const double w = std::clamp((1 / t - 1 / temperature2) / (1 / temperature1 - 1 / temperature2), 0.0, 1.0);
    Matrix3 m;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) m[r][c] = w * xyzToCamera1[r][c] + (1 - w) * xyzToCamera2[r][c];
    return m;
}

std::optional<Chromaticity> CameraWhiteModel::xyFromNeutral(const std::array<double, 3>& neutral) const {
    if (!valid) return std::nullopt;
    auto solve = [&](double t) -> std::optional<Chromaticity> {
        const auto inverse = invert(matrixAt(t));
        if (!inverse) return std::nullopt;
        const auto xyz = apply(*inverse, neutral);
        const double sum = xyz[0] + xyz[1] + xyz[2];
        if (!(sum > 0) || !(xyz[1] > 0) || !std::isfinite(sum)) return std::nullopt;
        const Chromaticity xy{xyz[0] / sum, xyz[1] / sum};
        if (!(xy.x > 0 && xy.y > 0 && xy.x + xy.y < 1)) return std::nullopt;
        return xy;
    };
    if (!isDual()) return solve(0);
    // The matrix depends on the temperature it is asked to find: iterate until the two agree.
    double t = std::sqrt(temperature1 * temperature2);
    std::optional<Chromaticity> xy;
    for (int k = 0; k < 64; k++) {
        xy = solve(t);
        if (!xy) return std::nullopt;
        const double next = temperatureTintFromXy(*xy).temperature;
        if (std::fabs(1 / next - 1 / t) < 1e-13) break;
        t = next;
    }
    return xy;
}

std::optional<std::array<double, 3>> CameraWhiteModel::neutralFromXy(Chromaticity xy) const {
    if (!valid || !(xy.y > 0)) return std::nullopt;
    const std::array<double, 3> xyz{xy.x / xy.y, 1, (1 - xy.x - xy.y) / xy.y};
    const auto n = apply(matrixAt(temperatureTintFromXy(xy).temperature), xyz);
    if (!(n[0] > 0 && n[1] > 0 && n[2] > 0) || !std::isfinite(n[0] + n[1] + n[2])) return std::nullopt;
    return std::array<double, 3>{n[0] / n[1], 1, n[2] / n[1]};
}

std::array<double, 3> neutralFromMultipliers(const std::array<double, 3>& m) { return {m[1] / m[0], 1, m[1] / m[2]}; }
std::array<double, 3> multipliersFromNeutral(const std::array<double, 3>& n) { return {n[1] / n[0], 1, n[1] / n[2]}; }

} // namespace compositor
