#pragma once

#include "core/types.h"

namespace core {

// Value-type vector math. Only IEEE-754 add, subtract, multiply, divide, and
// sqrt appear here, all correctly rounded on every conformant platform, so
// these types are deterministic and shared by sim and render alike. Anything
// needing trig lives with its caller.

/// Correctly rounded square root via the hardware instruction. Deterministic
/// on every target, unlike libm transcendentals.
[[nodiscard]] inline f32 sqrt_f32(f32 x) { return __builtin_sqrtf(x); }

/// A 2D vector of f32. Cheap value, freely copied.
struct Vec2 {
    f32 x = 0.0f;
    f32 y = 0.0f;

    [[nodiscard]] friend constexpr Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
    [[nodiscard]] friend constexpr Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
    [[nodiscard]] friend constexpr Vec2 operator*(Vec2 v, f32 s) { return {v.x * s, v.y * s}; }
    [[nodiscard]] friend constexpr Vec2 operator*(f32 s, Vec2 v) { return v * s; }
    [[nodiscard]] friend constexpr Vec2 operator/(Vec2 v, f32 s) { return {v.x / s, v.y / s}; }
    [[nodiscard]] friend constexpr Vec2 operator-(Vec2 v) { return {-v.x, -v.y}; }
    [[nodiscard]] friend constexpr bool operator==(Vec2 a, Vec2 b) {
        return a.x == b.x && a.y == b.y;
    }
    constexpr Vec2& operator+=(Vec2 b) { x += b.x; y += b.y; return *this; }
    constexpr Vec2& operator-=(Vec2 b) { x -= b.x; y -= b.y; return *this; }
    constexpr Vec2& operator*=(f32 s) { x *= s; y *= s; return *this; }

    /// Dot product.
    [[nodiscard]] friend constexpr f32 dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
    /// Squared length. Cheaper than length, exact for comparisons.
    [[nodiscard]] constexpr f32 length_sq() const { return x * x + y * y; }
    /// Euclidean length.
    [[nodiscard]] f32 length() const { return sqrt_f32(length_sq()); }
    /// Unit vector, or zero when this is the zero vector.
    [[nodiscard]] Vec2 normalized() const {
        const f32 len = length();
        return len > 0.0f ? *this / len : Vec2{};
    }
};

/// A 3D vector of f32. Cheap value, freely copied.
struct Vec3 {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;

    [[nodiscard]] friend constexpr Vec3 operator+(Vec3 a, Vec3 b) {
        return {a.x + b.x, a.y + b.y, a.z + b.z};
    }
    [[nodiscard]] friend constexpr Vec3 operator-(Vec3 a, Vec3 b) {
        return {a.x - b.x, a.y - b.y, a.z - b.z};
    }
    [[nodiscard]] friend constexpr Vec3 operator*(Vec3 v, f32 s) {
        return {v.x * s, v.y * s, v.z * s};
    }
    [[nodiscard]] friend constexpr Vec3 operator*(f32 s, Vec3 v) { return v * s; }
    [[nodiscard]] friend constexpr Vec3 operator/(Vec3 v, f32 s) {
        return {v.x / s, v.y / s, v.z / s};
    }
    [[nodiscard]] friend constexpr Vec3 operator-(Vec3 v) { return {-v.x, -v.y, -v.z}; }
    [[nodiscard]] friend constexpr bool operator==(Vec3 a, Vec3 b) {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }
    constexpr Vec3& operator+=(Vec3 b) { x += b.x; y += b.y; z += b.z; return *this; }
    constexpr Vec3& operator-=(Vec3 b) { x -= b.x; y -= b.y; z -= b.z; return *this; }
    constexpr Vec3& operator*=(f32 s) { x *= s; y *= s; z *= s; return *this; }

    /// Dot product.
    [[nodiscard]] friend constexpr f32 dot(Vec3 a, Vec3 b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }
    /// Right-handed cross product.
    [[nodiscard]] friend constexpr Vec3 cross(Vec3 a, Vec3 b) {
        return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    }
    /// Squared length. Cheaper than length, exact for comparisons.
    [[nodiscard]] constexpr f32 length_sq() const { return x * x + y * y + z * z; }
    /// Euclidean length.
    [[nodiscard]] f32 length() const { return sqrt_f32(length_sq()); }
    /// Unit vector, or zero when this is the zero vector.
    [[nodiscard]] Vec3 normalized() const {
        const f32 len = length();
        return len > 0.0f ? *this / len : Vec3{};
    }
};

/// A 4D vector of f32, the homogeneous companion to Vec3.
struct Vec4 {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
    f32 w = 0.0f;

    [[nodiscard]] friend constexpr Vec4 operator+(Vec4 a, Vec4 b) {
        return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
    }
    [[nodiscard]] friend constexpr Vec4 operator-(Vec4 a, Vec4 b) {
        return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
    }
    [[nodiscard]] friend constexpr Vec4 operator*(Vec4 v, f32 s) {
        return {v.x * s, v.y * s, v.z * s, v.w * s};
    }
    [[nodiscard]] friend constexpr Vec4 operator*(f32 s, Vec4 v) { return v * s; }
    [[nodiscard]] friend constexpr bool operator==(Vec4 a, Vec4 b) {
        return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
    }

    /// Dot product.
    [[nodiscard]] friend constexpr f32 dot(Vec4 a, Vec4 b) {
        return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    }
    /// The 3D part.
    [[nodiscard]] constexpr Vec3 xyz() const { return {x, y, z}; }
};

/// Linear interpolation with exact endpoints: t=0 gives a, t=1 gives b.
[[nodiscard]] constexpr f32 lerp(f32 a, f32 b, f32 t) { return (1.0f - t) * a + t * b; }
/// Component-wise linear interpolation with exact endpoints.
[[nodiscard]] constexpr Vec3 lerp(Vec3 a, Vec3 b, f32 t) { return (1.0f - t) * a + t * b; }

/// Shortest-path interpolation of angles in radians, with exact endpoints.
[[nodiscard]] constexpr f32 angle_lerp(f32 a, f32 b, f32 t) {
    if (t <= 0.0f) {
        return a;
    }
    if (t >= 1.0f) {
        return b;
    }
    constexpr f32 PI = 3.14159265358979f;
    constexpr f32 TWO_PI = 6.28318530717959f;
    f32 d = b - a;
    while (d > PI) {
        d -= TWO_PI;
    }
    while (d < -PI) {
        d += TWO_PI;
    }
    return a + t * d;
}

}  // namespace core
