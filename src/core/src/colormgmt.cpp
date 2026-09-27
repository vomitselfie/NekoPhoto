// Colour management through Little CMS (colormgmt.h, docs/color-management.md).
#include "compositor/colormgmt.h"
#include "compositor/depth.h"
#include "compositor/document.h"
#include "compositor/parallel.h"
#include "lcms2.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <list>
#include <map>
#include <mutex>

namespace compositor {

namespace {

uint64_t fingerprint(const std::vector<uint8_t>& bytes) {
    uint64_t h = 1469598103934665603ull;
    for (uint8_t b : bytes) { h ^= b; h *= 1099511628211ull; }
    return h ^ (uint64_t(bytes.size()) << 1);
}

/// A profile handle in `context`; untagged opens as the built-in sRGB.
cmsHPROFILE openProfile(cmsContext context, const ColorProfile& profile) {
    const ColorProfile& p = effectiveProfile(profile);
    return cmsOpenProfileFromMemTHR(context, p.icc.data(), cmsUInt32Number(p.icc.size()));
}

cmsUInt32Number lcmsFormat(PixelFormat f) {
    switch (f) {
    case PixelFormat::RGBA8: return TYPE_RGBA_8;
    case PixelFormat::RGBA16: return TYPE_RGBA_FLT;   // through the float pipeline: exact for 15-bit levels
    case PixelFormat::RGBFloat: return TYPE_RGB_FLT;
    }
    return TYPE_RGBA_8;
}

cmsUInt32Number lcmsIntent(RenderingIntent intent) { return cmsUInt32Number(intent); }

// ---- The transform cache -------------------------------------------------------------------------------------------

struct CacheKey {
    uint64_t from = 0, to = 0, proof = 0;
    int intent = 0, proofIntent = 0, flags = 0, input = 0, output = 0;
    std::array<uint8_t, 3> warning{};
    auto tie() const { return std::tie(from, to, proof, intent, proofIntent, flags, input, output, warning); }
    bool operator==(const CacheKey& o) const { return tie() == o.tie(); }
};

constexpr size_t cacheCapacity = 24;
std::mutex cacheMutex;
std::list<std::pair<CacheKey, ColorTransformPtr>>& cache() {
    static std::list<std::pair<CacheKey, ColorTransformPtr>> entries;
    return entries;
}

ColorTransformPtr cached(const CacheKey& key) {
    std::lock_guard<std::mutex> lock(cacheMutex);
    auto& entries = cache();
    for (auto it = entries.begin(); it != entries.end(); ++it)
        if (it->first == key) { entries.splice(entries.begin(), entries, it); return entries.front().second; }
    return nullptr;
}

void remember(const CacheKey& key, const ColorTransformPtr& transform) {
    std::lock_guard<std::mutex> lock(cacheMutex);
    auto& entries = cache();
    entries.emplace_front(key, transform);
    while (entries.size() > cacheCapacity) entries.pop_back();
}

} // namespace

// ---- The transform object ------------------------------------------------------------------------------------------

struct TransformFactory {
    static std::shared_ptr<ColorTransform> make(cmsContext context, cmsHTRANSFORM transform, PixelFormat input, PixelFormat output) {
        std::shared_ptr<ColorTransform> t(new ColorTransform());
        t->context_ = context;
        t->transform_ = transform;
        t->input_ = input;
        t->output_ = output;
        return t;
    }
};

ColorTransform::~ColorTransform() {
    if (transform_) cmsDeleteTransform(static_cast<cmsHTRANSFORM>(transform_));
    if (context_) cmsDeleteContext(static_cast<cmsContext>(context_));
}

void ColorTransform::apply(const void* in, void* out, size_t count) const {
    auto* t = static_cast<cmsHTRANSFORM>(transform_);
    if (input_ == PixelFormat::RGBFloat) { cmsDoTransform(t, in, out, cmsUInt32Number(count)); return; }
    // Premultiplied pixels are made straight for Little CMS a chunk at a time, and premultiplied again after; alpha is
    // copied. An 8-bit chunk that is all opaque goes through as it is. 16-bit pixels go through Little CMS's float
    // pipeline (its 16-bit one precalculates a grid, which is coarse near the gamut's edges).
    constexpr size_t chunk = 1024;
    thread_local std::vector<float> wide;
    thread_local std::vector<uint8_t> narrow;
    wide.resize(chunk * 4);
    narrow.resize(chunk * 4);
    for (size_t done = 0; done < count; done += chunk) {
        const size_t n = std::min(chunk, count - done);
        const uint8_t* src8 = input_ == PixelFormat::RGBA8 ? static_cast<const uint8_t*>(in) + done * 4 : nullptr;
        const uint16_t* src16 = input_ == PixelFormat::RGBA16 ? static_cast<const uint16_t*>(in) + done * 4 : nullptr;
        uint8_t* dst8 = output_ == PixelFormat::RGBA8 ? static_cast<uint8_t*>(out) + done * 4 : nullptr;
        uint16_t* dst16 = output_ == PixelFormat::RGBA16 ? static_cast<uint16_t*>(out) + done * 4 : nullptr;
        bool opaque = true;
        const void* staged = nullptr;
        if (src8) {
            for (size_t i = 0; i < n && opaque; i++) opaque = src8[i * 4 + 3] == 255;
            if (opaque) staged = src8;
            else {
                for (size_t i = 0; i < n * 4; i += 4) {
                    const unsigned a = src8[i + 3];
                    narrow[i + 3] = uint8_t(a);
                    for (size_t c = 0; c < 3; c++)
                        narrow[i + c] = a == 255 ? src8[i + c] : a == 0 ? 0 : uint8_t(std::min(255u, (src8[i + c] * 255u + a / 2) / a));
                }
                staged = narrow.data();
            }
        } else {
            constexpr float unit = 1.0f / float(one16);
            for (size_t i = 0; i < n * 4; i += 4) {
                const uint32_t a = src16[i + 3];
                opaque &= a >= one16;
                wide[i + 3] = float(std::min(a, one16)) * unit;
                for (size_t c = 0; c < 3; c++) {
                    const uint32_t v = src16[i + c];
                    wide[i + c] = a >= one16 ? float(v) * unit : a == 0 ? 0.0f : std::min(1.0f, float(v) / float(a));
                }
            }
            staged = wide.data();
        }
        if (dst8) {
            cmsDoTransform(t, staged, dst8, cmsUInt32Number(n));
            if (!opaque)
                for (size_t i = 0; i < n * 4; i += 4) {
                    const unsigned a = dst8[i + 3];
                    if (a == 255) continue;
                    for (size_t c = 0; c < 3; c++) dst8[i + c] = uint8_t((dst8[i + c] * a + 127) / 255);
                }
        } else {
            // Into `wide` (in place when it holds the input), then back to 0..32768 with the input's own alpha.
            cmsDoTransform(t, staged, wide.data(), cmsUInt32Number(n));
            for (size_t i = 0; i < n * 4; i += 4) {
                const uint32_t a = src8 ? widen8(src8[i + 3]) : src16[i + 3];
                for (size_t c = 0; c < 3; c++) {
                    const float straight = std::clamp(wide[i + c], 0.0f, 1.0f) * float(std::min(a, one16));
                    dst16[i + c] = uint16_t(std::lround(straight));
                }
                dst16[i + 3] = uint16_t(a);
            }
        }
    }
}

// ---- Profiles ------------------------------------------------------------------------------------------------------

const char* workingSpaceKey(WorkingSpace space) {
    switch (space) {
    case WorkingSpace::SRGB: return "srgb";
    case WorkingSpace::AdobeRGB: return "adobe-rgb";
    case WorkingSpace::DisplayP3: return "display-p3";
    case WorkingSpace::ProPhoto: return "prophoto";
    }
    return "srgb";
}

std::optional<WorkingSpace> workingSpaceFromKey(const std::string& key) {
    for (WorkingSpace s : {WorkingSpace::SRGB, WorkingSpace::AdobeRGB, WorkingSpace::DisplayP3, WorkingSpace::ProPhoto})
        if (key == workingSpaceKey(s)) return s;
    return std::nullopt;
}

const char* workingSpaceName(WorkingSpace space) {
    switch (space) {
    case WorkingSpace::SRGB: return "sRGB IEC61966-2.1";
    case WorkingSpace::AdobeRGB: return "Adobe RGB (1998)";
    case WorkingSpace::DisplayP3: return "Display P3";
    case WorkingSpace::ProPhoto: return "ProPhoto RGB";
    }
    return "sRGB IEC61966-2.1";
}

namespace {

std::vector<uint8_t> saveProfile(cmsHPROFILE h) {
    cmsUInt32Number size = 0;
    if (!cmsSaveProfileToMem(h, nullptr, &size) || size == 0) return {};
    std::vector<uint8_t> bytes(size);
    if (!cmsSaveProfileToMem(h, bytes.data(), &size)) return {};
    bytes.resize(size);
    return bytes;
}

std::vector<uint8_t> makeBuiltin(WorkingSpace space) {
    cmsContext context = cmsCreateContext(nullptr, nullptr);
    cmsHPROFILE h = nullptr;
    const cmsCIExyY d65{0.3127, 0.3290, 1.0}, d50{0.3457, 0.3585, 1.0};
    auto rgb = [&](const cmsCIExyY& white, std::array<double, 6> xy, cmsToneCurve* curve) {
        const cmsCIExyYTRIPLE primaries{{xy[0], xy[1], 1.0}, {xy[2], xy[3], 1.0}, {xy[4], xy[5], 1.0}};
        cmsToneCurve* curves[3] = {curve, curve, curve};
        cmsHPROFILE p = cmsCreateRGBProfileTHR(context, &white, &primaries, curves);
        cmsFreeToneCurve(curve);
        return p;
    };
    // sRGB's curve: IEC 61966-2.1, Little CMS's parametric type 4.
    const double srgbCurve[5] = {2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92, 0.04045};
    switch (space) {
    case WorkingSpace::SRGB: h = cmsCreate_sRGBProfileTHR(context); break;
    case WorkingSpace::AdobeRGB: h = rgb(d65, {0.64, 0.33, 0.21, 0.71, 0.15, 0.06}, cmsBuildGamma(context, 563.0 / 256.0)); break;
    case WorkingSpace::DisplayP3: h = rgb(d65, {0.680, 0.320, 0.265, 0.690, 0.150, 0.060}, cmsBuildParametricToneCurve(context, 4, srgbCurve)); break;
    case WorkingSpace::ProPhoto: h = rgb(d50, {0.7347, 0.2653, 0.1596, 0.8404, 0.0366, 0.0001}, cmsBuildGamma(context, 1.8)); break;
    }
    std::vector<uint8_t> bytes;
    if (h) {
        cmsMLU* description = cmsMLUalloc(context, 1);
        cmsMLUsetASCII(description, "en", "US", workingSpaceName(space));
        cmsWriteTag(h, cmsSigProfileDescriptionTag, description);
        cmsMLUfree(description);
        cmsMLU* copyright = cmsMLUalloc(context, 1);
        cmsMLUsetASCII(copyright, "en", "US", "No copyright, use freely");
        cmsWriteTag(h, cmsSigCopyrightTag, copyright);
        cmsMLUfree(copyright);
        cmsSetHeaderRenderingIntent(h, INTENT_PERCEPTUAL);
        bytes = saveProfile(h);
        cmsCloseProfile(h);
    }
    // The same bytes in every session: a fixed creation date (2026-01-01), then the profile ID computed over it.
    if (bytes.size() >= 128) {
        const uint16_t date[6] = {2026, 1, 1, 0, 0, 0};
        for (int i = 0; i < 6; i++) { bytes[size_t(24 + i * 2)] = uint8_t(date[i] >> 8); bytes[size_t(25 + i * 2)] = uint8_t(date[i]); }
        if (cmsHPROFILE again = cmsOpenProfileFromMemTHR(context, bytes.data(), cmsUInt32Number(bytes.size()))) {
            cmsMD5computeID(again);
            std::vector<uint8_t> resaved = saveProfile(again);
            if (!resaved.empty()) bytes = std::move(resaved);
            cmsCloseProfile(again);
        }
    }
    cmsDeleteContext(context);
    return bytes;
}

ColorModel modelOf(cmsColorSpaceSignature space) {
    switch (space) {
    case cmsSigRgbData: return ColorModel::RGB;
    case cmsSigGrayData: return ColorModel::Gray;
    case cmsSigCmykData: return ColorModel::CMYK;
    case cmsSigLabData: return ColorModel::Lab;
    default: return ColorModel::Other;
    }
}

} // namespace

const ColorProfile& builtinProfile(WorkingSpace space) {
    static const std::array<ColorProfile, 4> profiles = [] {
        std::array<ColorProfile, 4> out;
        for (WorkingSpace s : {WorkingSpace::SRGB, WorkingSpace::AdobeRGB, WorkingSpace::DisplayP3, WorkingSpace::ProPhoto}) {
            ColorProfile& p = out[size_t(s)];
            p.icc = makeBuiltin(s);
            p.description = workingSpaceName(s);
            p.model = ColorModel::RGB;
        }
        return out;
    }();
    return profiles[size_t(space)];
}

std::optional<ColorProfile> profileFromIcc(const uint8_t* data, size_t size) {
    if (!data || size < 132 || size > (size_t(64) << 20)) return std::nullopt;
    cmsContext context = cmsCreateContext(nullptr, nullptr);
    cmsHPROFILE h = cmsOpenProfileFromMemTHR(context, data, cmsUInt32Number(size));
    if (!h) { cmsDeleteContext(context); return std::nullopt; }
    ColorProfile p;
    p.icc.assign(data, data + size);
    p.model = modelOf(cmsGetColorSpace(h));
    char text[256] = {};
    if (cmsGetProfileInfoASCII(h, cmsInfoDescription, "en", "US", text, sizeof text) > 0) p.description = text;
    if (p.description.empty()) p.description = "Untitled profile";
    cmsCloseProfile(h);
    cmsDeleteContext(context);
    return p;
}

std::optional<ColorProfile> profileFromIcc(const std::vector<uint8_t>& icc) { return profileFromIcc(icc.data(), icc.size()); }

const ColorProfile& effectiveProfile(const ColorProfile& profile) { return profile.empty() ? srgbProfile() : profile; }

bool equivalentProfiles(const ColorProfile& a, const ColorProfile& b) {
    const ColorProfile& pa = effectiveProfile(a);
    const ColorProfile& pb = effectiveProfile(b);
    if (pa.icc == pb.icc) return true;
    if (pa.model != ColorModel::RGB || pb.model != ColorModel::RGB) return false;
    static std::mutex mutex;
    static std::map<std::pair<uint64_t, uint64_t>, bool> known;
    const uint64_t fa = fingerprint(pa.icc), fb = fingerprint(pb.icc);
    const std::pair<uint64_t, uint64_t> key{std::min(fa, fb), std::max(fa, fb)};
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (auto it = known.find(key); it != known.end()) return it->second;
    }
    // A grid of colours through a to b: the same colours when none moves by more than a quarter of an 8-bit level.
    bool same = false;
    cmsContext context = cmsCreateContext(nullptr, nullptr);
    cmsHPROFILE ha = openProfile(context, pa), hb = openProfile(context, pb);
    if (ha && hb) {
        if (cmsHTRANSFORM t = cmsCreateTransformTHR(context, ha, TYPE_RGB_16, hb, TYPE_RGB_16, INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOCACHE | cmsFLAGS_NOOPTIMIZE)) {
            std::vector<uint16_t> in, out;
            for (int r = 0; r <= 4; r++) for (int g = 0; g <= 4; g++) for (int b2 = 0; b2 <= 4; b2++)
                for (int v : {r, g, b2}) in.push_back(uint16_t(v * 65535 / 4));
            out.resize(in.size());
            cmsDoTransform(t, in.data(), out.data(), cmsUInt32Number(in.size() / 3));
            same = true;
            for (size_t i = 0; i < in.size(); i++) if (std::abs(int(in[i]) - int(out[i])) > 64) { same = false; break; }
            cmsDeleteTransform(t);
        }
    }
    if (ha) cmsCloseProfile(ha);
    if (hb) cmsCloseProfile(hb);
    cmsDeleteContext(context);
    std::lock_guard<std::mutex> lock(mutex);
    if (known.size() > 256) known.clear();
    known[key] = same;
    return same;
}

std::optional<WorkingSpace> matchingWorkingSpace(const ColorProfile& profile) {
    for (WorkingSpace s : {WorkingSpace::SRGB, WorkingSpace::AdobeRGB, WorkingSpace::DisplayP3, WorkingSpace::ProPhoto})
        if (equivalentProfiles(profile, builtinProfile(s))) return s;
    return std::nullopt;
}

// ---- Transforms ----------------------------------------------------------------------------------------------------

const char* renderingIntentKey(RenderingIntent intent) {
    switch (intent) {
    case RenderingIntent::Perceptual: return "perceptual";
    case RenderingIntent::RelativeColorimetric: return "relative";
    case RenderingIntent::Saturation: return "saturation";
    case RenderingIntent::AbsoluteColorimetric: return "absolute";
    }
    return "relative";
}

std::optional<RenderingIntent> renderingIntentFromKey(const std::string& key) {
    for (RenderingIntent i : {RenderingIntent::Perceptual, RenderingIntent::RelativeColorimetric, RenderingIntent::Saturation, RenderingIntent::AbsoluteColorimetric})
        if (key == renderingIntentKey(i)) return i;
    return std::nullopt;
}

namespace {
cmsUInt32Number baseFlags(PixelFormat input, PixelFormat output) {
    cmsUInt32Number flags = cmsFLAGS_NOCACHE;   // no one-pixel cache: safe to apply from several threads
    if (input != PixelFormat::RGBFloat && output != PixelFormat::RGBFloat) flags |= cmsFLAGS_COPY_ALPHA;
    return flags;
}
} // namespace

ColorTransformPtr transformBetween(const ColorProfile& from, const ColorProfile& to, const ConvertOptions& options, PixelFormat input, PixelFormat output) {
    if ((input == PixelFormat::RGBFloat) != (output == PixelFormat::RGBFloat)) return nullptr;
    if (equivalentProfiles(from, to) && input == output) return nullptr;
    const ColorProfile& a = effectiveProfile(from);
    const ColorProfile& b = effectiveProfile(to);
    if (a.model != ColorModel::RGB || b.model != ColorModel::RGB) return nullptr;
    CacheKey key;
    key.from = fingerprint(a.icc); key.to = fingerprint(b.icc);
    key.intent = int(options.intent); key.flags = options.blackPointCompensation ? 1 : 0;
    key.input = int(input); key.output = int(output);
    if (auto hit = cached(key)) return hit;
    cmsContext context = cmsCreateContext(nullptr, nullptr);
    cmsHPROFILE ha = openProfile(context, a), hb = openProfile(context, b);
    cmsHTRANSFORM t = nullptr;
    if (ha && hb) {
        cmsUInt32Number flags = baseFlags(input, output);
        if (options.blackPointCompensation) flags |= cmsFLAGS_BLACKPOINTCOMPENSATION;
        t = cmsCreateTransformTHR(context, ha, lcmsFormat(input), hb, lcmsFormat(output), lcmsIntent(options.intent), flags);
    }
    if (ha) cmsCloseProfile(ha);
    if (hb) cmsCloseProfile(hb);
    if (!t) { cmsDeleteContext(context); return nullptr; }
    ColorTransformPtr made = TransformFactory::make(context, t, input, output);
    remember(key, made);
    return made;
}

ColorTransformPtr proofTransform(const ColorProfile& document, const ColorProfile& display, const ProofSettings& proof, PixelFormat input, PixelFormat output) {
    const ColorProfile& a = effectiveProfile(document);
    const ColorProfile& b = effectiveProfile(display);
    const ColorProfile& c = effectiveProfile(proof.profile);
    if (a.model != ColorModel::RGB || b.model != ColorModel::RGB) return nullptr;
    CacheKey key;
    key.from = fingerprint(a.icc); key.to = fingerprint(b.icc); key.proof = fingerprint(c.icc) | 1;
    key.intent = int(RenderingIntent::RelativeColorimetric); key.proofIntent = int(proof.intent);
    key.flags = (proof.blackPointCompensation ? 1 : 0) | (proof.gamutWarning ? 2 : 0);
    key.input = int(input); key.output = int(output);
    if (proof.gamutWarning) key.warning = {proof.warning[0], proof.warning[1], proof.warning[2]};
    if (auto hit = cached(key)) return hit;
    cmsContext context = cmsCreateContext(nullptr, nullptr);
    cmsHPROFILE ha = openProfile(context, a), hb = openProfile(context, b), hc = openProfile(context, c);
    cmsHTRANSFORM t = nullptr;
    if (ha && hb && hc) {
        cmsUInt32Number flags = baseFlags(input, output) | cmsFLAGS_SOFTPROOFING;
        if (proof.blackPointCompensation) flags |= cmsFLAGS_BLACKPOINTCOMPENSATION;
        if (proof.gamutWarning) {
            flags |= cmsFLAGS_GAMUTCHECK;
            cmsUInt16Number alarm[cmsMAXCHANNELS] = {};
            for (int i = 0; i < 3; i++) alarm[i] = cmsUInt16Number(proof.warning[i] * 257);
            cmsSetAlarmCodesTHR(context, alarm);
        }
        t = cmsCreateProofingTransformTHR(context, ha, lcmsFormat(input), hb, lcmsFormat(output), hc, lcmsIntent(proof.intent),
                                          INTENT_RELATIVE_COLORIMETRIC, flags);
    }
    for (cmsHPROFILE h : {ha, hb, hc}) if (h) cmsCloseProfile(h);
    if (!t) { cmsDeleteContext(context); return nullptr; }
    ColorTransformPtr made = TransformFactory::make(context, t, input, output);
    remember(key, made);
    return made;
}

void clearTransformCache() {
    std::lock_guard<std::mutex> lock(cacheMutex);
    cache().clear();
}

size_t transformCacheSize() {
    std::lock_guard<std::mutex> lock(cacheMutex);
    return cache().size();
}

// ---- Converting pixels and colours ---------------------------------------------------------------------------------

void convertImage(Image& image, const ColorTransform* transform) {
    if (!transform || image.isEmpty()) return;
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) transform->apply(image.row(y), image.row(y), size_t(image.width()));
    }, 16);
}

void convertImage(Image16& image, const ColorTransform* transform) {
    if (!transform || image.width() <= 0 || image.height() <= 0) return;
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) transform->apply(image.row(y), image.row(y), size_t(image.width()));
    }, 16);
}

bool convertImage(Image& image, const ColorProfile& from, const ColorProfile& to, const ConvertOptions& options) {
    if (equivalentProfiles(from, to)) return true;
    ColorTransformPtr t = transformBetween(from, to, options, PixelFormat::RGBA8, PixelFormat::RGBA8);
    if (!t) return false;
    convertImage(image, t.get());
    return true;
}

bool convertImage(Image16& image, const ColorProfile& from, const ColorProfile& to, const ConvertOptions& options) {
    if (equivalentProfiles(from, to)) return true;
    ColorTransformPtr t = transformBetween(from, to, options, PixelFormat::RGBA16, PixelFormat::RGBA16);
    if (!t) return false;
    convertImage(image, t.get());
    return true;
}

void convertImage16To8(const Image16& in, Image& out, const ColorTransform& transform) {
    if (out.width() != in.width() || out.height() != in.height()) out = Image(in.width(), in.height());
    parallelRows(0, in.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) transform.apply(in.row(y), out.row(y), size_t(in.width()));
    }, 16);
}

void convertColor(const ColorProfile& from, const ColorProfile& to, double rgb[3], const ConvertOptions& options) {
    ColorTransformPtr t = transformBetween(from, to, options, PixelFormat::RGBFloat, PixelFormat::RGBFloat);
    if (!t) return;
    float in[3] = {float(rgb[0]), float(rgb[1]), float(rgb[2])}, out[3] = {};
    t->apply(in, out, 1);
    for (int i = 0; i < 3; i++) rgb[i] = std::clamp(double(out[i]), 0.0, 1.0);
}

void convertColor(const ColorProfile& from, const ColorProfile& to, uint8_t rgb[3], const ConvertOptions& options) {
    double v[3] = {rgb[0] / 255.0, rgb[1] / 255.0, rgb[2] / 255.0};
    convertColor(from, to, v, options);
    for (int i = 0; i < 3; i++) rgb[i] = uint8_t(std::lround(v[i] * 255));
}

// ---- Transfer curves -----------------------------------------------------------------------------------------------

namespace {
constexpr int curveSamples = 4096;

float srgbToLinear(float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); }
float srgbFromLinear(float v) { return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f; }

float lookup(const std::vector<float>& table, float v) {
    const float x = std::clamp(v, 0.0f, 1.0f) * float(curveSamples);
    const int i = std::min(curveSamples - 1, int(x));
    const float f = x - float(i);
    return table[size_t(i)] + (table[size_t(i) + 1] - table[size_t(i)]) * f;
}
} // namespace

float TransferCurve::toLinear(float v) const {
    switch (kind_) {
    case Kind::SRGB: return srgbToLinear(std::clamp(v, 0.0f, 1.0f));
    case Kind::Gamma: return std::pow(std::clamp(v, 0.0f, 1.0f), float(gamma_));
    case Kind::Table: return lookup(*forward_, v);
    }
    return v;
}

float TransferCurve::fromLinear(float v) const {
    switch (kind_) {
    case Kind::SRGB: return srgbFromLinear(std::clamp(v, 0.0f, 1.0f));
    case Kind::Gamma: return std::pow(std::clamp(v, 0.0f, 1.0f), float(1 / gamma_));
    case Kind::Table: return lookup(*inverse_, v);
    }
    return v;
}

TransferCurve TransferCurve::srgb() { return TransferCurve(); }

TransferCurve TransferCurve::ofProfile(const ColorProfile& profile) {
    TransferCurve curve;
    if (auto space = matchingWorkingSpace(profile)) {
        switch (*space) {
        case WorkingSpace::SRGB: case WorkingSpace::DisplayP3: return curve;
        case WorkingSpace::AdobeRGB: curve.kind_ = Kind::Gamma; curve.gamma_ = 563.0 / 256.0; return curve;
        case WorkingSpace::ProPhoto: curve.kind_ = Kind::Gamma; curve.gamma_ = 1.8; return curve;
        }
    }
    if (profile.model != ColorModel::RGB && profile.model != ColorModel::Gray) return curve;
    cmsContext context = cmsCreateContext(nullptr, nullptr);
    cmsHPROFILE h = cmsOpenProfileFromMemTHR(context, profile.icc.data(), cmsUInt32Number(profile.icc.size()));
    if (h) {
        const auto* trc = static_cast<const cmsToneCurve*>(cmsReadTag(h, profile.model == ColorModel::Gray ? cmsSigGrayTRCTag : cmsSigRedTRCTag));
        cmsToneCurve* reverse = trc ? cmsReverseToneCurveEx(curveSamples + 1, trc) : nullptr;
        if (trc && reverse) {
            auto forward = std::make_shared<std::vector<float>>(size_t(curveSamples) + 1);
            auto inverse = std::make_shared<std::vector<float>>(size_t(curveSamples) + 1);
            for (int i = 0; i <= curveSamples; i++) {
                const float x = float(i) / float(curveSamples);
                (*forward)[size_t(i)] = std::clamp(cmsEvalToneCurveFloat(trc, x), 0.0f, 1.0f);
                (*inverse)[size_t(i)] = std::clamp(cmsEvalToneCurveFloat(reverse, x), 0.0f, 1.0f);
            }
            const double g = cmsEstimateGamma(trc, 0.01);
            curve.kind_ = Kind::Table;
            curve.gamma_ = g > 0 ? g : 2.2;
            curve.forward_ = forward;
            curve.inverse_ = inverse;
        }
        if (reverse) cmsFreeToneCurve(reverse);
        cmsCloseProfile(h);
    }
    cmsDeleteContext(context);
    return curve;
}

TransferCurve documentTransfer(const Document& document) { return TransferCurve::ofProfile(document.profile); }

} // namespace compositor
