#pragma once

#include "core/types.h"
#include "core/vec.h"

namespace core {

/// A rotation quaternion. Identity by default. Like the rest of core math this
/// uses only deterministic operations. Constructors that involve an angle take
/// the caller's sine and cosine of the half angle, so sim passes its fixed trig
/// and render passes libm, and the algebra here stays shared.
struct Quat {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
    f32 w = 1.0f;

    /// Rotation about unit axis, from the sine and cosine of half the angle.
    [[nodiscard]] static constexpr Quat from_axis_half(Vec3 unit_axis, f32 sin_half,
                                                       f32 cos_half) {
        return {unit_axis.x * sin_half, unit_axis.y * sin_half, unit_axis.z * sin_half, cos_half};
    }

    /// Hamilton product. Applying the result rotates by `b` first, then `a`.
    [[nodiscard]] friend constexpr Quat operator*(Quat a, Quat b) {
        return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
    }

    [[nodiscard]] friend constexpr bool operator==(Quat a, Quat b) {
        return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
    }

    /// The reverse rotation. Equals the inverse for unit quaternions.
    [[nodiscard]] constexpr Quat conjugate() const { return {-x, -y, -z, w}; }

    /// Squared magnitude. 1 for a unit quaternion.
    [[nodiscard]] constexpr f32 length_sq() const { return x * x + y * y + z * z + w * w; }

    /// Unit quaternion, or identity when this is zero.
    [[nodiscard]] Quat normalized() const {
        const f32 len = sqrt_f32(length_sq());
        if (len <= 0.0f) {
            return Quat{};
        }
        return {x / len, y / len, z / len, w / len};
    }

    /// Rotate a vector by this unit quaternion.
    [[nodiscard]] constexpr Vec3 rotate(Vec3 v) const {
        // v' = v + 2w(q x v) + 2(q x (q x v))
        const Vec3 q{x, y, z};
        const Vec3 t = cross(q, v) * 2.0f;
        return v + t * w + cross(q, t);
    }
};

/// Normalized linear interpolation between unit quaternions, exact at the
/// endpoints. Takes the shorter arc. Sufficient for the small steps of frame
/// interpolation and animation blending; slerp can come later if it earns it.
[[nodiscard]] inline Quat nlerp(Quat a, Quat b, f32 t) {
    const f32 cos_ab = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    const f32 sign = cos_ab < 0.0f ? -1.0f : 1.0f;
    const Quat blended{lerp(a.x, b.x * sign, t), lerp(a.y, b.y * sign, t),
                       lerp(a.z, b.z * sign, t), lerp(a.w, b.w * sign, t)};
    return blended.normalized();
}

}  // namespace core
