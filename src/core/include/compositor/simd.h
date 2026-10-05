// The GCC/Clang vector extensions the kernels use: fixed-width lane types, a shuffle that works on both
// compilers (GCC before 12 has no __builtin_shufflevector, and its __builtin_shuffle cannot change the lane
// count), and widening of a vector's halves.
#pragma once
#include <cstdint>
#include <cstring>

namespace compositor::simd {

typedef uint8_t u8x4 __attribute__((vector_size(4)));
typedef uint8_t u8x8 __attribute__((vector_size(8)));
typedef uint8_t u8x16 __attribute__((vector_size(16)));
typedef uint16_t u16x4 __attribute__((vector_size(8)));
typedef uint16_t u16x8 __attribute__((vector_size(16)));
typedef int32_t i32x4 __attribute__((vector_size(16)));
typedef uint32_t u32x4 __attribute__((vector_size(16)));
typedef float f32x4 __attribute__((vector_size(16)));

/// Lanes picked from `a` (0..N-1) and `b` (N..2N-1) into a vector of the same type; `type` names it.
#if defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 12)
#define COMPOSITOR_SHUFFLE(type, a, b, ...) __builtin_shufflevector(a, b, __VA_ARGS__)
#else
#define COMPOSITOR_SHUFFLE(type, a, b, ...) __builtin_shuffle(a, b, (type){__VA_ARGS__})
#endif

/// The low or high eight bytes of a 16-byte vector, widened to 16 bits.
inline u16x8 widenLow(u8x16 v) { u8x8 half; std::memcpy(&half, &v, 8); return __builtin_convertvector(half, u16x8); }
inline u16x8 widenHigh(u8x16 v) { u8x8 half; std::memcpy(&half, reinterpret_cast<const char*>(&v) + 8, 8); return __builtin_convertvector(half, u16x8); }

/// 16-bit source-over on four samples: min(one, round(s k / 2^15) + round(d inv / 2^15)), with k and inv at most
/// 2^15 so every product fits 32-bit lanes exactly (the 64-bit scalar form's values).
inline void sourceOver16(const uint16_t* src, uint32_t k, uint32_t inv, uint16_t* dst, uint32_t one) {
    u16x4 s, d;
    std::memcpy(&s, src, 8);
    std::memcpy(&d, dst, 8);
    const u32x4 half = {16384, 16384, 16384, 16384}, top = {one, one, one, one};
    u32x4 r = ((__builtin_convertvector(s, u32x4) * k + half) >> 15) + ((__builtin_convertvector(d, u32x4) * inv + half) >> 15);
    r = r > top ? top : r;
    const u16x4 o = __builtin_convertvector(r, u16x4);
    std::memcpy(dst, &o, 8);
}

} // namespace compositor::simd
