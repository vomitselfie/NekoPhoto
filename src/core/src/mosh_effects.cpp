// The Mosh effects, each a port of its OpenMosh WGSL shader (MIT, crates/mosh-engine/shaders/<name>.wgsl; see
// THIRD-PARTY-NOTICES.md). The code follows the shader line for line, in single precision and in its order of
// evaluation, so the seeded patterns come out as OpenMosh draws them. Parameters arrive as OpenMosh's uniform slots:
// p[0] is the first parameter in the registry, p[1] the second, and so on.
#include "mosh_effects.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace compositor::mosh {

namespace {

using namespace rt;

// ---- Glitch ---------------------------------------------------------------------------------------------------

// Soft Glitch: chromatic aberration. p0 = amount, p1 = angle (radians).
void softGlitch(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const float amount = u.p[0] * 0.05f;
    const float angle = u.p[1];
    const vec2 off = vec2{cos(angle), sin(angle)} * amount;
    shade(in.width(), in.height(), [&](const Frag& f) {
        float r = sample(in, f.uv + off).x;
        vec4 ga = sample(in, f.uv);
        float b = sample(in, f.uv - off).z;
        return vec4{r, ga.y, b, ga.w};
    }, sink);
}

// Hard Glitch: multi-scale random block displacement and a channel shift. p0 = amount, p1 = blocks, p2 = colour shift.
void hardGlitch(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const float amount = u.p[0];
    const float blocks = std::max(u.p[1], 2.0f);
    const float shift = u.p[2];
    shade(in.width(), in.height(), [&](const Frag& f) {
        vec2 uv = f.uv;
        for (int s = 0; s < 3; s++) {
            float scale = blocks * float(1 << s);   // pow(2.0, f32(s)), exact
            vec2 cell = floor(uv * scale) * (1.0f / scale);   // the GPU divides by the reciprocal; the hash sees its bits
            float r = rand2(cell + u.seed + float(s) * 7.31f);
            if (r < amount * 0.35f) {
                float ox = rand2(cell + u.seed + 1.7f) - 0.5f;
                float oy = rand2(cell + u.seed + 3.9f) - 0.5f;
                uv = uv + vec2{ox, oy * 0.3f} * amount * 0.4f;
            }
        }
        vec2 off{shift * amount * 0.03f, 0.0f};
        float r = sample(in, uv + off).x;
        vec4 ga = sample(in, uv);
        float b = sample(in, uv - off).z;
        return vec4{r, ga.y, b, ga.w};
    }, sink);
}

// Decimate: random blocks collapse to one colour. p0 = amount, p1 = block size.
void decimate(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const float amount = u.p[0];
    const vec2 grid = u.resolution / std::max(u.p[1], 2.0f);
    shade(in.width(), in.height(), [&](const Frag& f) {
        vec2 cell = floor(f.uv * grid);
        float r = rand2(fma32(cell, v2(0.713f), v2(u.seed)));   // cell * 0.713 + seed, fused as the GPU does
        vec2 uv = f.uv;
        if (r < amount) uv = (cell + 0.5f) / grid;
        return sample(in, uv);
    }, sink);
}

// Data-Mosh: stuck blocks copy from a drifted offset, rows smear. p0 = block size, p1 = drift, p2 = stuck fraction.
void dataMosh(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const vec2 grid = u.resolution / std::max(u.p[0], 4.0f);
    const float drift = u.p[1];
    const float stick = u.p[2];
    shade(in.width(), in.height(), [&](const Frag& f) {
        vec2 cell = floor(f.uv * grid);
        vec2 uv = f.uv;
        if (rand2(fma32(cell, v2(0.731f), v2(u.seed))) < stick) {   // cell * 0.731 + seed, fused as the GPU does
            float dx = rand2(cell + u.seed + 11.3f) - 0.5f;
            float dy = rand2(cell + u.seed + 71.7f) - 0.5f;
            uv = uv + vec2{dx, dy} * drift * 0.25f;
        }
        float rowr = rand2(vec2{cell.y, u.seed});
        uv.x += (rowr - 0.5f) * drift * 0.06f;
        return sample(in, uv);
    }, sink);
}

// Splitter: alternating offset strips. p0 = strips, p1 = offset, p2 = vertical.
void splitter(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const float strips = std::max(u.p[0], 1.0f);
    const float offset = u.p[1];
    const bool vertical = u.p[2] > 0.5f;
    shade(in.width(), in.height(), [&](const Frag& f) {
        vec2 uv = f.uv;
        if (vertical) {
            float i = std::floor(uv.x * strips);
            float dir = float(toI32(i) % 2) * 2.0f - 1.0f;
            uv.y = fract(uv.y + dir * offset);
        } else {
            float i = std::floor(uv.y * strips);
            float dir = float(toI32(i) % 2) * 2.0f - 1.0f;
            uv.x = fract(uv.x + dir * offset);
        }
        return sample(in, uv);
    }, sink);
}

// Jitter: a random horizontal offset per band. p0 = amount, p1 = band height in pixels.
void jitter(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const float bands = u.resolution.y / std::max(u.p[1], 1.0f);
    shade(in.width(), in.height(), [&](const Frag& f) {
        float band = std::floor(f.uv.y * bands);
        float r = rand2(vec2{band, u.seed}) - 0.5f;
        vec2 uv = f.uv;
        uv.x += r * u.p[0] * 0.2f;
        return sample(in, uv);
    }, sink);
}

// Slices: randomly offset slabs. p0 = count, p1 = offset, p2 = vertical.
void slices(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const float count = std::max(u.p[0], 1.0f);
    const float offset = u.p[1];
    const bool vertical = u.p[2] > 0.5f;
    shade(in.width(), in.height(), [&](const Frag& f) {
        vec2 uv = f.uv;
        if (vertical) {
            float i = std::floor(uv.x * count);
            float r = rand2(vec2{i, u.seed}) - 0.5f;
            uv.y = fract(uv.y + r * offset);
        } else {
            float i = std::floor(uv.y * count);
            float r = rand2(vec2{i, u.seed}) - 0.5f;
            uv.x = fract(uv.x + r * offset);
        }
        return sample(in, uv);
    }, sink);
}

// Shake: the whole frame moved by a random offset. p0 = amount.
void shake(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const float ox = rand2(vec2{u.seed, 1.3f}) - 0.5f;
    const float oy = rand2(vec2{u.seed, 7.7f}) - 0.5f;
    const vec2 off = vec2{ox, oy} * u.p[0];
    shade(in.width(), in.height(), [&](const Frag& f) { return sample(in, f.uv + off); }, sink);
}

// Pixel Sort (OpenMosh's compute shader): along each row (or column), in segments of 256 pixels whose grid is shifted
// by a seeded offset per line, the runs of pixels whose luma lies in [low, high] are sorted by luma; the pixels outside
// the range stay where they are and bound the runs. The shader's odd-even transposition sort swaps neighbours only
// when strictly out of order, so it is a stable sort, and 256 rounds sort any segment fully: a stable sort of each
// run gives the same order. p0 = low, p1 = high, p2 = reverse (descending), p3 = vertical.
void pixelSort(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const float low = u.p[0], high = u.p[1];
    const bool reverse = u.p[2] > 0.5f, vertical = u.p[3] > 0.5f;
    const int width = in.width(), height = in.height();
    const int lines = vertical ? width : height, along = vertical ? height : width;
    // from[line * along + i]: where along the line the pixel that lands at i comes from.
    std::vector<int32_t> from(size_t(lines) * size_t(along));
    parallelRows(0, lines, [&](int l0, int l1) {
        std::vector<float> key(static_cast<size_t>(along));
        for (int line = l0; line < l1; line++) {
            int32_t* order = from.data() + size_t(line) * size_t(along);
            std::iota(order, order + along, 0);
            for (int i = 0; i < along; i++) {
                vec4 c = vertical ? in.load(line, i) : in.load(i, line);
                float l = luma(c.rgb());
                key[size_t(i)] = l >= low && l <= high ? l : -1.0f;
            }
            uint32_t hx = uint32_t(line), hy = bitsOf(u.seed);
            pcg2d(hx, hy);
            const int off = int(hx & 255u);
            auto less = [&](int32_t a, int32_t b) { return reverse ? key[size_t(a)] > key[size_t(b)] : key[size_t(a)] < key[size_t(b)]; };
            for (int start = -off; start < along; start += 256) {
                const int s0 = std::max(start, 0), s1 = std::min(start + 256, along);
                for (int i = s0; i < s1;) {
                    if (key[size_t(i)] < 0) { i++; continue; }
                    int j = i;
                    while (j < s1 && key[size_t(j)] >= 0) j++;
                    std::stable_sort(order + i, order + j, less);
                    i = j;
                }
            }
        }
    }, 4);
    shade(width, height, [&](const Frag& f) {
        if (vertical) return in.load(f.x, from[size_t(f.x) * size_t(along) + size_t(f.y)]);
        return in.load(from[size_t(f.y) * size_t(along) + size_t(f.x)], f.y);
    }, sink);
}

// Strobe: a flash frame, from time and phase (time is 0 for a still image: phase 0.5 and up shows the flash).
// p0 = phase, p1 = rate, p2 = mode (0 blackout, 1 whiteout, 2 invert).
void strobe(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const float flash = step(0.5f, fract(u.time * u.p[1] + u.p[0]));
    const int mode = toI32(u.p[2] + 0.5f);
    shade(in.width(), in.height(), [&](const Frag& f) {
        vec4 c = sample(in, f.uv);
        vec3 out = c.rgb();
        if (flash > 0.5f) {
            if (mode == 0) out = v3(0.0f);
            else if (mode == 1) out = v3(1.0f);
            else out = 1.0f - c.rgb();
        }
        return v4(out, c.w);
    }, sink);
}

// ---- Distort --------------------------------------------------------------------------------------------------

// Wave: sinusoidal displacement. p0 = amplitude, p1 = frequency, p2 = phase, p3 = vertical.
void wave(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const float amplitude = u.p[0], frequency = u.p[1], phase = u.p[2], vertical = u.p[3];
    shade(in.width(), in.height(), [&](const Frag& f) {
        vec2 uv = f.uv;
        if (vertical > 0.5f) uv.y += amplitude * sin(uv.x * frequency + phase);
        else uv.x += amplitude * sin(uv.y * frequency + phase);
        return sample(in, uv);
    }, sink);
}

// Kaleidoscope: mirrored radial wedges. p0 = segments, p1 = rotation.
void kaleidoscope(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const float aspect = u.resolution.x / u.resolution.y;
    const float seg = 6.2831853f / std::max(u.p[0], 2.0f);
    shade(in.width(), in.height(), [&](const Frag& f) {
        vec2 d = f.uv - 0.5f;
        d.x *= aspect;
        float r = length(d);
        float a = atan2(d.y, d.x) + u.p[1];
        a = std::fabs(fract(a / seg) - 0.5f) * seg;
        vec2 uv = vec2{cos(a), sin(a)} * r;
        uv.x /= aspect;
        uv = uv + 0.5f;
        return sample(in, uv);
    }, sink);
}

// ---- Retro ----------------------------------------------------------------------------------------------------

// Pixelate: blocky downsample. p0 = block size in pixels.
void pixelate(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const float size = std::max(u.p[0], 1.0f);
    const vec2 block = size / u.resolution;
    shade(in.width(), in.height(), [&](const Frag& f) { return sample(in, (floor(f.uv / block) + 0.5f) * block); }, sink);
}

// Scan Lines: darkened horizontal lines. p0 = density, p1 = opacity.
void scanlines(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const float density = u.p[0], opacity = u.p[1];
    shade(in.width(), in.height(), [&](const Frag& f) {
        vec4 c = sample(in, f.uv);
        float line = 0.5f + 0.5f * sin(f.uv.y * density * 3.14159265f);
        return v4(c.rgb() * (1.0f - opacity * line), c.w);
    }, sink);
}

// VHS: line jitter, colour bleed and tape noise. p0 = tracking, p1 = bleed, p2 = noise. The tape noise hashes each
// pixel's coordinates, which a GPU interpolates to within a few units in the last place: the grain here is the same
// kind of noise as OpenMosh's but not the same grains (docs/mosh.md).
void vhs(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const float tracking = u.p[0], bleed = u.p[1], noise = u.p[2];
    const vec2 off{bleed * 0.008f, 0.0f};
    shade(in.width(), in.height(), [&](const Frag& f) {
        vec2 uv = f.uv;
        float line = std::floor(uv.y * u.resolution.y);
        if (rand2(vec2{line, u.seed}) < tracking * 0.1f) uv.x += (rand2(vec2{line, u.seed + 1.0f}) - 0.5f) * tracking * 0.2f;
        float band = vnoise(vec2{uv.y * 6.0f, u.seed * 17.0f});
        uv.x += (band - 0.5f) * tracking * 0.02f;
        float r = sample(in, uv + off).x;
        vec4 ga = sample(in, uv);
        float b = sample(in, uv - off).z;
        vec3 c{r, ga.y, b};
        float l = luma(c);
        c = mix(c, v3(l), bleed * 0.3f);
        float n = rand2(uv * u.resolution + u.seed * 31.0f);
        c = mix(c, v3(n), noise * 0.2f);
        if (rand2(vec2{line, u.seed + 5.0f}) > 1.0f - noise * 0.03f) c = v3(rand2(uv * u.resolution + u.seed));
        return v4(c, ga.w);
    }, sink);
}

// 8-Bit CGA: pixelate, then the nearest of a four-colour palette. p0 = pixel size, p1 = palette.
void cga8bit(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const float size = std::max(u.p[0], 1.0f);
    const vec2 block = size / u.resolution;
    const int mode = toI32(u.p[1] + 0.5f);
    vec3 pal[4];
    if (mode == 0) { pal[0] = v3(0.0f); pal[1] = {0.33f, 1.0f, 1.0f}; pal[2] = {1.0f, 0.33f, 1.0f}; pal[3] = v3(1.0f); }
    else if (mode == 1) { pal[0] = v3(0.0f); pal[1] = {0.33f, 1.0f, 0.33f}; pal[2] = {1.0f, 0.33f, 0.33f}; pal[3] = {1.0f, 1.0f, 0.33f}; }
    else { pal[0] = v3(0.0f); pal[1] = v3(0.33f); pal[2] = v3(0.66f); pal[3] = v3(1.0f); }
    shade(in.width(), in.height(), [&](const Frag& f) {
        vec4 c = sample(in, (floor(f.uv / block) + 0.5f) * block);
        int best = 0;
        float bestd = 1e9f;
        for (int i = 0; i < 4; i++) {
            vec3 diff = c.rgb() - pal[i];
            // Perceptually weighted distance, as the shader.
            float dd = dot(diff * diff, vec3{0.299f, 0.587f, 0.114f});
            if (dd < bestd) { bestd = dd; best = i; }
        }
        return v4(pal[best], c.w);
    }, sink);
}

// CRT: barrel curvature, scanlines and an RGB aperture mask. p0 = curvature, p1 = scanlines, p2 = mask.
void crt(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const vec2 edge = 2.0f / u.resolution;
    shade(in.width(), in.height(), [&](const Frag& f) {
        vec2 uv = f.uv * 2.0f - 1.0f;
        uv = uv * (1.0f + u.p[0] * 0.25f * dot(uv, uv));
        uv = uv / (1.0f + u.p[0] * 0.25f) * 0.5f + 0.5f;
        vec4 c0 = sample(in, uv);
        vec3 c = c0.rgb();
        float inside = smoothstep(0.0f, edge.x, uv.x) * smoothstep(0.0f, edge.x, 1.0f - uv.x) * smoothstep(0.0f, edge.y, uv.y) *
                       smoothstep(0.0f, edge.y, 1.0f - uv.y);
        c = c * inside;
        float scan = 0.5f + 0.5f * sin(uv.y * u.resolution.y * 3.14159265f);
        c = c * (1.0f - u.p[1] * 0.6f * scan);
        int col = toI32(f.uv.x * u.resolution.x) % 3;
        vec3 tint = col == 0 ? vec3{1.0f, 0.7f, 0.7f} : col == 1 ? vec3{0.7f, 1.0f, 0.7f} : vec3{0.7f, 0.7f, 1.0f};
        c = c * mix(v3(1.0f), tint, u.p[2]);
        return v4(c, c0.w);
    }, sink);
}

// Dither: Bayer 4x4 ordered dithering. p0 = scale, p1 = levels.
void dither(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const float scale = std::max(u.p[0], 1.0f);
    const float levels = std::max(u.p[1], 2.0f) - 1.0f;
    shade(in.width(), in.height(), [&](const Frag& f) {
        vec2 px = floor(f.uv * u.resolution / scale);
        float t = bayer4(px) - 0.5f;
        vec4 c = sample(in, f.uv);
        const vec3 k = c.rgb();   // c * levels + 0.5, fused as the GPU does: it decides which side of a step a pixel lands
        vec3 q = floor(vec3{fma32(k.x, levels, 0.5f), fma32(k.y, levels, 0.5f), fma32(k.z, levels, 0.5f)} + t) / levels;
        return v4(clamp(q, 0.0f, 1.0f), c.w);
    }, sink);
}

// Dot Screen: luminance as black dots on white. p0 = scale, p1 = angle.
void dotScreen(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const float scale = std::max(u.p[0], 2.0f);
    shade(in.width(), in.height(), [&](const Frag& f) {
        vec4 c = sample(in, f.uv);
        float l = luma(c.rgb());
        vec2 p = rotate(u.p[1], f.uv * u.resolution / scale);
        float d = length(fract(p) - 0.5f);
        float r = (1.0f - l) * 0.75f;
        float v = smoothstep(r - 0.08f, r + 0.08f, d);
        return v4(v3(v), c.w);
    }, sink);
}

// Halftone: colour dots sized by luminance on black. p0 = scale, p1 = angle.
void halftone(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const float scale = std::max(u.p[0], 2.0f);
    shade(in.width(), in.height(), [&](const Frag& f) {
        vec2 p = rotate(u.p[1], f.uv * u.resolution / scale);
        // Sampled at the dot's centre, so each dot is one flat colour.
        vec2 center = rotate(-u.p[1], floor(p) + 0.5f) * scale / u.resolution;
        vec4 c = sample(in, clamp(center, 0.0f, 1.0f));
        float l = luma(c.rgb());
        float d = length(fract(p) - 0.5f);
        float r = std::sqrt(l) * 0.6f;
        float v = 1.0f - smoothstep(r - 0.08f, r + 0.08f, d);
        return v4(c.rgb() * v, c.w);
    }, sink);
}

// ---- Distort, continued -------------------------------------------------------------------------------------------

// Bulge: radial lens distortion. p0 = strength, p1 = radius, p2 = centre x, p3 = centre y.
void bulge(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const float strength = u.p[0], radius = u.p[1];
    const vec2 center{u.p[2], u.p[3]};
    const float aspect = u.resolution.x / u.resolution.y;
    shade(in.width(), in.height(), [&](const Frag& f) {
        vec2 d = f.uv - center;
        d.x *= aspect;
        float dist = length(d);
        if (dist < radius) {
            float t = dist / radius;
            float k = 1.0f - strength * (1.0f - t * t);
            d = d * k;
        }
        d.x /= aspect;
        return sample(in, center + d);
    }, sink);
}

// Stretch: a Gaussian-weighted local stretch about a line. p0 = centre, p1 = width, p2 = amount, p3 = vertical.
void stretch(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const float c = u.p[0], w = u.p[1], k = u.p[2];
    const bool vertical = u.p[3] > 0.5f;
    shade(in.width(), in.height(), [&](const Frag& f) {
        vec2 uv = f.uv;
        if (vertical) {
            float d = uv.x - c;
            uv.x = c + d / (1.0f + k * exp(-(d * d) / (w * w + 1e-5f)));
        } else {
            float d = uv.y - c;
            uv.y = c + d / (1.0f + k * exp(-(d * d) / (w * w + 1e-5f)));
        }
        return sample(in, uv);
    }, sink);
}

// Push: a translation, wrapping round or not. p0 = dx, p1 = dy, p2 = wrap.
void push(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const vec2 off{u.p[0], u.p[1]};
    const bool wrap = u.p[2] > 0.5f;
    shade(in.width(), in.height(), [&](const Frag& f) {
        vec2 uv = f.uv - off;
        if (wrap) uv = fract(uv);
        return sample(in, uv);
    }, sink);
}

// Luma-Mesh: each pixel displaced along a direction by its luma. p0 = amount, p1 = angle.
void lumaMesh(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const vec2 dir{cos(u.p[1]), sin(u.p[1])};
    shade(in.width(), in.height(), [&](const Frag& f) {
        float l = luma(sample(in, f.uv).rgb());
        return sample(in, f.uv + dir * (l - 0.5f) * u.p[0]);
    }, sink);
}

// 3D Transform: a perspective tilt, then scale, rotation and offset. p0 = scale, p1 = rotation, p2 = x, p3 = y,
// p4 = tilt x, p5 = tilt y.
void transform3d(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const float aspect = u.resolution.x / u.resolution.y;
    const vec2 shift = 0.5f - vec2{u.p[2], u.p[3]};
    shade(in.width(), in.height(), [&](const Frag& f) {
        vec2 uv = f.uv - 0.5f;
        uv.x *= aspect;
        float z = 1.0f + u.p[4] * uv.x + u.p[5] * uv.y;
        uv = uv * std::max(z, 0.1f);
        uv = rotate(-u.p[1], uv);
        uv = uv / std::max(u.p[0], 0.01f);
        uv.x /= aspect;
        uv = uv + shift;
        return sample(in, uv);
    }, sink);
}

// Tile: a grid of copies, alternate ones mirrored. p0 = columns, p1 = rows, p2 = mirror.
void tile(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const vec2 n{std::max(u.p[0], 1.0f), std::max(u.p[1], 1.0f)};
    const bool mirrored = u.p[2] > 0.5f;
    shade(in.width(), in.height(), [&](const Frag& f) {
        vec2 scaled = f.uv * n;
        vec2 cell = floor(scaled);
        vec2 uv = fract(scaled);
        if (mirrored) {
            if (toI32(cell.x) % 2 == 1) uv.x = 1.0f - uv.x;
            if (toI32(cell.y) % 2 == 1) uv.y = 1.0f - uv.y;
        }
        return sample(in, uv);
    }, sink);
}

// Mirror: one half reflected onto the other. p0 = mode (0 left to right, 1 right to left, 2 top to bottom, 3 bottom
// to top).
void mirror(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const int mode = toI32(u.p[0] + 0.5f);
    shade(in.width(), in.height(), [&](const Frag& f) {
        vec2 uv = f.uv;
        if (mode == 0) { if (uv.x > 0.5f) uv.x = 1.0f - uv.x; }
        else if (mode == 1) { if (uv.x < 0.5f) uv.x = 1.0f - uv.x; }
        else if (mode == 2) { if (uv.y > 0.5f) uv.y = 1.0f - uv.y; }
        else if (uv.y < 0.5f) uv.y = 1.0f - uv.y;
        return sample(in, uv);
    }, sink);
}

// Wobble: a sinusoidal warp on both axes. p0 = amount, p1 = frequency, p2 = phase.
void wobble(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const float amount = u.p[0], fr = u.p[1], ph = u.p[2];
    shade(in.width(), in.height(), [&](const Frag& f) {
        vec2 uv = f.uv;
        uv.x += sin(uv.y * fr + ph) * amount;
        uv.y += cos(uv.x * fr + ph * 1.3f) * amount;
        return sample(in, uv);
    }, sink);
}

// Smear: a directional blur of eight taps, their spacing jittered per pixel. p0 = distance, p1 = angle. The jitter
// hashes each pixel's coordinates, which a GPU interpolates to within a few units in the last place: the jitter is the
// same kind as OpenMosh's but not the same draw (docs/mosh.md).
void smear(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const vec2 dir = vec2{cos(u.p[1]), sin(u.p[1])} * u.p[0];
    shade(in.width(), in.height(), [&](const Frag& f) {
        float jitter = rand2(f.uv * u.resolution);
        vec4 acc;
        for (int i = 0; i < 8; i++) {
            float t = (float(i) + jitter) / 8.0f;
            acc = acc + sample(in, f.uv - dir * t);
        }
        return acc / 8.0f;
    }, sink);
}

// Twirl: a swirl about the centre, fading out to the radius. p0 = angle, p1 = radius.
void twirl(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const float aspect = u.resolution.x / u.resolution.y;
    const float radius = u.p[1];
    shade(in.width(), in.height(), [&](const Frag& f) {
        vec2 d = f.uv - 0.5f;
        d.x *= aspect;
        float r = length(d);
        if (r < radius) {
            float t = 1.0f - r / radius;
            d = rotate(u.p[0] * t * t, d);
        }
        d.x /= aspect;
        return sample(in, d + 0.5f);
    }, sink);
}

// Optical-Flow: a liquid warp along the luma gradient (or across it). p0 = amount, p1 = gradient distance in pixels,
// p2 = swirl.
void opticalFlow(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const vec2 d = u.p[1] / u.resolution;
    shade(in.width(), in.height(), [&](const Frag& f) {
        float gx = luma(sample(in, f.uv + vec2{d.x, 0.0f}).rgb()) - luma(sample(in, f.uv - vec2{d.x, 0.0f}).rgb());
        float gy = luma(sample(in, f.uv + vec2{0.0f, d.y}).rgb()) - luma(sample(in, f.uv - vec2{0.0f, d.y}).rgb());
        vec2 flow{mix(gx, -gy, u.p[2]), mix(gy, gx, u.p[2])};   // mix(gradient, curl, swirl)
        float wob = sin(u.time + luma(sample(in, f.uv).rgb()) * 6.2831853f) * 0.5f + 0.5f;
        return sample(in, f.uv + flow * u.p[0] * (0.5f + wob));
    }, sink);
}

// ---- Retro, continued ---------------------------------------------------------------------------------------------

// Super 8: film grain, a vignette and a warm cast. p0 = grain, p1 = vignette, p2 = warmth. The grain hashes pixel
// coordinates, as VHS's noise does (a different draw of the same grain).
void super8(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const vec2 seedOff{u.seed * 91.7f, u.seed * 33.1f};
    shade(in.width(), in.height(), [&](const Frag& f) {
        vec4 c0 = sample(in, f.uv);
        vec3 c = c0.rgb();
        float n = rand2(f.uv * u.resolution + seedOff) - 0.5f;
        c = c + n * u.p[0] * 0.3f;
        float d = distance(f.uv, v2(0.5f));
        c = c * (1.0f - u.p[1] * smoothstep(0.35f, 0.85f, d));
        c = mix(c, c * vec3{1.12f, 0.96f, 0.72f}, u.p[2]);
        return v4(clamp(c, 0.0f, 1.0f), c0.w);
    }, sink);
}

// Bad TV: a rolled picture, a wavy distortion and static. p0 = distortion, p1 = roll, p2 = noise. The static hashes
// pixel coordinates (a different draw of the same noise; docs/mosh.md).
void badTv(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const float distortion = u.p[0];
    shade(in.width(), in.height(), [&](const Frag& f) {
        vec2 uv = f.uv;
        uv.y = fract(uv.y + u.p[1]);
        uv.x += sin(uv.y * 12.0f + u.seed * 6.2831853f) * distortion * 0.04f;
        uv.x += (vnoise(vec2{uv.y * 4.0f, u.seed * 10.0f}) - 0.5f) * distortion * 0.2f;
        vec4 c0 = sample(in, uv);
        vec3 c = c0.rgb();
        float n = rand2(f.uv * u.resolution + u.seed * 47.0f);
        c = mix(c, v3(n), u.p[2] * 0.35f);
        return v4(c, c0.w);
    }, sink);
}

// Ascii: an 8x8 glyph per cell, from a density ramp by the cell's luma. p0 = cell size in pixels, p1 = colour (0 green,
// 1 white, 2 the image's), p2 = invert.
void ascii(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const float size = std::max(u.p[0], 4.0f);
    const bool invert = u.p[2] > 0.5f;
    const int mode = toI32(u.p[1] + 0.5f);
    // ' ' . : = + * # @, each as two words of four rows, a byte a row, bit N the pixel at x = N.
    static constexpr uint32_t glyphs[8][2] = {
        {0x00000000u, 0x00000000u}, {0x00000000u, 0x00181800u}, {0x00181800u, 0x00181800u}, {0x007e0000u, 0x0000007eu},
        {0x7e181800u, 0x0018187eu}, {0xff3c6600u, 0x0000663cu}, {0x24247e24u, 0x0024247eu}, {0xffff7e3cu, 0x3c7effffu},
    };
    shade(in.width(), in.height(), [&](const Frag& f) {
        vec2 cellUv = (floor(f.uv * u.resolution / size) + 0.5f) * size / u.resolution;
        vec4 c = sample(in, clamp(cellUv, 0.0f, 1.0f));
        float l = luma(c.rgb());
        if (invert) l = 1.0f - l;
        int idx = std::clamp(toI32(l * 8.0f), 0, 7);
        vec2 pf = floor(fract(f.uv * u.resolution / size) * 8.0f);
        uint32_t px = toU32(pf.x), py = toU32(pf.y);
        uint32_t rowWord = py >= 4u ? glyphs[idx][1] : glyphs[idx][0];
        uint32_t rowByte = (rowWord >> ((py & 3u) * 8u)) & 0xffu;
        float on = float((rowByte >> (px & 31u)) & 1u);
        vec3 ink{0.3f, 1.0f, 0.4f};
        if (mode == 1) ink = v3(1.0f);
        else if (mode == 2) ink = c.rgb() / std::max(l, 0.05f);
        return v4(clamp(ink * on, 0.0f, 1.0f), c.w);
    }, sink);
}

// ---- Stylize --------------------------------------------------------------------------------------------------

// Bleach: bleach bypass, the image overlaid with its own luma. p0 = amount.
void bleach(const Frame& in, const Uniforms& u, const RowSink& sink) {
    shade(in.width(), in.height(), [&](const Frag& f) {
        vec4 c = sample(in, f.uv);
        float l = luma(c.rgb());
        vec3 blend = mix(2.0f * c.rgb() * l, 1.0f - 2.0f * (1.0f - c.rgb()) * (1.0f - l), step(0.5f, l));
        return v4(mix(c.rgb(), blend, u.p[0]), c.w);
    }, sink);
}

// Edges: the Sobel magnitude of the luma. p0 = amount, p1 = invert.
void edges(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const vec2 px = 1.0f / u.resolution;
    shade(in.width(), in.height(), [&](const Frag& f) {
        float gx = 0, gy = 0;
        for (int y = -1; y <= 1; y++)
            for (int x = -1; x <= 1; x++) {
                float l = luma(sample(in, f.uv + vec2{float(x), float(y)} * px).rgb());
                gx += l * float(x) * (y == 0 ? 2.0f : 1.0f);
                gy += l * float(y) * (x == 0 ? 2.0f : 1.0f);
            }
        float v = clamp(std::sqrt(gx * gx + gy * gy) * u.p[0], 0.0f, 1.0f);
        if (u.p[1] > 0.5f) v = 1.0f - v;
        return v4(v3(v), sample(in, f.uv).w);
    }, sink);
}

// Emboss: a directional relief of the luma. p0 = strength, p1 = angle.
void emboss(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const vec2 dir = vec2{cos(u.p[1]), sin(u.p[1])} * u.p[0] / u.resolution;
    shade(in.width(), in.height(), [&](const Frag& f) {
        float a = luma(sample(in, f.uv - dir).rgb());
        float b = luma(sample(in, f.uv + dir).rgb());
        float alpha = sample(in, f.uv).w;
        return v4(v3(clamp(0.5f + (a - b) * 2.0f, 0.0f, 1.0f)), alpha);
    }, sink);
}

// Vignette: darkened corners. p0 = amount, p1 = radius, p2 = softness.
void vignette(const Frame& in, const Uniforms& u, const RowSink& sink) {
    shade(in.width(), in.height(), [&](const Frag& f) {
        vec4 c = sample(in, f.uv);
        float d = distance(f.uv, v2(0.5f)) * 1.4142f;
        float v = smoothstep(u.p[1], u.p[1] + u.p[2], d);
        return v4(c.rgb() * (1.0f - u.p[0] * v), c.w);
    }, sink);
}

// Noise Displace: a smooth-noise warp. p0 = amount, p1 = scale.
void noiseDisplace(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const float scale = u.p[1];
    const float s = u.seed * 13.7f;
    const vec2 second = vec2{91.3f, 41.9f} + s;
    shade(in.width(), in.height(), [&](const Frag& f) {
        float n1 = vnoise(f.uv * scale + s);
        float n2 = vnoise(f.uv * scale + second);
        vec2 uv = f.uv + (vec2{n1, n2} - 0.5f) * u.p[0];
        return sample(in, uv);
    }, sink);
}

/// luma as a GPU evaluates the dot product: a multiply and two fused multiply-adds.
float lumaFused(vec3 c) { return fma32(c.z, 0.114f, fma32(c.y, 0.587f, c.x * 0.299f)); }

// Watercolor: a Kuwahara filter, each pixel the mean of the least varied of four 3x3 quadrants. p0 = radius.
void watercolor(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const vec2 px = u.p[0] / (2.0f * u.resolution);
    shade(in.width(), in.height(), [&](const Frag& f) {
        vec3 best;
        float bestv = 1e9f;
        for (int q = 0; q < 4; q++) {
            float sx = q % 2 == 0 ? 1.0f : -1.0f;
            float sy = q < 2 ? 1.0f : -1.0f;
            vec3 mean;
            float m2 = 0;
            for (int y = 0; y <= 2; y++)
                for (int x = 0; x <= 2; x++) {
                    vec2 o = vec2{float(x) * sx, float(y) * sy} * px;
                    vec3 s = sample(in, f.uv + o).rgb();
                    mean = mean + s;
                    float l = lumaFused(s);
                    m2 = fma32(l, l, m2);
                }
            mean = mean / 9.0f;
            const float lm = lumaFused(mean);
            float variance = fma32(-lm, lm, m2 / 9.0f);
            if (variance < bestv) { bestv = variance; best = mean; }
        }
        return v4(best, sample(in, f.uv).w);
    }, sink);
}

// Zoom Blur: radial streaks toward a centre, twelve taps jittered per pixel (the jitter as Smear's). p0 = strength,
// p1 = centre x, p2 = centre y.
void zoomBlur(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const vec2 center{u.p[1], u.p[2]};
    shade(in.width(), in.height(), [&](const Frag& f) {
        vec2 d = (f.uv - center) * u.p[0];
        float jitter = rand2(f.uv * u.resolution);
        vec4 acc;
        for (int i = 0; i < 12; i++) {
            float t = (float(i) + jitter) / 12.0f;
            acc = acc + sample(in, f.uv - d * t);
        }
        return acc / 12.0f;
    }, sink);
}

/// One pass of Glow's separable 13-tap Gaussian along `step`.
Frame glowBlur(const Frame& in, vec2 step) {
    float w[13], wsum = 0;
    for (int i = -6; i <= 6; i++) {
        w[i + 6] = exp(-float(i * i) * 0.056f);
        wsum += w[i + 6];
    }
    return pass(in.width(), in.height(), [&](const Frag& f) {
        vec4 acc;
        for (int i = -6; i <= 6; i++) acc = acc + sample(in, f.uv + step * float(i)) * w[i + 6];
        return acc / wsum;
    });
}

// Glow: a bright pass, blurred across and down, added over the image. p0 = threshold, p1 = intensity, p2 = radius in
// pixels. Four passes, the frames between them at 16 bits.
void glow(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const Frame bright = pass(in.width(), in.height(), [&](const Frag& f) {
        vec4 c = sample(in, f.uv);
        return v4(max(c.rgb() - u.p[0], v3(0.0f)), c.w);
    });
    const Frame across = glowBlur(bright, vec2{u.p[2] / 6.0f / u.resolution.x, 0.0f});
    const Frame down = glowBlur(across, vec2{0.0f, u.p[2] / 6.0f / u.resolution.y});
    shade(in.width(), in.height(), [&](const Frag& f) {
        vec4 base = sample(in, f.uv);
        vec3 g = sample(down, f.uv).rgb();
        return v4(clamp(base.rgb() + g * u.p[1] * 2.0f, 0.0f, 1.0f), base.w);
    }, sink);
}

// Light Streak: a bright pass drawn out along a direction by three passes of a symmetric streak whose tap spacing grows
// fourfold each pass, added over the image. p0 = threshold, p1 = length, p2 = angle, p3 = intensity.
void lightStreak(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const float threshold = u.p[0];
    Frame cur = pass(in.width(), in.height(), [&](const Frag& f) {
        vec4 c = sample(in, f.uv);
        vec3 b = max(c.rgb() - threshold, v3(0.0f)) / std::max(1.0f - threshold, 1e-4f);
        return v4(b, c.w);
    });
    const vec2 dir = vec2{cos(u.p[2]), sin(u.p[2])} / u.resolution;
    const float att = 0.90f + u.p[1] * 0.08f;
    for (int p = 1; p <= 3; p++) {
        const float stride = pow(4.0f, float(p) - 1.0f);
        const float spread = stride * (2.0f + u.p[1] * 6.0f);
        float w[4] = {1.0f, 0, 0, 0};
        for (int s = 1; s < 4; s++) w[s] = pow(att, stride * float(s));
        const Frame& src = cur;
        Frame next = pass(in.width(), in.height(), [&](const Frag& f) {
            vec3 acc = sample(src, f.uv).rgb();
            float wsum = 1.0f;
            for (int s = 1; s < 4; s++) {
                acc = acc + sample(src, f.uv + dir * spread * float(s)).rgb() * w[s];
                acc = acc + sample(src, f.uv - dir * spread * float(s)).rgb() * w[s];
                wsum += 2.0f * w[s];
            }
            return v4(acc / wsum, sample(src, f.uv).w);
        });
        cur = std::move(next);
    }
    shade(in.width(), in.height(), [&](const Frag& f) {
        vec4 base = sample(in, f.uv);
        vec3 streak = sample(cur, f.uv).rgb();
        return v4(clamp(base.rgb() + streak * u.p[3], 0.0f, 1.0f), base.w);
    }, sink);
}

// Feedback: video-feedback trails, ten iterations each zooming and turning the last and keeping the brighter of it
// (decayed) and the image. p0 = zoom, p1 = rotation per iteration, p2 = decay.
void feedback(const Frame& in, const Uniforms& u, const RowSink& sink) {
    constexpr int iterations = 10;   // OpenMosh's FEEDBACK_ITERATIONS
    const float aspect = u.resolution.x / u.resolution.y;
    auto iteration = [&](const Frame& previous) {
        const Frame* prev = &previous;
        return [&in, &u, prev, aspect](const Frag& f) {
            vec2 d = f.uv - 0.5f;
            d.x *= aspect;
            d = rotate(-u.p[1], d) / (1.0f + u.p[0]);
            d.x /= aspect;
            vec4 trail = sample(*prev, d + 0.5f);
            vec4 base = sample(in, f.uv);
            return v4(max(base.rgb(), trail.rgb() * u.p[2]), base.w);
        };
    };
    Frame cur = pass(in.width(), in.height(), iteration(in));
    for (int i = 1; i < iterations - 1; i++) {
        Frame next = pass(in.width(), in.height(), iteration(cur));
        cur = std::move(next);
    }
    shade(in.width(), in.height(), iteration(cur), sink);
}

// ---- Color ----------------------------------------------------------------------------------------------------

// Color Correction: brightness, contrast, saturation and gamma. p0 = brightness, p1 = contrast, p2 = saturation,
// p3 = gamma.
void colorCorrection(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const float g = 1.0f / std::max(u.p[3], 0.05f);
    shade(in.width(), in.height(), [&](const Frag& f) {
        vec4 src = sample(in, f.uv);
        vec3 c = src.rgb() + u.p[0] * 0.5f;
        c = (c - 0.5f) * (1.0f + u.p[1]) + 0.5f;
        float l = luma(c);
        c = mix(v3(l), c, 1.0f + u.p[2]);
        c = pow(clamp(c, 0.0f, 1.0f), v3(g));
        return v4(c, src.w);
    }, sink);
}

// Duotone: luma mapped between a dark and a light hue. p0 = shadow hue, p1 = highlight hue, p2 = mix.
void duotone(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const vec3 dark = hsv2rgb(vec3{u.p[0], 0.75f, 0.30f});
    const vec3 light = hsv2rgb(vec3{u.p[1], 0.55f, 0.97f});
    shade(in.width(), in.height(), [&](const Frag& f) {
        vec4 src = sample(in, f.uv);
        float l = luma(src.rgb());
        vec3 mapped = mix(dark, light, l);
        return v4(mix(src.rgb(), mapped, u.p[2]), src.w);
    }, sink);
}

// Solarize: channels above a threshold inverted. p0 = centre, p1 = amount.
void solarize(const Frame& in, const Uniforms& u, const RowSink& sink) {
    shade(in.width(), in.height(), [&](const Frag& f) {
        vec4 src = sample(in, f.uv);
        vec3 c = src.rgb();
        vec3 flipped = mix(c, 1.0f - c, vec3{step(u.p[0], c.x), step(u.p[0], c.y), step(u.p[0], c.z)});
        return v4(mix(c, flipped, u.p[1]), src.w);
    }, sink);
}

// Chromatic Warp: red and blue scaled about a centre, the other way from each other. p0 = amount, p1 = centre x,
// p2 = centre y.
void chromaticWarp(const Frame& in, const Uniforms& u, const RowSink& sink) {
    const vec2 center{u.p[1], u.p[2]};
    shade(in.width(), in.height(), [&](const Frag& f) {
        vec2 d = f.uv - center;
        float r = sample(in, center + d * (1.0f - u.p[0])).x;
        vec4 ga = sample(in, f.uv);
        float b = sample(in, center + d * (1.0f + u.p[0])).z;
        return vec4{r, ga.y, b, ga.w};
    }, sink);
}

// Sepia. p0 = amount.
void sepia(const Frame& in, const Uniforms& u, const RowSink& sink) {
    shade(in.width(), in.height(), [&](const Frag& f) {
        vec4 src = sample(in, f.uv);
        vec3 c = src.rgb();
        vec3 s{dot(c, vec3{0.393f, 0.769f, 0.189f}), dot(c, vec3{0.349f, 0.686f, 0.168f}), dot(c, vec3{0.272f, 0.534f, 0.131f})};
        return v4(mix(c, clamp(s, 0.0f, 1.0f), u.p[0]), src.w);
    }, sink);
}

struct Entry { std::string_view id; EffectFn fn; };
constexpr Entry kEffects[] = {
    {"soft-glitch", softGlitch}, {"hard-glitch", hardGlitch}, {"decimate", decimate}, {"data-mosh", dataMosh},
    {"splitter", splitter}, {"jitter", jitter}, {"slices", slices}, {"shake", shake}, {"pixel-sort", pixelSort},
    {"strobe", strobe}, {"wave", wave}, {"kaleidoscope", kaleidoscope}, {"pixelate", pixelate}, {"scanlines", scanlines},
    {"vhs", vhs}, {"cga-8bit", cga8bit}, {"crt", crt}, {"dither", dither}, {"dot-screen", dotScreen}, {"halftone", halftone},
    {"bulge", bulge}, {"stretch", stretch}, {"push", push}, {"luma-mesh", lumaMesh}, {"transform-3d", transform3d}, {"tile", tile},
    {"mirror", mirror}, {"wobble", wobble}, {"smear", smear}, {"twirl", twirl}, {"optical-flow", opticalFlow},
    {"super8", super8}, {"bad-tv", badTv}, {"ascii", ascii},
    {"bleach", bleach}, {"edges", edges}, {"emboss", emboss}, {"vignette", vignette}, {"noise-displace", noiseDisplace},
    {"watercolor", watercolor}, {"zoom-blur", zoomBlur}, {"glow", glow}, {"light-streak", lightStreak}, {"feedback", feedback},
    {"color-correction", colorCorrection}, {"duotone", duotone}, {"solarize", solarize}, {"chromatic-warp", chromaticWarp}, {"sepia", sepia},
};

} // namespace

EffectFn effectFunction(std::string_view id) {
    for (const Entry& e : kEffects)
        if (e.id == id) return e.fn;
    return nullptr;
}

} // namespace compositor::mosh
