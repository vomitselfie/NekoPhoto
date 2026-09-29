// The Camera Raw kernels over premultiplied float RGBA on the 8-bit scale (0..255), unrounded, for 16-bit documents
// (CameraRawPixelsBody.inc).
#include <math.h>
#include <stddef.h>

// lens_distort (LensPixels.c) on floats: bilinear, unrounded.
static void camera_raw_lens_distort_float(const float *source, float *destination, size_t width, size_t height, size_t stride, double k) {
    double cx = width * 0.5, cy = height * 0.5;
    double halfDiagonal2 = cx * cx + cy * cy;
    for (size_t y = 0; y < height; ++y) {
        double dy = y + 0.5 - cy;
        float *out = destination + y * stride;
        for (size_t x = 0; x < width; ++x) {
            double dx = x + 0.5 - cx;
            double scale = 1.0 - k * (dx * dx + dy * dy) / halfDiagonal2;
            double sx = cx + dx * scale - 0.5, sy = cy + dy * scale - 0.5;
            double fx0 = floor(sx), fy0 = floor(sy);
            double fx = sx - fx0, fy = sy - fy0;
            long x0 = (long)fx0, y0 = (long)fy0;
            double sums[4] = {0, 0, 0, 0};
            for (int j = 0; j < 2; ++j) {
                long row = y0 + j;
                if (row < 0 || row >= (long)height) continue;
                double wy = j ? fy : 1 - fy;
                if (wy == 0) continue;
                const float *line = source + (size_t)row * stride;
                for (int i = 0; i < 2; ++i) {
                    long column = x0 + i;
                    if (column < 0 || column >= (long)width) continue;
                    double weight = wy * (i ? fx : 1 - fx);
                    if (weight == 0) continue;
                    const float *p = line + (size_t)column * 4;
                    for (int c = 0; c < 4; ++c) sums[c] += weight * p[c];
                }
            }
            for (int c = 0; c < 4; ++c) out[x * 4 + c] = (float)sums[c];
        }
    }
}

#define CR_PIXEL float
#define CR_NAME(name) name##_float
#define CR_ROUND(value) (value)
#define CR_LENS_DISTORT camera_raw_lens_distort_float
#define CR_DECISION(value, alpha) (round((value) * (alpha)) / (alpha))
#include "CameraRawPixelsBody.inc"
