// White balance as Temperature (kelvin) and Tint, the way Camera Raw shows it for a RAW file.
//
// A white point's chromaticity (CIE 1931 xy) turns into a correlated colour temperature with Robertson's method
// (A. R. Robertson, "Computation of Correlated Color Temperature and Distribution Temperature", JOSA 58, 1968),
// over the isotemperature lines he published and Wyszecki & Stiles reprint (Color Science, 2nd ed., table 1(3.11)):
// 31 lines from 0 to 600 mired, each a point of the Planckian locus in CIE 1960 uv and the slope of the line through it.
//
// Tint is the white point's signed distance from the locus, measured along the isotemperature line in CIE 1960 uv
// and multiplied by 3000 (tintScale): one tint unit is 1/3000 of a uv unit, which puts the fluorescent and flash
// white points camera makers record in the tens, as Camera Raw's readout does. Positive tint is a white point on the
// green side of the locus (the correction then adds magenta, as Camera Raw's positive Tint does); negative is
// magenta. Between two tabulated lines the locus point and the line's direction are interpolated linearly in mired,
// and the reading solves for the line that passes through the white point, so xy -> (T, tint) -> xy is exact up to
// rounding.
//
// A camera's neutral (the camera-space response to the scene white, the reciprocal of its white balance multipliers)
// turns into xy through the camera's XYZ -> camera matrix, as the DNG specification describes: one matrix, or two
// calibrated under two illuminants and interpolated by reciprocal temperature (1/T) between them.
#pragma once
#include <array>
#include <optional>

namespace compositor {

struct Chromaticity { double x = 0, y = 0; };

struct TemperatureTint {
    double temperature = 0;   // kelvin
    double tint = 0;          // positive: the white point lies on the green side of the locus
};

/// Camera Raw's ranges for a RAW file.
constexpr double kMinRawTemperature = 2000, kMaxRawTemperature = 50000, kMaxRawTint = 150;
/// uv distance -> tint units (see above).
constexpr double kTintScale = 3000;

/// Robertson's correlated colour temperature and the tint of `xy`. Temperatures beyond the table (above 0 mired or
/// below 1667 K) are clamped to its ends.
TemperatureTint temperatureTintFromXy(Chromaticity xy);
/// The white point with that temperature and tint; the inverse of temperatureTintFromXy.
Chromaticity xyFromTemperatureTint(double temperature, double tint);

/// Correlated colour temperature of an EXIF / DNG LightSource code (17 Standard A, 21 D65, ...); 0 when unknown.
double lightSourceTemperature(int code);

using Matrix3 = std::array<std::array<double, 3>, 3>;
std::optional<Matrix3> invert(const Matrix3& m);

/// A camera's colour model for white balance: XYZ -> camera-space matrices calibrated under one or two illuminants.
struct CameraWhiteModel {
    Matrix3 xyzToCamera1{};
    double temperature1 = 0;   // the first matrix's illuminant; 0 for a single-matrix model
    Matrix3 xyzToCamera2{};
    double temperature2 = 0;   // the second's; 0 when there is only one
    bool valid = false;

    static CameraWhiteModel single(const Matrix3& xyzToCamera);
    /// Two matrices with their illuminants' temperatures; a single model when either temperature is unknown.
    static CameraWhiteModel dual(const Matrix3& m1, double t1, const Matrix3& m2, double t2);
    bool isDual() const { return temperature1 > 0 && temperature2 > 0 && temperature1 != temperature2; }
    /// The XYZ -> camera matrix for a white point of temperature `t`: interpolated in 1/T between the two illuminants
    /// (clamped to them), or the one matrix.
    Matrix3 matrixAt(double t) const;
    /// The white point whose camera response is `neutral` (camera RGB, any scale). For two matrices the temperature
    /// and the matrix are solved together. Empty when the neutral maps outside the visible gamut.
    std::optional<Chromaticity> xyFromNeutral(const std::array<double, 3>& neutral) const;
    /// The camera response to white point `xy`, green 1.
    std::optional<std::array<double, 3>> neutralFromXy(Chromaticity xy) const;
};

/// White balance multipliers (green 1) <-> the camera neutral they bring to grey: each the reciprocal of the other.
std::array<double, 3> neutralFromMultipliers(const std::array<double, 3>& multipliers);
std::array<double, 3> multipliersFromNeutral(const std::array<double, 3>& neutral);

} // namespace compositor
