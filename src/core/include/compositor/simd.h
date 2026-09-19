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
typedef uint16_t u16x8 __attribute__((vector_size(16)));
typedef int32_t i32x4 __attribute__((vector_size(16)));
typedef uint32_t u32x4 __attribute__((vector_size(16)));

/// Lanes picked from `a` (0..N-1) and `b` (N..2N-1) into a vector of the same type; `type` names it.
#if defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 12)
#define COMPOSITOR_SHUFFLE(type, a, b, ...) __builtin_shufflevector(a, b, __VA_ARGS__)
#else
#define COMPOSITOR_SHUFFLE(type, a, b, ...) __builtin_shuffle(a, b, (type){__VA_ARGS__})
#endif

/// The low or high eight bytes of a 16-byte vector, widened to 16 bits.
inline u16x8 widenLow(u8x16 v) { u8x8 half; std::memcpy(&half, &v, 8); return __builtin_convertvector(half, u16x8); }
inline u16x8 widenHigh(u8x16 v) { u8x8 half; std::memcpy(&half, reinterpret_cast<const char*>(&v) + 8, 8); return __builtin_convertvector(half, u16x8); }

} // namespace compositor::simd
