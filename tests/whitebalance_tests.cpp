// Temperature and Tint (compositor/whitebalance.h): Robertson's method against reference white points, the round trip
// through xy, the tint scale's sign, and camera neutrals through one and two calibration matrices.
#include "check.h"
#include "compositor/whitebalance.h"
#include <cmath>

using namespace compositor;

namespace {

// XYZ -> linear sRGB: a camera whose native space is sRGB, D65 white.
const Matrix3 kSrgb = {{{3.2406, -1.5372, -0.4986}, {-0.9689, 1.8758, 0.0415}, {0.0557, -0.2040, 1.0570}}};

Matrix3 scaledRows(const Matrix3& m, double a, double b, double c) {
    Matrix3 r = m;
    for (double& v : r[0]) v *= a;
    for (double& v : r[1]) v *= b;
    for (double& v : r[2]) v *= c;
    return r;
}

} // namespace

TEST_CASE(reference_white_points_read_their_temperatures) {
    // CIE D65, D50 and Standard illuminant A (2 degree observer).
    const TemperatureTint d65 = temperatureTintFromXy({0.31271, 0.32902});
    CHECK_NEAR(d65.temperature, 6504, 5);
    CHECK(std::fabs(d65.tint) < 10);
    CHECK_NEAR(temperatureTintFromXy({0.34567, 0.35850}).temperature, 5003, 5);
    const TemperatureTint a = temperatureTintFromXy({0.44757, 0.40745});
    CHECK_NEAR(a.temperature, 2856, 3);
    CHECK(std::fabs(a.tint) < 2);   // A is a Planckian radiator: on the locus
}

TEST_CASE(temperature_and_tint_round_trip_through_xy) {
    for (double t : {2000.0, 2800.0, 3200.0, 4000.0, 5000.0, 6500.0, 8000.0, 10000.0, 20000.0, 50000.0})
        for (double tint : {-150.0, -40.0, 0.0, 7.0, 60.0, 150.0}) {
            const Chromaticity xy = xyFromTemperatureTint(t, tint);
            const TemperatureTint back = temperatureTintFromXy(xy);
            CHECK_NEAR(back.temperature, t, t * 1e-9);
            CHECK_NEAR(back.tint, tint, 1e-7);
            const Chromaticity again = xyFromTemperatureTint(back.temperature, back.tint);
            CHECK_NEAR(again.x, xy.x, 1e-12);
            CHECK_NEAR(again.y, xy.y, 1e-12);
        }
}

TEST_CASE(tint_is_positive_on_the_green_side) {
    const Chromaticity locus = xyFromTemperatureTint(5000, 0);
    const Chromaticity green = xyFromTemperatureTint(5000, 50), magenta = xyFromTemperatureTint(5000, -50);
    CHECK(green.y > locus.y);
    CHECK(magenta.y < locus.y);
    // 1 tint unit is 1/3000 of a CIE 1960 uv unit.
    auto uv = [](Chromaticity c) {
        const double d = -2 * c.x + 12 * c.y + 3;
        return std::array<double, 2>{4 * c.x / d, 6 * c.y / d};
    };
    const auto a = uv(locus), b = uv(green);
    CHECK_NEAR(std::hypot(b[0] - a[0], b[1] - a[1]) * kTintScale, 50, 1e-9);
    // Warmer settings sit further along the locus toward red.
    CHECK(xyFromTemperatureTint(3000, 0).x > xyFromTemperatureTint(6500, 0).x);
}

TEST_CASE(a_single_matrix_camera_reads_its_neutral) {
    const CameraWhiteModel model = CameraWhiteModel::single(kSrgb);
    REQUIRE(model.valid);
    // An sRGB camera sees D65 as (1, 1, 1).
    const auto xy = model.xyFromNeutral({1, 1, 1});
    REQUIRE(xy);
    CHECK_NEAR(temperatureTintFromXy(*xy).temperature, 6504, 10);
    // neutral -> xy -> (T, tint) -> xy -> neutral.
    for (const std::array<double, 3> neutral : {std::array<double, 3>{0.5, 1, 0.7}, {0.8, 1, 0.9}, {1.3, 1, 1.6}}) {
        const auto white = model.xyFromNeutral(neutral);
        REQUIRE(white);
        const TemperatureTint value = temperatureTintFromXy(*white);
        const auto back = model.neutralFromXy(xyFromTemperatureTint(value.temperature, value.tint));
        REQUIRE(back);
        for (int c = 0; c < 3; c++) CHECK_NEAR((*back)[size_t(c)], neutral[size_t(c)] / neutral[1], 1e-9);
    }
    // Multipliers and neutrals are reciprocal.
    const auto m = multipliersFromNeutral({0.5, 1, 0.7});
    CHECK_NEAR(m[0], 2, 1e-12);
    CHECK_NEAR(m[2], 1 / 0.7, 1e-12);
    CHECK_NEAR(neutralFromMultipliers(m)[2], 0.7, 1e-12);
}

TEST_CASE(two_matrices_interpolate_in_reciprocal_temperature) {
    const Matrix3 a = scaledRows(kSrgb, 1.25, 1.0, 0.8);
    const CameraWhiteModel model = CameraWhiteModel::dual(a, 2856, kSrgb, 6504);
    REQUIRE(model.valid);
    REQUIRE(model.isDual());
    // At and beyond the illuminants, their own matrices.
    CHECK_NEAR(model.matrixAt(2856)[0][0], a[0][0], 1e-12);
    CHECK_NEAR(model.matrixAt(2000)[0][0], a[0][0], 1e-12);
    CHECK_NEAR(model.matrixAt(6504)[0][0], kSrgb[0][0], 1e-12);
    CHECK_NEAR(model.matrixAt(20000)[0][0], kSrgb[0][0], 1e-12);
    // Halfway in 1/T (not in kelvin) is the average.
    const double middle = 2 / (1 / 2856.0 + 1 / 6504.0);
    CHECK_NEAR(model.matrixAt(middle)[0][0], (a[0][0] + kSrgb[0][0]) / 2, 1e-12);
    // The solved white point is self-consistent: its temperature's matrix maps it back to the neutral.
    for (const std::array<double, 3> neutral : {std::array<double, 3>{0.5, 1, 0.7}, {0.9, 1, 1.1}, {0.4, 1, 0.9}}) {
        const auto xy = model.xyFromNeutral(neutral);
        REQUIRE(xy);
        const auto back = model.neutralFromXy(*xy);
        REQUIRE(back);
        for (int c = 0; c < 3; c++) CHECK_NEAR((*back)[size_t(c)], neutral[size_t(c)] / neutral[1], 1e-9);
    }
}

TEST_CASE(light_sources_have_their_dng_temperatures) {
    CHECK_EQ(lightSourceTemperature(17), 2856.0);
    CHECK_EQ(lightSourceTemperature(21), 6504.0);
    CHECK_EQ(lightSourceTemperature(0), 0.0);
}

TEST_MAIN()
