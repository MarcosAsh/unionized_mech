#pragma once

#include "core/quat.h"
#include "core/types.h"
#include "core/vec.h"

namespace core {

/// A 4x4 matrix stored column-major to match GLSL: the element at column c,
/// row r is m[c * 4 + r]. Identity by default. Projection and view builders
/// live with the renderer because they need trig; the algebra lives here.
struct Mat4 {
    f32 m[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};

    /// Matrix product. Applying the result transforms by `b` first, then `a`.
    [[nodiscard]] friend Mat4 operator*(const Mat4& a, const Mat4& b) {
        Mat4 out;
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

    [[nodiscard]] friend bool operator==(const Mat4& a, const Mat4& b) {
        for (u32 i = 0; i < 16; ++i) {
            if (a.m[i] != b.m[i]) {
                return false;
            }
        }
        return true;
    }

    /// Rows and columns exchanged.
    [[nodiscard]] Mat4 transposed() const {
        Mat4 out;
        for (u32 c = 0; c < 4; ++c) {
            for (u32 r = 0; r < 4; ++r) {
                out.m[r * 4 + c] = m[c * 4 + r];
            }
        }
        return out;
    }

    /// Transform a point, w = 1.
    [[nodiscard]] Vec3 transform_point(Vec3 p) const {
        return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
                m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
                m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
    }

    /// Transform a direction, w = 0. Translation does not apply.
    [[nodiscard]] Vec3 transform_dir(Vec3 d) const {
        return {m[0] * d.x + m[4] * d.y + m[8] * d.z,
                m[1] * d.x + m[5] * d.y + m[9] * d.z,
                m[2] * d.x + m[6] * d.y + m[10] * d.z};
    }

    /// Pure translation.
    [[nodiscard]] static Mat4 translation(Vec3 t) {
        Mat4 out;
        out.m[12] = t.x;
        out.m[13] = t.y;
        out.m[14] = t.z;
        return out;
    }

    /// Per-axis scale.
    [[nodiscard]] static Mat4 scale(Vec3 s) {
        Mat4 out;
        out.m[0] = s.x;
        out.m[5] = s.y;
        out.m[10] = s.z;
        return out;
    }

    /// Rotation from a unit quaternion.
    [[nodiscard]] static Mat4 from_quat(Quat q) {
        const f32 xx = q.x * q.x;
        const f32 yy = q.y * q.y;
        const f32 zz = q.z * q.z;
        const f32 xy = q.x * q.y;
        const f32 xz = q.x * q.z;
        const f32 yz = q.y * q.z;
        const f32 wx = q.w * q.x;
        const f32 wy = q.w * q.y;
        const f32 wz = q.w * q.z;

        Mat4 out;
        out.m[0] = 1.0f - 2.0f * (yy + zz);
        out.m[1] = 2.0f * (xy + wz);
        out.m[2] = 2.0f * (xz - wy);
        out.m[4] = 2.0f * (xy - wz);
        out.m[5] = 1.0f - 2.0f * (xx + zz);
        out.m[6] = 2.0f * (yz + wx);
        out.m[8] = 2.0f * (xz + wy);
        out.m[9] = 2.0f * (yz - wx);
        out.m[10] = 1.0f - 2.0f * (xx + yy);
        return out;
    }

    /// Model matrix: rotate by `q`, then translate by `t`.
    [[nodiscard]] static Mat4 trs(Vec3 t, Quat q) {
        Mat4 out = from_quat(q);
        out.m[12] = t.x;
        out.m[13] = t.y;
        out.m[14] = t.z;
        return out;
    }
};

}  // namespace core
