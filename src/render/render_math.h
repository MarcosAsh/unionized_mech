#pragma once

// Internal to the render module. Camera matrices, column-major to match GLSL.
// Render is decoupled from the simulation and may use libm freely.

#include "core/types.h"

#include <cmath>

namespace render {

/// A 4x4 matrix stored column-major (element at column c, row r is m[c*4 + r]).
struct Mat4 {
    f32 m[16];
};

/// Matrix product a * b.
[[nodiscard]] inline Mat4 mat4_mul(const Mat4& a, const Mat4& b) {
    Mat4 out{};
    for (u32 c = 0; c < 4; ++c) {
        for (u32 r = 0; r < 4; ++r) {
            f32 sum = 0.0f;
            for (u32 k = 0; k < 4; ++k) {
                sum += a.m[k * 4 + r] * b.m[c * 4 + k];
            }
            out.m[c * 4 + r] = sum;
        }
    }
    return out;
}

/// Right-handed Vulkan perspective. Depth maps to [0, 1] and Y is flipped so the
/// image is upright in Vulkan's coordinate system.
[[nodiscard]] inline Mat4 perspective(f32 fovy, f32 aspect, f32 znear, f32 zfar) {
    const f32 f = 1.0f / std::tan(fovy * 0.5f);
    Mat4 out{};
    out.m[0] = f / aspect;
    out.m[5] = -f;
    out.m[10] = zfar / (znear - zfar);
    out.m[11] = -1.0f;
    out.m[14] = (znear * zfar) / (znear - zfar);
    return out;
}

/// First-person view matrix from a position and yaw/pitch, matching the sim's
/// convention: yaw 0 faces -Z, positive pitch looks up.
[[nodiscard]] inline Mat4 view_fps(f32 px, f32 py, f32 pz, f32 yaw, f32 pitch) {
    const f32 cp = std::cos(pitch);
    const f32 sp = std::sin(pitch);
    const f32 cy = std::cos(yaw);
    const f32 sy = std::sin(yaw);

    const f32 fx = sy * cp;
    const f32 fy = sp;
    const f32 fz = -cy * cp;

    f32 rx = -fz;
    f32 rz = fx;
    const f32 rl = std::sqrt(rx * rx + rz * rz);
    if (rl > 0.0f) {
        rx /= rl;
        rz /= rl;
    }
    const f32 ry = 0.0f;

    const f32 ux = ry * fz - rz * fy;
    const f32 uy = rz * fx - rx * fz;
    const f32 uz = rx * fy - ry * fx;

    Mat4 out{};
    out.m[0] = rx;
    out.m[4] = ry;
    out.m[8] = rz;
    out.m[12] = -(rx * px + ry * py + rz * pz);
    out.m[1] = ux;
    out.m[5] = uy;
    out.m[9] = uz;
    out.m[13] = -(ux * px + uy * py + uz * pz);
    out.m[2] = -fx;
    out.m[6] = -fy;
    out.m[10] = -fz;
    out.m[14] = fx * px + fy * py + fz * pz;
    out.m[15] = 1.0f;
    return out;
}

}  // namespace render
