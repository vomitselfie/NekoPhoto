// Colour management through Little CMS (colormgmt.h, docs/color-management.md).
#include "compositor/colormgmt.h"
#include "compositor/depth.h"
#include "compositor/document.h"
#include "compositor/parallel.h"
#include "compositor/view32.h"
#include "lcms2.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <list>
#include <map>
#include <mutex>

namespace compositor {

namespace blobs {
extern const unsigned char defaultCmykProfile[];
extern const unsigned long long defaultCmykProfileSize;
} // namespace blobs

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

/// Whether a transform between these layouts stages its pixels itself (ColorTransform::applyStaged): any CMYK or Lab
/// pixel layout. RGBA to RGBA keeps Little CMS's alpha-carrying layouts, as before CMYK and Lab existed.
bool stagedLayouts(PixelFormat input, PixelFormat output) {
    if (isColorFormat(input) || isColorFormat(output)) return false;
    auto rgba = [](PixelFormat f) { return f == PixelFormat::RGBA8 || f == PixelFormat::RGBA16 || f == PixelFormat::RGBAFloat; };
    return !(rgba(input) && rgba(output));
}

bool wideFormat(PixelFormat f) { return f == PixelFormat::RGBA16 || f == PixelFormat::CMYKA16 || f == PixelFormat::LabA16; }

/// Little CMS's layout for `f`. Staged transforms see colours only, straight: bytes for the 8-bit layouts (reversed
/// CMYK for inverted ink, Little CMS's 8-bit Lab, which is the same 128 offset), floats for the 16-bit ones (CMYK ink
/// 0..100, Lab L 0..100 and signed a and b).
cmsUInt32Number lcmsFormat(PixelFormat f, bool staged = false) {
    switch (f) {
    case PixelFormat::RGBA8: return staged ? TYPE_RGB_8 : TYPE_RGBA_8;
    case PixelFormat::RGBA16: return staged ? TYPE_RGB_FLT : TYPE_RGBA_FLT;   // through the float pipeline: exact for 15-bit levels
    case PixelFormat::RGBFloat: return TYPE_RGB_FLT;
    case PixelFormat::RGBAFloat: return TYPE_RGBA_FLT;   // made straight a chunk at a time (apply)
    case PixelFormat::CMYKA8: return TYPE_CMYK_8_REV;
    case PixelFormat::CMYKA16: case PixelFormat::CMYKFloat: return TYPE_CMYK_FLT;
    case PixelFormat::LabA8: return TYPE_Lab_8;
    case PixelFormat::LabA16: case PixelFormat::LabFloat: return TYPE_Lab_FLT;
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
        t->staged_ = stagedLayouts(input, output);
        return t;
    }
};

ColorTransform::~ColorTransform() {
    if (transform_) cmsDeleteTransform(static_cast<cmsHTRANSFORM>(transform_));
    if (context_) cmsDeleteContext(static_cast<cmsContext>(context_));
}

void ColorTransform::applyStaged(const void* in, void* out, size_t count) const {
    // CMYK and Lab layouts (and RGBA beside them): the colours made straight into Little CMS's own units a chunk at a
    // time, converted, and premultiplied again with the input's alpha brought to the output's depth. Everything a
    // chunk reads is staged before anything is written, so `out` may be `in` when the layouts have the same size.
    auto* t = static_cast<cmsHTRANSFORM>(transform_);
    const size_t inColors = size_t(pixelFormatChannels(input_) - 1), outColors = size_t(pixelFormatChannels(output_) - 1);
    const bool inWide = wideFormat(input_), outWide = wideFormat(output_);
    const ColorModel inModel = pixelFormatModel(input_), outModel = pixelFormatModel(output_);
    constexpr size_t chunk = 1024;
    thread_local std::vector<uint8_t> in8, out8;
    thread_local std::vector<float> inF, outF;
    thread_local std::vector<uint32_t> alphas;
    in8.resize(chunk * 4); out8.resize(chunk * 4); inF.resize(chunk * 4); outF.resize(chunk * 4); alphas.resize(chunk);
    constexpr float unit = 1.0f / float(one16);
    for (size_t done = 0; done < count; done += chunk) {
        const size_t n = std::min(chunk, count - done);
        const uint8_t* src8 = inWide ? nullptr : static_cast<const uint8_t*>(in) + done * (inColors + 1);
        const uint16_t* src16 = inWide ? static_cast<const uint16_t*>(in) + done * (inColors + 1) : nullptr;
        for (size_t i = 0; i < n; i++) {
            if (!inWide) {
                const uint8_t* p = src8 + i * (inColors + 1);
                const unsigned a = p[inColors];
                alphas[i] = outWide ? widen8(uint8_t(a)) : a;
                for (size_t c = 0; c < inColors; c++)
                    in8[i * inColors + c] = a == 255 ? p[c] : a == 0 ? 0 : uint8_t(std::min(255u, (p[c] * 255u + a / 2) / a));
            } else {
                const uint16_t* p = src16 + i * (inColors + 1);
                const uint32_t a = std::min<uint32_t>(p[inColors], one16);
                alphas[i] = outWide ? a : narrow16(a);
                for (size_t c = 0; c < inColors; c++) {
                    const float s = a >= one16 ? float(p[c]) * unit : a == 0 ? 0.0f : std::min(1.0f, float(p[c]) / float(a));
                    float v = s;
                    if (inModel == ColorModel::CMYK) v = (1.0f - s) * 100.0f;
                    else if (inModel == ColorModel::Lab) v = c == 0 ? s * 100.0f : (s * float(one16) - 16384.0f) / 128.0f;
                    inF[i * inColors + c] = v;
                }
            }
        }
        cmsDoTransform(t, inWide ? static_cast<const void*>(inF.data()) : in8.data(), outWide ? static_cast<void*>(outF.data()) : out8.data(), cmsUInt32Number(n));
        if (!outWide) {
            uint8_t* dst = static_cast<uint8_t*>(out) + done * (outColors + 1);
            for (size_t i = 0; i < n; i++, dst += outColors + 1) {
                const unsigned a = alphas[i];
                for (size_t c = 0; c < outColors; c++) {
                    const unsigned v = out8[i * outColors + c];
                    dst[c] = a == 255 ? uint8_t(v) : uint8_t((v * a + 127) / 255);
                }
                dst[outColors] = uint8_t(a);
            }
        } else {
            uint16_t* dst = static_cast<uint16_t*>(out) + done * (outColors + 1);
            for (size_t i = 0; i < n; i++, dst += outColors + 1) {
                const uint32_t a = alphas[i];
                for (size_t c = 0; c < outColors; c++) {
                    const float f = outF[i * outColors + c];
                    float s = f;
                    if (outModel == ColorModel::CMYK) s = 1.0f - f / 100.0f;
                    else if (outModel == ColorModel::Lab) s = c == 0 ? f / 100.0f : (f * 128.0f + 16384.0f) / float(one16);
                    dst[c] = uint16_t(std::lround(std::clamp(s, 0.0f, 1.0f) * float(a)));
                }
                dst[outColors] = uint16_t(a);
            }
        }
    }
}

void ColorTransform::apply(const void* in, void* out, size_t count) const {
    auto* t = static_cast<cmsHTRANSFORM>(transform_);
    if (isColorFormat(input_)) { cmsDoTransform(t, in, out, cmsUInt32Number(count)); return; }
    if (staged_) { applyStaged(in, out, count); return; }
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
        const float* srcF = input_ == PixelFormat::RGBAFloat ? static_cast<const float*>(in) + done * 4 : nullptr;
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
        } else if (srcF) {
            // Linear float: straight, clipped to what Little CMS's display pipeline takes (the tone mapping is done).
            for (size_t i = 0; i < n * 4; i += 4) {
                const float a = std::clamp(srcF[i + 3], 0.0f, 1.0f);
                opaque &= a >= 1;
                wide[i + 3] = a;
                for (size_t c = 0; c < 3; c++) wide[i + c] = a <= 0 ? 0.0f : std::clamp(srcF[i + c] / a, 0.0f, 1.0f);
            }
            staged = wide.data();
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

/// A profile's bytes made the same in every session: a fixed creation date (2026-01-01), then the profile ID computed
/// over it.
void stabilise(cmsContext context, std::vector<uint8_t>& bytes) {
    if (bytes.size() < 128) return;
    const uint16_t date[6] = {2026, 1, 1, 0, 0, 0};
    for (int i = 0; i < 6; i++) { bytes[size_t(24 + i * 2)] = uint8_t(date[i] >> 8); bytes[size_t(25 + i * 2)] = uint8_t(date[i]); }
    if (cmsHPROFILE again = cmsOpenProfileFromMemTHR(context, bytes.data(), cmsUInt32Number(bytes.size()))) {
        cmsMD5computeID(again);
        std::vector<uint8_t> resaved = saveProfile(again);
        if (!resaved.empty()) bytes = std::move(resaved);
        cmsCloseProfile(again);
    }
}

std::vector<uint8_t> makeLab() {
    cmsContext context = cmsCreateContext(nullptr, nullptr);
    std::vector<uint8_t> bytes;
    if (cmsHPROFILE h = cmsCreateLab4ProfileTHR(context, cmsD50_xyY())) {
        cmsMLU* description = cmsMLUalloc(context, 1);
        cmsMLUsetASCII(description, "en", "US", "Lab D50");
        cmsWriteTag(h, cmsSigProfileDescriptionTag, description);
        cmsMLUfree(description);
        cmsMLU* copyright = cmsMLUalloc(context, 1);
        cmsMLUsetASCII(copyright, "en", "US", "No copyright, use freely");
        cmsWriteTag(h, cmsSigCopyrightTag, copyright);
        cmsMLUfree(copyright);
        bytes = saveProfile(h);
        cmsCloseProfile(h);
    }
    stabilise(context, bytes);
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

const ColorProfile& effectiveProfile(const ColorProfile& profile, ColorModel model) {
    if (!profile.empty()) return profile;
    if (model == ColorModel::CMYK) return defaultCmykProfile();
    if (model == ColorModel::Lab) return labProfile();
    return srgbProfile();
}

const ColorProfile& labProfile() {
    static const ColorProfile profile = [] {
        ColorProfile p;
        p.icc = makeLab();
        p.description = "Lab D50";
        p.model = ColorModel::Lab;
        return p;
    }();
    return profile;
}

const ColorProfile& defaultCmykProfile() {
    static const ColorProfile profile = [] {
        auto p = profileFromIcc(blobs::defaultCmykProfile, size_t(blobs::defaultCmykProfileSize));
        return p ? *p : ColorProfile{};
    }();
    return profile;
}

PixelFormat pixelFormatFor(SampleType type, ColorMode mode) {
    if (type == SampleType::F32 && mode == ColorMode::RGB) return PixelFormat::RGBAFloat;
    const bool wide = type != SampleType::U8;
    switch (mode) {
    case ColorMode::CMYK: return wide ? PixelFormat::CMYKA16 : PixelFormat::CMYKA8;
    case ColorMode::Lab: return wide ? PixelFormat::LabA16 : PixelFormat::LabA8;
    case ColorMode::RGB: break;
    }
    return wide ? PixelFormat::RGBA16 : PixelFormat::RGBA8;
}

ColorModel pixelFormatModel(PixelFormat format) {
    switch (format) {
    case PixelFormat::CMYKA8: case PixelFormat::CMYKA16: case PixelFormat::CMYKFloat: return ColorModel::CMYK;
    case PixelFormat::LabA8: case PixelFormat::LabA16: case PixelFormat::LabFloat: return ColorModel::Lab;
    case PixelFormat::RGBA8: case PixelFormat::RGBA16: case PixelFormat::RGBFloat: case PixelFormat::RGBAFloat: break;
    }
    return ColorModel::RGB;
}

int pixelFormatChannels(PixelFormat format) {
    switch (format) {
    case PixelFormat::CMYKA8: case PixelFormat::CMYKA16: return 5;
    case PixelFormat::CMYKFloat: return 4;
    case PixelFormat::RGBFloat: case PixelFormat::LabFloat: return 3;
    case PixelFormat::RGBA8: case PixelFormat::RGBA16: case PixelFormat::LabA8: case PixelFormat::LabA16: case PixelFormat::RGBAFloat: break;
    }
    return 4;
}

bool isColorFormat(PixelFormat format) {
    return format == PixelFormat::RGBFloat || format == PixelFormat::CMYKFloat || format == PixelFormat::LabFloat;
}

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
    // Alpha rides along in Little CMS's RGBA layouts; staged layouts hand it colours only.
    if (!isColorFormat(input) && !isColorFormat(output) && !stagedLayouts(input, output)) flags |= cmsFLAGS_COPY_ALPHA;
    return flags;
}
} // namespace

ColorTransformPtr transformBetween(const ColorProfile& from, const ColorProfile& to, const ConvertOptions& options, PixelFormat input, PixelFormat output) {
    if (isColorFormat(input) != isColorFormat(output)) return nullptr;
    // Float pixels go to the screen only (toDisplayF): RGBAFloat to RGBA8.
    if (output == PixelFormat::RGBAFloat || (input == PixelFormat::RGBAFloat && output != PixelFormat::RGBA8)) return nullptr;
    const ColorProfile& a = effectiveProfile(from, pixelFormatModel(input));
    const ColorProfile& b = effectiveProfile(to, pixelFormatModel(output));
    if (a.model != pixelFormatModel(input) || b.model != pixelFormatModel(output)) return nullptr;
    if (input == output && equivalentProfiles(a, b)) return nullptr;
    const bool staged = stagedLayouts(input, output);
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
        t = cmsCreateTransformTHR(context, ha, lcmsFormat(input, staged), hb, lcmsFormat(output, staged), lcmsIntent(options.intent), flags);
    }
    if (ha) cmsCloseProfile(ha);
    if (hb) cmsCloseProfile(hb);
    if (!t) { cmsDeleteContext(context); return nullptr; }
    ColorTransformPtr made = TransformFactory::make(context, t, input, output);
    remember(key, made);
    return made;
}

ColorTransformPtr lookupTransform(const ColorProfile& document, const std::vector<uint8_t>& lookup, PixelFormat format) {
    if (isColorFormat(format) || format == PixelFormat::RGBAFloat) return nullptr;
    const ColorModel model = pixelFormatModel(format);
    const ColorProfile& a = effectiveProfile(document, model);
    if (a.model != model) return nullptr;
    const bool staged = stagedLayouts(format, format);
    CacheKey key;
    key.from = fingerprint(a.icc); key.to = key.from; key.proof = fingerprint(lookup) | 2;
    key.intent = int(RenderingIntent::Perceptual);
    key.input = int(format); key.output = int(format);
    if (auto hit = cached(key)) return hit;
    cmsContext context = cmsCreateContext(nullptr, nullptr);
    cmsHPROFILE doc = openProfile(context, a);
    cmsHPROFILE link = cmsOpenProfileFromMemTHR(context, lookup.data(), cmsUInt32Number(lookup.size()));
    cmsHTRANSFORM t = nullptr;
    if (doc && link) {
        const cmsColorSpaceSignature space = model == ColorModel::CMYK ? cmsSigCmykData : model == ColorModel::Lab ? cmsSigLabData : cmsSigRgbData;
        const cmsProfileClassSignature cls = cmsGetDeviceClass(link);
        if (cls == cmsSigAbstractClass) {
            // An abstract profile works on the profile connection space: the document's values through it and back.
            cmsHPROFILE chain[3] = {doc, link, doc};
            t = cmsCreateMultiprofileTransformTHR(context, chain, 3, lcmsFormat(format, staged), lcmsFormat(format, staged), INTENT_PERCEPTUAL, baseFlags(format, format));
        } else if (cls == cmsSigLinkClass && cmsGetColorSpace(link) == space && cmsGetPCS(link) == space) {
            // A device link from the document's colour space to itself (a CMYK to CMYK link, say), applied as it is.
            t = cmsCreateTransformTHR(context, link, lcmsFormat(format, staged), nullptr, lcmsFormat(format, staged), INTENT_PERCEPTUAL, baseFlags(format, format));
        }
    }
    if (doc) cmsCloseProfile(doc);
    if (link) cmsCloseProfile(link);
    if (!t) { cmsDeleteContext(context); return nullptr; }
    ColorTransformPtr made = TransformFactory::make(context, t, format, format);
    remember(key, made);
    return made;
}

ColorTransformPtr proofTransform(const ColorProfile& document, const ColorProfile& display, const ProofSettings& proof, PixelFormat input, PixelFormat output) {
    if (isColorFormat(input) != isColorFormat(output)) return nullptr;
    if (output == PixelFormat::RGBAFloat || (input == PixelFormat::RGBAFloat && output != PixelFormat::RGBA8)) return nullptr;
    const ColorProfile& a = effectiveProfile(document, pixelFormatModel(input));
    const ColorProfile& b = effectiveProfile(display, pixelFormatModel(output));
    const ColorProfile& c = effectiveProfile(proof.profile);   // any model: an RGB device, or a press (Working CMYK)
    if (a.model != pixelFormatModel(input) || b.model != pixelFormatModel(output)) return nullptr;
    if (c.model != ColorModel::RGB && c.model != ColorModel::CMYK && c.model != ColorModel::Gray) return nullptr;
    const bool staged = stagedLayouts(input, output);
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
        t = cmsCreateProofingTransformTHR(context, ha, lcmsFormat(input, staged), hb, lcmsFormat(output, staged), hc, lcmsIntent(proof.intent),
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
    if (pixelFormatChannels(transform->input()) != image.channels() || pixelFormatChannels(transform->output()) != image.channels()) return;
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) transform->apply(image.row(y), image.row(y), size_t(image.width()));
    }, 16);
}

void convertImage(ImageC8& image, const ColorTransform* transform) {
    if (!transform || image.isEmpty() || transform->input() != PixelFormat::CMYKA8 || transform->output() != PixelFormat::CMYKA8) return;
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) transform->apply(image.row(y), image.row(y), size_t(image.width()));
    }, 16);
}

namespace {
/// The layout a buffer holds, when it is `mode`'s at its own depth.
std::optional<PixelFormat> layoutOf(const AnyImage& image, ColorMode mode) {
    if (!image || image.sampleType() == SampleType::F32 || image.channels() != colorModeChannels(mode)) return std::nullopt;
    if (image.sampleType() == SampleType::U8 && (mode == ColorMode::CMYK) != bool(image.c8())) return std::nullopt;
    return pixelFormatFor(image.sampleType(), mode);
}

template <class In, class Out>
void convertRows(const In& in, Out& out, const ColorTransform& transform) {
    parallelRows(0, in.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) transform.apply(in.row(y), out.row(y), size_t(in.width()));
    }, 16);
}
} // namespace

AnyImage convertImage(const AnyImage& image, ColorMode fromMode, const ColorProfile& from, ColorMode toMode, const ColorProfile& to, const ConvertOptions& options) {
    const auto input = layoutOf(image, fromMode);
    if (!input) return nullptr;
    const PixelFormat output = pixelFormatFor(image.sampleType(), toMode);
    ColorTransformPtr t = transformBetween(from, to, options, *input, output);
    if (!t) {
        // Nothing to do when both are the same colours in the same layout; otherwise a profile could not be used.
        if (*input == output && equivalentProfiles(effectiveProfile(from, pixelFormatModel(output)), effectiveProfile(to, pixelFormatModel(output)))) return image;
        return nullptr;
    }
    const int w = image.width(), h = image.height();
    if (image.sampleType() == SampleType::U16) {
        auto out = std::make_shared<Image16>(w, h, colorModeChannels(toMode));
        convertRows(*image.u16(), *out, *t);
        return Image16Ptr(out);
    }
    auto source = [&](auto&& f) { if (image.c8()) f(*image.c8()); else f(*image.u8()); };
    if (toMode == ColorMode::CMYK) {
        auto out = std::make_shared<ImageC8>(w, h, 5);
        source([&](const auto& in) { convertRows(in, *out, *t); });
        return ImageC8Ptr(out);
    }
    auto out = std::make_shared<Image>(w, h);
    source([&](const auto& in) { convertRows(in, *out, *t); });
    return ImagePtr(out);
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

bool convertImageTo8(const AnyImage& in, Image& out, const ColorTransform& transform) {
    const PixelFormat f = transform.input();
    if (!in || transform.output() != PixelFormat::RGBA8 || isColorFormat(f) || pixelFormatChannels(f) != in.channels()) return false;
    const bool wide = f == PixelFormat::RGBA16 || f == PixelFormat::CMYKA16 || f == PixelFormat::LabA16;
    if (wide != (in.sampleType() == SampleType::U16) || (f == PixelFormat::CMYKA8) != bool(in.c8())) return false;
    if (out.width() != in.width() || out.height() != in.height()) out = Image(in.width(), in.height());
    if (in.u16()) convertRows(*in.u16(), out, transform);
    else if (in.c8()) convertRows(*in.c8(), out, transform);
    else if (in.u8()) convertRows(*in.u8(), out, transform);
    else return false;
    return true;
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

float TransferCurve::fromLinearExact(float v) const {
    if (kind_ != Kind::Table) return fromLinear(v);
    // The forward table's interpolation inverted: the segment that holds v, then the fraction along it.
    const std::vector<float>& f = *forward_;
    v = std::clamp(v, 0.0f, 1.0f);
    if (v <= f.front()) return 0;
    if (v >= f.back()) return 1;
    const size_t i = size_t(std::upper_bound(f.begin(), f.end(), v) - f.begin()) - 1;
    const float d = f[i + 1] - f[i];
    const float fraction = d > 0 ? (v - f[i]) / d : 0;
    return (float(i) + fraction) / float(curveSamples);
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

// ---- 32 bits: linear profiles and the display ----------------------------------------------------------------------

namespace {

constexpr cmsTagSignature trcTags[3] = {cmsSigRedTRCTag, cmsSigGreenTRCTag, cmsSigBlueTRCTag};
constexpr cmsTagSignature colorantTags[3] = {cmsSigRedColorantTag, cmsSigGreenColorantTag, cmsSigBlueColorantTag};

bool matrixShaperRgb(cmsHPROFILE h) { return h && cmsGetColorSpace(h) == cmsSigRgbData && cmsIsMatrixShaper(h); }

bool linearCurves(cmsHPROFILE h) {
    if (!matrixShaperRgb(h)) return false;
    for (cmsTagSignature sig : trcTags) {
        const auto* curve = static_cast<const cmsToneCurve*>(cmsReadTag(h, sig));
        if (!curve || !cmsIsToneCurveLinear(curve)) return false;
    }
    return true;
}

/// The profile with its three tone curves replaced by `make`'s and a new description; the lookup-table tags, which
/// Little CMS would prefer to the matrix and curves, are dropped. Empty when it is not an RGB matrix-shaper.
ColorProfile withCurves(const ColorProfile& profile, cmsToneCurve* (*make)(cmsContext), const std::string& description) {
    cmsContext context = cmsCreateContext(nullptr, nullptr);
    std::vector<uint8_t> bytes;
    if (cmsHPROFILE h = cmsOpenProfileFromMemTHR(context, profile.icc.data(), cmsUInt32Number(profile.icc.size()))) {
        if (matrixShaperRgb(h)) {
            bool ok = true;
            for (cmsTagSignature sig : trcTags) {
                cmsToneCurve* curve = make(context);
                ok = ok && curve && cmsWriteTag(h, sig, curve);
                if (curve) cmsFreeToneCurve(curve);
            }
            for (cmsTagSignature sig : {cmsSigAToB0Tag, cmsSigAToB1Tag, cmsSigAToB2Tag, cmsSigBToA0Tag, cmsSigBToA1Tag, cmsSigBToA2Tag,
                                        cmsSigDToB0Tag, cmsSigDToB1Tag, cmsSigDToB2Tag, cmsSigDToB3Tag, cmsSigBToD0Tag, cmsSigBToD1Tag,
                                        cmsSigBToD2Tag, cmsSigBToD3Tag, cmsSigProfileDescriptionMLTag})
                if (cmsIsTag(h, sig)) cmsWriteTag(h, sig, nullptr);
            cmsMLU* text = cmsMLUalloc(context, 1);
            cmsMLUsetASCII(text, "en", "US", description.c_str());
            ok = ok && cmsWriteTag(h, cmsSigProfileDescriptionTag, text);
            cmsMLUfree(text);
            if (ok) bytes = saveProfile(h);
        }
        cmsCloseProfile(h);
    }
    if (!bytes.empty()) stabilise(context, bytes);
    cmsDeleteContext(context);
    if (auto made = profileFromIcc(bytes)) return *made;
    return {};
}

cmsToneCurve* linearCurve(cmsContext context) { return cmsBuildGamma(context, 1.0); }
cmsToneCurve* srgbToneCurve(cmsContext context) {
    const double parameters[5] = {2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92, 0.04045};
    return cmsBuildParametricToneCurve(context, 4, parameters);
}

/// The primaries (their colorant XYZ) and white point of an RGB matrix-shaper, for comparing linear profiles.
std::optional<std::array<double, 12>> primariesOf(const ColorProfile& profile) {
    cmsContext context = cmsCreateContext(nullptr, nullptr);
    std::optional<std::array<double, 12>> out;
    if (cmsHPROFILE h = cmsOpenProfileFromMemTHR(context, profile.icc.data(), cmsUInt32Number(profile.icc.size()))) {
        if (matrixShaperRgb(h)) {
            std::array<double, 12> v{};
            bool ok = true;
            for (size_t i = 0; i < 3 && ok; i++) {
                const auto* xyz = static_cast<const cmsCIEXYZ*>(cmsReadTag(h, colorantTags[i]));
                ok = xyz != nullptr;
                if (ok) { v[i * 3] = xyz->X; v[i * 3 + 1] = xyz->Y; v[i * 3 + 2] = xyz->Z; }
            }
            if (const auto* white = static_cast<const cmsCIEXYZ*>(cmsReadTag(h, cmsSigMediaWhitePointTag))) { v[9] = white->X; v[10] = white->Y; v[11] = white->Z; }
            if (ok) out = v;
        }
        cmsCloseProfile(h);
    }
    cmsDeleteContext(context);
    return out;
}

std::mutex linearMutex;
std::map<uint64_t, ColorProfile>& linearCache() { static std::map<uint64_t, ColorProfile> cache; return cache; }
std::map<uint64_t, bool>& linearityCache() { static std::map<uint64_t, bool> cache; return cache; }
std::map<uint64_t, std::array<float, 3>>& weightsCache() { static std::map<uint64_t, std::array<float, 3>> cache; return cache; }

std::string withoutSuffix(const std::string& text, const std::string& suffix) {
    return text.size() > suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0 ? text.substr(0, text.size() - suffix.size()) : text;
}

} // namespace

bool isLinearProfile(const ColorProfile& profile) {
    const ColorProfile& p = effectiveProfile(profile);
    if (p.model != ColorModel::RGB) return false;
    const uint64_t key = fingerprint(p.icc);
    {
        std::lock_guard<std::mutex> lock(linearMutex);
        if (auto it = linearityCache().find(key); it != linearityCache().end()) return it->second;
    }
    cmsContext context = cmsCreateContext(nullptr, nullptr);
    bool linear = false;
    if (cmsHPROFILE h = cmsOpenProfileFromMemTHR(context, p.icc.data(), cmsUInt32Number(p.icc.size()))) {
        linear = linearCurves(h);
        cmsCloseProfile(h);
    }
    cmsDeleteContext(context);
    std::lock_guard<std::mutex> lock(linearMutex);
    if (linearityCache().size() > 64) linearityCache().clear();
    linearityCache()[key] = linear;
    return linear;
}

ColorProfile linearProfile(const ColorProfile& profile) {
    const ColorProfile& p = effectiveProfile(profile);
    if (p.model == ColorModel::RGB && isLinearProfile(p)) return p;
    const uint64_t key = fingerprint(p.icc);
    {
        std::lock_guard<std::mutex> lock(linearMutex);
        if (auto it = linearCache().find(key); it != linearCache().end()) return it->second;
    }
    ColorProfile made = p.model == ColorModel::RGB ? withCurves(p, linearCurve, p.description + " (Linear)") : ColorProfile{};
    // Not a matrix-shaper (a lookup-table profile, say): linear sRGB.
    if (made.empty() && &p != &srgbProfile()) made = linearProfile(srgbProfile());
    std::lock_guard<std::mutex> lock(linearMutex);
    if (linearCache().size() > 64) linearCache().clear();
    linearCache()[key] = made;
    return made;
}

ColorProfile gammaCounterpart(const ColorProfile& profile) {
    if (!isLinearProfile(profile)) return profile;
    for (WorkingSpace s : {WorkingSpace::SRGB, WorkingSpace::AdobeRGB, WorkingSpace::DisplayP3, WorkingSpace::ProPhoto})
        if (linearProfile(builtinProfile(s)).icc == profile.icc) return builtinProfile(s);
    // A linear profile made elsewhere (a 32-bit file's): the working space with the same primaries and white.
    if (auto primaries = primariesOf(profile))
        for (WorkingSpace s : {WorkingSpace::SRGB, WorkingSpace::AdobeRGB, WorkingSpace::DisplayP3, WorkingSpace::ProPhoto}) {
            auto other = primariesOf(builtinProfile(s));
            if (!other) continue;
            bool same = true;
            for (size_t i = 0; i < 12 && same; i++) same = std::fabs((*primaries)[i] - (*other)[i]) < 2e-3;
            if (same) return builtinProfile(s);
        }
    ColorProfile made = withCurves(profile, srgbToneCurve, withoutSuffix(profile.description, " (Linear)"));
    return made.empty() ? srgbProfile() : made;
}

ColorProfile encodedProfileOf(const Document& document) {
    if (document.sampleType != SampleType::F32) return document.profile;
    if (document.encodedProfile) return *document.encodedProfile;
    return gammaCounterpart(effectiveProfile(document.profile));
}

TransferCurve encodedTransfer(const Document& document) { return TransferCurve::ofProfile(encodedProfileOf(document)); }

std::array<float, 3> luminanceWeights(const ColorProfile& profile) {
    const ColorProfile& p = effectiveProfile(profile);
    const uint64_t key = fingerprint(p.icc);
    {
        std::lock_guard<std::mutex> lock(linearMutex);
        if (auto it = weightsCache().find(key); it != weightsCache().end()) return it->second;
    }
    std::array<float, 3> weights{0.2126f, 0.7152f, 0.0722f};
    if (auto primaries = primariesOf(p)) {
        const double sum = (*primaries)[1] + (*primaries)[4] + (*primaries)[7];
        if (sum > 0) for (size_t i = 0; i < 3; i++) weights[i] = float((*primaries)[i * 3 + 1] / sum);
    }
    std::lock_guard<std::mutex> lock(linearMutex);
    if (weightsCache().size() > 64) weightsCache().clear();
    weightsCache()[key] = weights;
    return weights;
}

void toDisplayF(const ImageF& in, Image& out, const ToneMap& tone, const ColorTransform* transform, const TransferCurve& curve) {
    if (out.width() != in.width() || out.height() != in.height()) out = Image(in.width(), in.height());
    const int w = in.width();
    if (transform && transform->input() == PixelFormat::RGBAFloat && transform->output() == PixelFormat::RGBA8) {
        parallelRows(0, in.height(), [&](int ya, int yb) {
            std::vector<float> row(size_t(w) * 4);
            for (int y = ya; y < yb; y++) {
                const float* s = in.row(y);
                for (int x = 0; x < w; x++) {
                    const float a = cleanCoverage(s[x * 4 + 3]);
                    float c[3] = {0, 0, 0};
                    if (a > 0) for (int k = 0; k < 3; k++) c[k] = cleanColour(s[x * 4 + k]) / a;
                    tone.apply(c);
                    for (int k = 0; k < 3; k++) row[size_t(x) * 4 + size_t(k)] = std::clamp(c[k], 0.0f, 1.0f) * a;
                    row[size_t(x) * 4 + 3] = a;
                }
                transform->apply(row.data(), out.row(y), size_t(w));
            }
        }, 16);
        return;
    }
    // Without a transform the curve encodes, from a table over linear 0..1: the display's 8 bits (the mode
    // conversion uses the curve's exact inverse instead).
    constexpr int steps = 65535;
    std::vector<float> encoded(size_t(steps) + 1);
    for (int i = 0; i <= steps; i++) encoded[size_t(i)] = curve.fromLinearExact(float(i) / float(steps));
    parallelRows(0, in.height(), [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            const float* s = in.row(y);
            uint8_t* d = out.row(y);
            for (int x = 0; x < w; x++, s += 4, d += 4) {
                const float a = cleanCoverage(s[3]);
                const long A = std::lround(a * 255);
                d[3] = uint8_t(A);
                if (A == 0) { d[0] = d[1] = d[2] = 0; continue; }
                float c[3];
                for (int k = 0; k < 3; k++) c[k] = cleanColour(s[k]) / a;
                tone.apply(c);
                for (int k = 0; k < 3; k++) {
                    const float e = encoded[size_t(std::lround(std::clamp(c[k], 0.0f, 1.0f) * float(steps)))];
                    d[k] = uint8_t(std::min(A, std::lround(double(e) * double(A))));
                }
            }
        }
    }, 16);
}

float peakLuminance(const ImageF& image, const std::array<float, 3>& weights) {
    float peak = 0;
    for (int y = 0; y < image.height(); y++) {
        const float* p = image.row(y);
        for (int x = 0; x < image.width(); x++, p += 4) {
            const float a = cleanCoverage(p[3]);
            if (a <= 0) continue;
            const float l = (weights[0] * cleanColour(p[0]) + weights[1] * cleanColour(p[1]) + weights[2] * cleanColour(p[2])) / a;
            peak = std::max(peak, l);
        }
    }
    return peak;
}

} // namespace compositor
