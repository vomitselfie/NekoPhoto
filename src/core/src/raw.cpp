#include "compositor/raw.h"
#include "compositor/document.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>

#ifdef COMPOSITOR_HAVE_LIBRAW
#include <libraw/libraw.h>
#endif

namespace compositor {

bool rawSupported() {
#ifdef COMPOSITOR_HAVE_LIBRAW
    return true;
#else
    return false;
#endif
}

bool isRawPath(const std::string& path) {
    static const std::array<const char*, 26> extensions{"cr2", "cr3", "crw", "nef", "nrw", "arw", "srf", "sr2", "raf", "orf", "rw2", "rwl",
                                                        "pef", "ptx", "dng", "3fr", "fff", "iiq", "erf", "kdc", "dcr", "mrw", "mos", "srw", "x3f", "raw"};
    const auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return std::find_if(extensions.begin(), extensions.end(), [&](const char* e) { return ext == e; }) != extensions.end();
}

std::vector<uint8_t> readRawFileBytes(const std::string& path, std::string* error, size_t limit) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) { if (error) *error = "Couldn't read the file."; return {}; }
    const std::streamoff size = in.tellg();
    if (size <= 0 || uint64_t(size) > uint64_t(limit)) { if (error) *error = size <= 0 ? "The file is empty." : "The file is too large."; return {}; }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) { if (error) *error = "Couldn't read the file."; return {}; }
    return bytes;
}

#ifdef COMPOSITOR_HAVE_LIBRAW
namespace {

int cancelCallback(void* data, enum LibRaw_progress, int, int) {
    const auto* cancel = static_cast<const std::atomic<bool>*>(data);
    return cancel && cancel->load() ? 1 : 0;
}

void setError(std::string* error, const std::string& message) { if (error) *error = message; }

/// Opens `bytes` (which must outlive it) and checks the size against the canvas budget; null with `error` otherwise.
std::unique_ptr<LibRaw> openRaw(const std::vector<uint8_t>& bytes, std::string* error) {
    if (bytes.empty()) { setError(error, "The file is empty."); return nullptr; }
    auto raw = std::make_unique<LibRaw>();   // large: on the heap
    // LibRaw only reads the buffer; before 0.21 its parameter was a plain void*.
    if (int r = raw->open_buffer(const_cast<uint8_t*>(bytes.data()), bytes.size()); r != LIBRAW_SUCCESS) {
        setError(error, std::string("Couldn't read the RAW file: ") + libraw_strerror(r));
        return nullptr;
    }
    const auto& size = raw->imgdata.sizes;
    if (size.width > 30000 || size.height > 30000) { setError(error, "Images up to 30,000 pixels per side are supported."); return nullptr; }
    if ((long long)size.width * size.height > Document::pixelBudget || (long long)size.raw_width * size.raw_height > 2 * Document::pixelBudget) {
        setError(error, "The RAW image is larger than the 100-megapixel canvas budget.");
        return nullptr;
    }
    return raw;
}

/// Demosaiced with the camera's white balance into sRGB at `bits` (8 or 16); LibRaw's processed image, or null.
libraw_processed_image_t* process(LibRaw& raw, int bits, const RawDecodeOptions& options, std::string* error) {
    if (options.multipliers) {
        const auto& m = *options.multipliers;
        raw.imgdata.params.use_camera_wb = 0;
        raw.imgdata.params.user_mul[0] = float(m[0]);
        raw.imgdata.params.user_mul[1] = raw.imgdata.params.user_mul[3] = float(m[1]);
        raw.imgdata.params.user_mul[2] = float(m[2]);
    } else {
        raw.imgdata.params.use_camera_wb = 1;
    }
    raw.imgdata.params.output_color = 1;   // sRGB
    raw.imgdata.params.output_bps = bits;
    raw.imgdata.params.half_size = options.halfSize ? 1 : 0;
    if (options.cancel) raw.set_progress_handler(cancelCallback, const_cast<std::atomic<bool>*>(options.cancel));
    auto fail = [&](int code) -> libraw_processed_image_t* {
        setError(error, code == LIBRAW_CANCELLED_BY_CALLBACK ? std::string("Cancelled.") : std::string("Couldn't read the RAW file: ") + libraw_strerror(code));
        return nullptr;
    };
    if (int r = raw.unpack(); r != LIBRAW_SUCCESS) return fail(r);
    if (options.cancel && options.cancel->load()) return fail(LIBRAW_CANCELLED_BY_CALLBACK);
    if (int r = raw.dcraw_process(); r != LIBRAW_SUCCESS) return fail(r);
    int code = 0;
    libraw_processed_image_t* developed = raw.dcraw_make_mem_image(&code);
    if (!developed) return fail(code);
    if (developed->type != LIBRAW_IMAGE_BITMAP || developed->bits != bits || (developed->colors != 3 && developed->colors != 1)) {
        LibRaw::dcraw_clear_mem(developed);
        setError(error, "The RAW file developed into an unexpected format.");
        return nullptr;
    }
    return developed;
}

} // namespace
#endif

std::shared_ptr<Image> decodeRaw(const std::string& path, std::string* error) {
#ifdef COMPOSITOR_HAVE_LIBRAW
    const std::vector<uint8_t> bytes = readRawFileBytes(path, error);
    if (bytes.empty()) return nullptr;
    auto raw = openRaw(bytes, error);
    if (!raw) return nullptr;
    libraw_processed_image_t* developed = process(*raw, 8, {}, error);
    if (!developed) return nullptr;
    auto out = std::make_shared<Image>(developed->width, developed->height);
    const int n = developed->colors;
    for (int y = 0; y < developed->height; y++) {
        const uint8_t* src = developed->data + size_t(y) * developed->width * n;
        uint8_t* dst = out->row(y);
        for (int x = 0; x < developed->width; x++, src += n, dst += 4) {
            dst[0] = src[0]; dst[1] = src[n == 3 ? 1 : 0]; dst[2] = src[n == 3 ? 2 : 0]; dst[3] = 255;
        }
    }
    LibRaw::dcraw_clear_mem(developed);
    return out;
#else
    (void)path;
    if (error) *error = "This build of NekoPhoto has no RAW support (LibRaw).";
    return nullptr;
#endif
}

bool readRawInfo(const std::vector<uint8_t>& bytes, RawInfo& info, std::string* error) {
#ifdef COMPOSITOR_HAVE_LIBRAW
    auto raw = openRaw(bytes, error);
    if (!raw) return false;
    const auto& id = raw->imgdata.idata;
    const auto& size = raw->imgdata.sizes;
    const auto& other = raw->imgdata.other;
    info.make = id.make;
    info.model = id.model;
    const bool turned = size.flip == 5 || size.flip == 6;   // a quarter turn either way
    info.width = turned ? size.height : size.width;
    info.height = turned ? size.width : size.height;
    info.iso = other.iso_speed;
    info.shutter = other.shutter;
    info.aperture = other.aperture;
    info.focalLength = other.focal_len;
    return true;
#else
    (void)bytes; (void)info;
    if (error) *error = "This build of NekoPhoto has no RAW support (LibRaw).";
    return false;
#endif
}

std::shared_ptr<Image16> decodeRaw16(const std::vector<uint8_t>& bytes, const RawDecodeOptions& options, std::string* error) {
#ifdef COMPOSITOR_HAVE_LIBRAW
    auto raw = openRaw(bytes, error);
    if (!raw) return nullptr;
    libraw_processed_image_t* developed = process(*raw, 16, options, error);
    if (!developed) return nullptr;
    auto out = std::make_shared<Image16>(developed->width, developed->height);
    const int n = developed->colors;
    // LibRaw's 0..65535 onto the 15-bit scale, rounded.
    auto level = [](uint16_t v) { return uint16_t((uint32_t(v) * 32768u + 32767u) / 65535u); };
    for (int y = 0; y < developed->height; y++) {
        const auto* src = reinterpret_cast<const uint16_t*>(developed->data) + size_t(y) * size_t(developed->width) * size_t(n);
        uint16_t* dst = out->row(y);
        for (int x = 0; x < developed->width; x++, src += n, dst += 4) {
            dst[0] = level(src[0]); dst[1] = level(src[n == 3 ? 1 : 0]); dst[2] = level(src[n == 3 ? 2 : 0]); dst[3] = 32768;
        }
    }
    LibRaw::dcraw_clear_mem(developed);
    return out;
#else
    (void)bytes; (void)options;
    if (error) *error = "This build of NekoPhoto has no RAW support (LibRaw).";
    return nullptr;
#endif
}

std::shared_ptr<Image16> developRaw(const std::vector<uint8_t>& bytes, const CameraRawSettings& settings, const RawDecodeOptions& options,
                                    std::string* error, uint32_t seed) {
    const CameraRawSettings normalized = settings.normalized();
    RawDecodeOptions decode = options;
    if (!decode.multipliers) {
        RawWhiteBalance balance;
        if (readRawWhiteBalance(bytes, balance, nullptr)) decode.multipliers = balance.multipliersFor(normalized);
    }
    auto image = decodeRaw16(bytes, decode, error);
    if (!image) return nullptr;
    if (options.cancel && options.cancel->load()) { if (error) *error = "Cancelled."; return nullptr; }
    if (!normalized.isIdentity() && !applyCameraRaw(*image, normalized, options.halfSize ? 0.5 : 1.0, seed)) {
        if (error) *error = "The Camera Raw settings are not valid.";
        return nullptr;
    }
    return image;
}

std::optional<std::array<double, 2>> rawAutoBalance(const std::vector<uint8_t>& bytes) {
    RawDecodeOptions options;
    options.halfSize = true;
    auto image = decodeRaw16(bytes, options, nullptr);
    if (!image) return std::nullopt;
    auto solved = CameraRawSettings::autoBalance(*image);
    if (solved) for (double& v : *solved) v = std::clamp(v, -100.0, 100.0);
    return solved;
}

// ---- white balance --------------------------------------------------------------------------------

namespace {

std::optional<std::array<double, 3>> balanced(double r, double g, double b) {
    if (!(r > 0 && g > 0 && b > 0) || !std::isfinite(r + g + b)) return std::nullopt;
    return std::array<double, 3>{r / g, 1, b / g};
}

bool isPresetMode(CameraRawWhiteBalance mode) {
    return mode != CameraRawWhiteBalance::Custom && mode != CameraRawWhiteBalance::Auto && mode != CameraRawWhiteBalance::AsShot;
}

// LibRaw's output curve with its default gamma (0.45 with a linear toe of slope 4.5, BT.709's shape), solved as its
// gamma_curve does: the toe ends at 0.018054 and the offset is 0.099297.
constexpr double kToe = 0.018054, kOffset = 0.099297, kPower = 0.45, kSlope = 4.5;
double toLinear(double v) { return v < kToe * kSlope ? v / kSlope : std::pow((v + kOffset) / (1 + kOffset), 1 / kPower); }
double fromLinear(double l) { return l < kToe ? l * kSlope : (1 + kOffset) * std::pow(l, kPower) - kOffset; }

std::array<double, 3> times(const Matrix3& m, const std::array<double, 3>& v) {
    return {m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2], m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
            m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2]};
}

} // namespace

const RawWhiteBalance::Preset* RawWhiteBalance::preset(CameraRawWhiteBalance mode) const {
    for (const Preset& p : presets) if (p.mode == mode) return &p;
    return nullptr;
}

std::optional<std::array<double, 3>> RawWhiteBalance::multipliersFor(TemperatureTint value) const {
    if (!kelvin) return std::nullopt;
    const Chromaticity xy = xyFromTemperatureTint(std::clamp(value.temperature, kMinRawTemperature, kMaxRawTemperature),
                                                  std::clamp(value.tint, -kMaxRawTint, kMaxRawTint));
    const auto neutral = model.neutralFromXy(xy);
    if (!neutral) return std::nullopt;
    return multipliersFromNeutral(*neutral);
}

std::optional<TemperatureTint> RawWhiteBalance::valueOf(const std::array<double, 3>& multipliers) const {
    if (!model.valid) return std::nullopt;
    const auto xy = model.xyFromNeutral(neutralFromMultipliers(multipliers));
    if (!xy) return std::nullopt;
    return temperatureTintFromXy(*xy);
}

std::optional<std::array<double, 3>> RawWhiteBalance::multipliersFor(const CameraRawSettings& settings) const {
    if (settings.whiteBalance == CameraRawWhiteBalance::AsShot) return std::nullopt;   // the camera's own, exactly
    if (isPresetMode(settings.whiteBalance))
        if (const Preset* p = preset(settings.whiteBalance)) return p->multipliers;
    if (!(settings.rawTemperature > 0)) return std::nullopt;
    return multipliersFor(TemperatureTint{settings.rawTemperature, settings.rawTint});
}

bool readRawWhiteBalance(const std::vector<uint8_t>& bytes, RawWhiteBalance& out, std::string* error) {
#ifdef COMPOSITOR_HAVE_LIBRAW
    auto raw = openRaw(bytes, error);
    if (!raw) return false;
    out = {};
    const auto& c = raw->imgdata.color;
    // As Shot: the camera's balance (a DNG's AsShotNeutral arrives here as its reciprocal), or LibRaw's daylight balance
    // when the file has none, which is what its decode falls back to as well.
    auto shot = balanced(c.cam_mul[0], c.cam_mul[1], c.cam_mul[2]);
    if (!shot) shot = balanced(c.pre_mul[0], c.pre_mul[1], c.pre_mul[2]);
    if (shot) out.asShot = *shot;
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) out.cameraToSrgb[r][k] = c.rgb_cam[r][k];
    auto nonZero = [](const Matrix3& m) {
        for (const auto& row : m) for (double v : row) if (v != 0) return true;
        return false;
    };
    if (raw->imgdata.idata.colors == 3 && raw->imgdata.idata.dng_version) {
        // The DNG's ColorMatrix1/2 (XYZ -> camera under CalibrationIlluminant1/2), after CameraCalibration1/2 when given.
        Matrix3 m[2] = {};
        double t[2] = {0, 0};
        for (int i = 0; i < 2; i++) {
            const auto& d = c.dng_color[i];
            Matrix3 calibration{};
            for (int r = 0; r < 3; r++)
                for (int k = 0; k < 3; k++) { m[i][r][k] = d.colormatrix[r][k]; calibration[r][k] = d.calibration[r][k]; }
            if (nonZero(calibration)) {
                Matrix3 product{};
                for (int r = 0; r < 3; r++)
                    for (int k = 0; k < 3; k++)
                        for (int j = 0; j < 3; j++) product[r][k] += calibration[r][j] * m[i][j][k];
                m[i] = product;
            }
            if (!nonZero(m[i])) continue;
            t[i] = lightSourceTemperature(d.illuminant);
            if (t[i] == 0) t[i] = 6504;   // an unnamed illuminant: D65
        }
        if (nonZero(m[0]) && nonZero(m[1])) out.model = CameraWhiteModel::dual(m[0], t[0], m[1], t[1]);
        else if (nonZero(m[0]) || nonZero(m[1])) out.model = CameraWhiteModel::single(nonZero(m[0]) ? m[0] : m[1]);
    } else if (raw->imgdata.idata.colors == 3) {
        // Other formats: LibRaw's matrix for the camera, Adobe's XYZ (D65) -> camera.
        Matrix3 m{};
        for (int r = 0; r < 3; r++)
            for (int k = 0; k < 3; k++) m[r][k] = c.cam_xyz[r][k];
        if (nonZero(m)) out.model = CameraWhiteModel::single(m);
    }
    if (const auto value = out.valueOf(out.asShot)) {
        out.kelvin = true;
        out.asShotValue = *value;
    }
    // The presets the camera recorded, by EXIF light source; none is made up.
    const std::pair<CameraRawWhiteBalance, std::vector<int>> sources[] = {
        {CameraRawWhiteBalance::Daylight, {1, 9}},
        {CameraRawWhiteBalance::Cloudy, {10}},
        {CameraRawWhiteBalance::Shade, {11}},
        {CameraRawWhiteBalance::Tungsten, {3, 17}},
        {CameraRawWhiteBalance::Fluorescent, {2, 14, 12, 13, 15, 16}},
        {CameraRawWhiteBalance::Flash, {4}},
    };
    if (out.kelvin)
        for (const auto& [mode, codes] : sources)
            for (int code : codes) {
                const auto* w = c.WB_Coeffs[code];
                const auto m = balanced(w[0], w[1], w[2]);
                if (!m) continue;
                const auto value = out.valueOf(*m);
                if (!value) continue;
                out.presets.push_back({mode, *m, *value});
                break;
            }
    return true;
#else
    (void)bytes; (void)out;
    if (error) *error = "This build of NekoPhoto has no RAW support (LibRaw).";
    return false;
#endif
}

std::optional<TemperatureTint> rawAutoWhiteBalance(const Image16& decoded, const std::array<double, 3>& multipliers, const RawWhiteBalance& balance) {
    if (!balance.kelvin) return std::nullopt;
    const auto toCamera = invert(balance.cameraToSrgb);
    if (!toCamera) return std::nullopt;
    // Gray world over the pixels that are neither clipped nor black, in linear light.
    std::vector<double> linear(32769);
    for (size_t i = 0; i < linear.size(); i++) linear[i] = toLinear(double(i) / 32768);
    double sum[3] = {0, 0, 0};
    size_t count = 0;
    for (int y = 0; y < decoded.height(); y++) {
        const uint16_t* p = decoded.row(y);
        for (int x = 0; x < decoded.width(); x++, p += 4) {
            if (p[3] == 0) continue;
            const uint16_t hi = std::max({p[0], p[1], p[2]}), lo = std::min({p[0], p[1], p[2]});
            if (hi > 31130 || lo < 160) continue;   // 95 % and 0.5 %
            for (int c = 0; c < 3; c++) sum[c] += linear[std::min<uint16_t>(p[c], 32768)];
            count++;
        }
    }
    if (count == 0) return std::nullopt;
    // Back through LibRaw's camera -> sRGB matrix and the multipliers of the decode: the grey's raw camera response.
    const auto camera = times(*toCamera, {sum[0], sum[1], sum[2]});
    std::array<double, 3> neutral{};
    for (int c = 0; c < 3; c++) neutral[c] = camera[c] / multipliers[size_t(c)];
    if (!(neutral[0] > 0 && neutral[1] > 0 && neutral[2] > 0)) return std::nullopt;
    const auto value = balance.valueOf(multipliersFromNeutral(neutral));
    if (!value) return std::nullopt;
    return TemperatureTint{std::clamp(value->temperature, kMinRawTemperature, kMaxRawTemperature), std::clamp(value->tint, -kMaxRawTint, kMaxRawTint)};
}

void rebalanceRawDecode(Image16& image, const RawWhiteBalance& balance, const std::array<double, 3>& from, const std::array<double, 3>& to) {
    const auto toCamera = invert(balance.cameraToSrgb);
    if (!toCamera || from == to) return;
    // LibRaw scales the channels by the multipliers over their smallest, then maps camera -> sRGB.
    const double fromMin = std::min({from[0], from[1], from[2]}), toMin = std::min({to[0], to[1], to[2]});
    Matrix3 a{};
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++)
            for (int j = 0; j < 3; j++)
                a[r][k] += balance.cameraToSrgb[r][j] * ((to[size_t(j)] / toMin) / (from[size_t(j)] / fromMin)) * (*toCamera)[j][k];
    std::vector<float> linear(32769);
    for (size_t i = 0; i < linear.size(); i++) linear[i] = float(toLinear(double(i) / 32768));
    constexpr int encodeSteps = 1 << 16;
    std::vector<uint16_t> encode(encodeSteps + 1);
    for (int i = 0; i <= encodeSteps; i++) encode[size_t(i)] = uint16_t(std::lround(std::clamp(fromLinear(double(i) / encodeSteps), 0.0, 1.0) * 32768));
    for (int y = 0; y < image.height(); y++) {
        uint16_t* p = image.row(y);
        for (int x = 0; x < image.width(); x++, p += 4) {
            const std::array<double, 3> l{linear[std::min<uint16_t>(p[0], 32768)], linear[std::min<uint16_t>(p[1], 32768)],
                                          linear[std::min<uint16_t>(p[2], 32768)]};
            const auto o = times(a, l);
            for (int c = 0; c < 3; c++) p[c] = encode[size_t(std::lround(std::clamp(o[size_t(c)], 0.0, 1.0) * encodeSteps))];
        }
    }
}

std::string resolveRawWhiteBalance(const std::vector<uint8_t>& bytes, CameraRawSettings& s, bool temperatureGiven, bool tintGiven) {
    RawWhiteBalance balance;
    std::string error;
    if (!readRawWhiteBalance(bytes, balance, &error)) return error;
    const bool kelvinGiven = temperatureGiven && std::fabs(s.temperature) > 100;
    const bool rawGiven = s.rawTemperature != 0 || s.rawTint != 0;
    const bool rawMode = s.whiteBalance != CameraRawWhiteBalance::Custom && s.whiteBalance != CameraRawWhiteBalance::Auto;
    if ((kelvinGiven || rawGiven || rawMode) && !balance.kelvin)
        return "this camera's colour matrix is unknown, so its white balance cannot be set in kelvin or by preset: give temperature and "
               "tint relative to the as-shot balance (-100..100)";
    if (kelvinGiven) {
        s.rawTemperature = s.temperature;
        s.rawTint = tintGiven ? s.tint : balance.asShotValue.tint;
        s.temperature = s.tint = 0;
        if (rawMode) s.whiteBalance = CameraRawWhiteBalance::Custom;   // numbers make it Custom, as a slider does
    } else if (rawGiven && s.rawTemperature == 0) {
        s.rawTemperature = balance.asShotValue.temperature;
    }
    if (kelvinGiven || rawGiven) {
        if (!(s.rawTemperature >= kMinRawTemperature && s.rawTemperature <= kMaxRawTemperature)) return "temperature must be 2000..50000 K";
        if (!(std::fabs(s.rawTint) <= kMaxRawTint)) return "tint must be -150..150";
    }
    auto fill = [&](TemperatureTint v) {
        s.rawTemperature = std::clamp(v.temperature, kMinRawTemperature, kMaxRawTemperature);
        s.rawTint = std::clamp(v.tint, -kMaxRawTint, kMaxRawTint);
        s.temperature = s.tint = 0;
    };
    const bool nothingGiven = !temperatureGiven && !tintGiven && !rawGiven;
    if (s.whiteBalance == CameraRawWhiteBalance::AsShot) {
        fill(balance.asShotValue);
    } else if (isPresetMode(s.whiteBalance)) {
        const RawWhiteBalance::Preset* p = balance.preset(s.whiteBalance);
        if (!p) return std::string("this file records no ") + cameraRawName(s.whiteBalance) + " white balance";
        fill(p->value);
    } else if (s.whiteBalance == CameraRawWhiteBalance::Auto && nothingGiven) {
        if (balance.kelvin) {
            RawDecodeOptions quick;
            quick.halfSize = true;
            if (auto decoded = decodeRaw16(bytes, quick, nullptr))
                if (auto solved = rawAutoWhiteBalance(*decoded, balance.asShot, balance)) fill(*solved);
        } else if (auto solved = rawAutoBalance(bytes)) {
            s.temperature = (*solved)[0];
            s.tint = (*solved)[1];
        }
    } else if (s.whiteBalance == CameraRawWhiteBalance::Custom && nothingGiven && balance.kelvin) {
        s.whiteBalance = CameraRawWhiteBalance::AsShot;   // nothing said about white balance: as shot, shown in kelvin
        fill(balance.asShotValue);
    }
    return {};
}

} // namespace compositor
