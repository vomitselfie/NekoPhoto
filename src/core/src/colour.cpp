#include "compositor/colour.h"
#include "lcms2.h"

namespace compositor {

std::shared_ptr<const CmykToSrgb> CmykToSrgb::fromProfile(const std::vector<uint8_t>& profile) {
    if (profile.size() < 128) return nullptr;
    std::shared_ptr<CmykToSrgb> t(new CmykToSrgb());
    cmsContext context = cmsCreateContext(nullptr, nullptr);
    if (!context) return nullptr;
    t->context_ = context;
    cmsHPROFILE cmyk = cmsOpenProfileFromMemTHR(context, profile.data(), cmsUInt32Number(profile.size()));
    if (!cmyk) return nullptr;
    if (cmsGetColorSpace(cmyk) != cmsSigCmykData) { cmsCloseProfile(cmyk); return nullptr; }
    cmsHPROFILE srgb = cmsCreate_sRGBProfileTHR(context);
    // PSD's CMYK is inverted (255 = no ink): the _REV layout reads it as is.
    t->transform_ = cmsCreateTransformTHR(context, cmyk, TYPE_CMYK_8_REV, srgb, TYPE_RGB_8, INTENT_RELATIVE_COLORIMETRIC,
                                          cmsFLAGS_BLACKPOINTCOMPENSATION | cmsFLAGS_NOCACHE);
    cmsCloseProfile(cmyk);
    cmsCloseProfile(srgb);
    if (!t->transform_) return nullptr;
    return t;
}

CmykToSrgb::~CmykToSrgb() {
    if (transform_) cmsDeleteTransform(static_cast<cmsHTRANSFORM>(transform_));
    if (context_) cmsDeleteContext(static_cast<cmsContext>(context_));
}

void CmykToSrgb::convert(const uint8_t* cmyk, uint8_t* rgb, size_t count) const {
    cmsDoTransform(static_cast<cmsHTRANSFORM>(transform_), cmyk, rgb, cmsUInt32Number(count));
}

} // namespace compositor
