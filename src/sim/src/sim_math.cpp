#include "sim_math.h"

namespace sim {

namespace {

constexpr f32 TWO_PI = 6.28318530717958647692f;
constexpr f32 HALF_PI = 1.57079632679489661923f;
constexpr f32 INV_TWO_PI = 0.159154943091895335769f;

// Taylor coefficients for sin over [-PI, PI]. Fixed literals, so the result is
// bit-identical everywhere.
constexpr f32 C3 = -0.16666667f;      // -1/6
constexpr f32 C5 = 0.0083333338f;     //  1/120
constexpr f32 C7 = -0.00019841270f;   // -1/5040
constexpr f32 C9 = 0.0000027557319f;  //  1/362880

}  // namespace

f32 wrap_angle(f32 x) {
    const f32 q = x * INV_TWO_PI;
    const f32 biased = q + (q >= 0.0f ? 0.5f : -0.5f);
    const i32 k = static_cast<i32>(biased);  // truncation toward zero, deterministic
    return x - static_cast<f32>(k) * TWO_PI;
}

f32 sim_sin(f32 x) {
    const f32 a = wrap_angle(x);
    const f32 a2 = a * a;
    return a * (1.0f + a2 * (C3 + a2 * (C5 + a2 * (C7 + a2 * C9))));
}

f32 sim_cos(f32 x) { return sim_sin(x + HALF_PI); }

f32 sim_sqrt(f32 x) { return __builtin_sqrtf(x); }

}  // namespace sim
