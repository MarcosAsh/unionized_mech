// Tests for the core math value types. Checks are plain ASSERTs with exact
// expected values, since every operation here is deterministic by design.

#include "core/assert.h"
#include "core/log.h"
#include "core/types.h"
#include "core/vec.h"

using namespace core;

static void test_vec3_algebra() {
    const Vec3 a{1.0f, 2.0f, 3.0f};
    const Vec3 b{4.0f, 5.0f, 6.0f};

    ASSERT((a + b == Vec3{5.0f, 7.0f, 9.0f}));
    ASSERT((b - a == Vec3{3.0f, 3.0f, 3.0f}));
    ASSERT((a * 2.0f == Vec3{2.0f, 4.0f, 6.0f}));
    ASSERT((2.0f * a == a * 2.0f));
    ASSERT((-a == Vec3{-1.0f, -2.0f, -3.0f}));
    ASSERT(dot(a, b) == 32.0f);

    Vec3 c = a;
    c += b;
    ASSERT((c == Vec3{5.0f, 7.0f, 9.0f}));
    c *= 0.5f;
    ASSERT((c == Vec3{2.5f, 3.5f, 4.5f}));
}

static void test_vec3_cross_handedness() {
    const Vec3 x{1.0f, 0.0f, 0.0f};
    const Vec3 y{0.0f, 1.0f, 0.0f};
    const Vec3 z{0.0f, 0.0f, 1.0f};

    // Right-handed: x cross y = z, and cyclic.
    ASSERT((cross(x, y) == z));
    ASSERT((cross(y, z) == x));
    ASSERT((cross(z, x) == y));
    ASSERT((cross(y, x) == -z));
    // Parallel vectors cross to zero.
    ASSERT((cross(x, x) == Vec3{}));
}

static void test_vec3_length_normalize() {
    const Vec3 v{3.0f, 4.0f, 0.0f};
    ASSERT(v.length_sq() == 25.0f);
    ASSERT(v.length() == 5.0f);

    const Vec3 n = v.normalized();
    ASSERT((n == Vec3{0.6f, 0.8f, 0.0f}));
    ASSERT((Vec3{}.normalized() == Vec3{}));
}

static void test_vec2_vec4() {
    const Vec2 a2{3.0f, 4.0f};
    ASSERT(a2.length() == 5.0f);
    ASSERT(dot(a2, Vec2{1.0f, 1.0f}) == 7.0f);

    const Vec4 a4{1.0f, 2.0f, 3.0f, 4.0f};
    ASSERT(dot(a4, a4) == 30.0f);
    ASSERT((a4.xyz() == Vec3{1.0f, 2.0f, 3.0f}));
}

static void test_lerp_endpoints() {
    ASSERT(lerp(2.0f, 5.0f, 0.0f) == 2.0f);
    ASSERT(lerp(2.0f, 5.0f, 1.0f) == 5.0f);
    const Vec3 a{1.0f, 2.0f, 3.0f};
    const Vec3 b{4.0f, 5.0f, 6.0f};
    ASSERT((lerp(a, b, 0.0f) == a));
    ASSERT((lerp(a, b, 1.0f) == b));
    ASSERT((lerp(a, b, 0.5f) == Vec3{2.5f, 3.5f, 4.5f}));
}

int main() {
    test_vec3_algebra();
    test_vec3_cross_handedness();
    test_vec3_length_normalize();
    test_vec2_vec4();
    test_lerp_endpoints();
    core::log_info("math_tests: all passed");
    return 0;
}
