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
using core::Vec4;
using core::angle_lerp;
using core::lerp;

/// The six planes of a view frustum, inward-facing and normalized, as
/// (normal.xyz, d) with dot(n, p) + d >= 0 for points inside.
struct Frustum {
    Vec4 planes[6];
};

/// Extract the frustum from a view-projection matrix (Gribb-Hartmann), for the
/// Vulkan [0, 1] depth convention.
[[nodiscard]] inline Frustum frustum_from(const Mat4& m) {
    // Row r of the column-major matrix.
    const auto row = [&m](u32 r) {
        return Vec4{m.m[r], m.m[4 + r], m.m[8 + r], m.m[12 + r]};
    };
    const Vec4 r0 = row(0);
    const Vec4 r1 = row(1);
    const Vec4 r2 = row(2);
    const Vec4 r3 = row(3);

    Frustum out;
    out.planes[0] = r3 + r0;  // left
    out.planes[1] = r3 - r0;  // right
    out.planes[2] = r3 + r1;  // bottom
    out.planes[3] = r3 - r1;  // top
    out.planes[4] = r2;       // near, depth >= 0
    out.planes[5] = r3 - r2;  // far
    for (u32 i = 0; i < 6; ++i) {
        Vec4& p = out.planes[i];
        const f32 len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
        if (len > 0.0f) {
            p = p * (1.0f / len);
        }
    }
    return out;
}

/// Orthographic projection for the Vulkan conventions: depth [0, 1], Y flipped.
/// Used by the sun shadow pass.
[[nodiscard]] inline Mat4 orthographic(f32 left, f32 right, f32 bottom, f32 top, f32 znear,
                                       f32 zfar) {
    Mat4 out;
    out.m[0] = 2.0f / (right - left);
    out.m[5] = -2.0f / (top - bottom);
    out.m[10] = 1.0f / (znear - zfar);
    out.m[12] = -(right + left) / (right - left);
    out.m[13] = (top + bottom) / (top - bottom);
    out.m[14] = znear / (znear - zfar);
    return out;
}

/// View matrix looking along `dir` from `eye`, up biased to world Y.
[[nodiscard]] inline Mat4 look_along(Vec3 eye, Vec3 dir) {
    const Vec3 forward = dir.normalized();
    Vec3 right = cross(forward, Vec3{0.0f, 1.0f, 0.0f});
    if (right.length_sq() < 1e-6f) {
        right = Vec3{1.0f, 0.0f, 0.0f};
    } else {
        right = right.normalized();
    }
    const Vec3 up = cross(right, forward);

    Mat4 out;
    out.m[0] = right.x;
    out.m[4] = right.y;
    out.m[8] = right.z;
    out.m[12] = -dot(right, eye);
    out.m[1] = up.x;
    out.m[5] = up.y;
    out.m[9] = up.z;
    out.m[13] = -dot(up, eye);
    out.m[2] = -forward.x;
    out.m[6] = -forward.y;
    out.m[10] = -forward.z;
    out.m[14] = dot(forward, eye);
    return out;
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
/// The camera's world-space axes for a first-person view. Shared with the sky
/// pass, which needs the same basis to build its view rays; a second copy of
/// this would drift the sky away from the camera the moment either is retuned.
inline void camera_basis(f32 yaw, f32 pitch, f32 roll, Vec3* out_right, Vec3* out_up,
                         Vec3* out_forward) {
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
    *out_right = right;
    *out_up = up;
    *out_forward = forward;
}

[[nodiscard]] inline Mat4 view_fps(f32 px, f32 py, f32 pz, f32 yaw, f32 pitch, f32 roll) {
    Vec3 right;
    Vec3 up;
    Vec3 forward;
    camera_basis(yaw, pitch, roll, &right, &up, &forward);

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
