// What the 8-bit Smart Filter kernels (smartfilter_render.cpp) share with the 16-bit ones
// (smartfilter_render16.cpp): the Gaussian line plans with their Photoshop calibrations, the parameter checks and
// Add Noise's position hash. Internal to the core.
#pragma once
#include "compositor/smartfilter.h"
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

} // namespace compositor::smartfilter_detail
