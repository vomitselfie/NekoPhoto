// What the 8-bit Smart Filter kernels (smartfilter_render.cpp) share with the 16-bit ones
// (smartfilter_render16.cpp): the Gaussian line plans with their Photoshop calibrations, the parameter checks and
// Add Noise's position hash and Radial Blur's sample points. Internal to the core.
#pragma once
#include "compositor/smartfilter.h"
#include <cmath>
#include <cstdint>
#include <vector>

namespace compositor::smartfilter_detail {

struct GaussianLinePlan {
    bool direct = false;
    std::vector<double> kernel;
    double gain = 1.0;
    double coefficient1 = 0.0;
    double coefficient2 = 0.0;
    double coefficient3 = 0.0;
};

GaussianLinePlan makeGaussianLinePlan(double radius, int margin);
GaussianLinePlan makeHighPassLinePlan(double radius, int margin);
GaussianLinePlan makeUnsharpLinePlan(double radius, int margin);
void filterGaussianLine(std::vector<double>& values, std::vector<double>& scratch, const GaussianLinePlan& plan);

/// Patchy's validate_stack for one entry (out-of-range values make the stack refused).
bool parametersValid(const SmartFilterParameters& parameters);
/// Patchy's filter_noise_hash.
uint32_t addNoiseHash(int32_t x, int32_t y, uint32_t seed);

// The limits the kernels use, as smartfilter_render.cpp has them.
inline constexpr double gaussianMarginScale = 3.0;
inline constexpr int32_t surfaceBlurDirectMaximumRadius = 8;
inline constexpr int32_t boxBlurDirectMaximumRadius = 12;
inline constexpr int32_t boxBlurMaximumRadius = 2000;
inline constexpr int32_t mosaicMinimumCellSize = 2;
inline constexpr double addNoiseMinimumAmount = 0.1, addNoiseMaximumAmount = 400.0;
inline constexpr int32_t addNoiseMinimumSeed = 0, addNoiseMaximumSeed = 999999999;

/// Where Radial Blur's sample `sample` of `samples` for the pixel `dx`, `dy` from the centre is read, relative to
/// the centre. Spin sweeps an arc of amount x 3.6 degrees centred on the pixel. Zoom reads along the line through the
/// centre, over a span of amount / 200 of the pixel's distance centred on the pixel (so a streak grows with the
/// distance from the centre and the centre itself stays put). The span was fitted to Photoshop 2026's preview of
/// Patchy's photoshop-smart-filter-radial-blur-zoom.psd (Zoom 25, Best): mean 0.22 levels, at most 4 levels off;
/// Photoshop's remaining difference looks like jittered sample positions, which this does not copy.
inline constexpr double radialZoomSpan = 0.5;
inline void radialBlurSource(double dx, double dy, int32_t amount, int32_t samples, bool zoom, int sample, double& sx, double& sy) {
    constexpr double kPi = 3.14159265358979323846;
    const double t = samples <= 1 ? 0.0 : double(sample) / double(samples - 1) - 0.5;
    if (zoom) {
        const double scale = 1.0 - radialZoomSpan * double(amount) / 100.0 * t;
        sx = dx * scale;
        sy = dy * scale;
        return;
    }
    const auto angle = double(amount) * 3.6 * kPi / 180.0 * t;
    sx = dx * std::cos(angle) - dy * std::sin(angle);
    sy = dx * std::sin(angle) + dy * std::cos(angle);
}

} // namespace compositor::smartfilter_detail
