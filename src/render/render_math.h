#pragma once

// Internal to the render module. Camera matrix builders on top of core math.
// Render is decoupled from the simulation and may use libm freely.

#include "core/mat.h"
#include "core/types.h"
#include "core/vec.h"

#include <cmath>

namespace render {

using core::Mat4;
using core::Vec3;
using core::lerp;

/// Shortest-path interpolation of angles in radians, with exact endpoints.
[[nodiscard]] inline f32 angle_lerp(f32 a, f32 b, f32 t) {
    if (t <= 0.0f) {
        return a;
    }
    if (t >= 1.0f) {
        return b;
    }
    const f32 pi = 3.14159265358979f;
    const f32 two_pi = 6.28318530717959f;
    f32 d = b - a;
    while (d > pi) {
        d -= two_pi;
    }
    while (d < -pi) {
        d += two_pi;
    }
    return a + t * d;
}

/// Right-handed Vulkan perspective. Depth maps to [0, 1] and Y is flipped so
/// the image is upright in Vulkan's coordinate system.
[[nodiscard]] inline Mat4 perspective(f32 fovy, f32 aspect, f32 znear, f32 zfar) {
    const f32 f = 1.0f / std::tan(fovy * 0.5f);
    Mat4 out;
    out.m[0] = f / aspect;
    out.m[5] = -f;
    out.m[10] = zfar / (znear - zfar);
    out.m[11] = -1.0f;
    out.m[14] = (znear * zfar) / (znear - zfar);
    out.m[15] = 0.0f;
    return out;
}

/// First-person view matrix from a position, yaw/pitch, and a roll (for wallrun
/// camera tilt), matching the sim's convention: yaw 0 faces -Z, positive pitch
/// looks up.
[[nodiscard]] inline Mat4 view_fps(f32 px, f32 py, f32 pz, f32 yaw, f32 pitch, f32 roll) {
    const f32 cp = std::cos(pitch);
    const f32 sp = std::sin(pitch);
    const f32 cy = std::cos(yaw);
    const f32 sy = std::sin(yaw);

    const Vec3 forward{sy * cp, sp, -cy * cp};
    Vec3 right = Vec3{-forward.z, 0.0f, forward.x}.normalized();
    Vec3 up = cross(right, forward);

    // Roll the right and up vectors around the forward axis.
    if (roll != 0.0f) {
        const f32 cr = std::cos(roll);
        const f32 sr = std::sin(roll);
        const Vec3 new_right = right * cr + up * sr;
        up = up * cr - right * sr;
        right = new_right;
    }

    const Vec3 pos{px, py, pz};
    Mat4 out;
    out.m[0] = right.x;
    out.m[4] = right.y;
    out.m[8] = right.z;
    out.m[12] = -dot(right, pos);
    out.m[1] = up.x;
    out.m[5] = up.y;
    out.m[9] = up.z;
    out.m[13] = -dot(up, pos);
    out.m[2] = -forward.x;
    out.m[6] = -forward.y;
    out.m[10] = -forward.z;
    out.m[14] = dot(forward, pos);
    return out;
}

}  // namespace render
