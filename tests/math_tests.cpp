// Tests for the core math value types. Checks are plain ASSERTs with exact
// expected values, since every operation here is deterministic by design.

#include "core/assert.h"
#include "core/log.h"
#include "core/mat.h"
#include "core/quat.h"
#include "core/types.h"
#include "core/vec.h"

using namespace core;

static bool near(f32 a, f32 b) {
    const f32 d = a - b;
    return (d < 0.0f ? -d : d) <= 1e-6f;
}

static bool near_v(Vec3 a, Vec3 b) { return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z); }

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

    // Angle interpolation keeps exact endpoints even across the wrap, so
    // rendering a snapshot pair at alpha 0 or 1 reproduces it exactly.
    ASSERT(angle_lerp(0.5f, 2.5f, 0.0f) == 0.5f);
    ASSERT(angle_lerp(0.5f, 2.5f, 1.0f) == 2.5f);
    ASSERT(angle_lerp(3.0f, -3.0f, 0.0f) == 3.0f);
    ASSERT(angle_lerp(3.0f, -3.0f, 1.0f) == -3.0f);
}

static void test_quat_rotation() {
    const Vec3 x{1.0f, 0.0f, 0.0f};
    const Vec3 y{0.0f, 1.0f, 0.0f};
    const Vec3 z{0.0f, 0.0f, 1.0f};

    // Identity leaves vectors alone, exactly.
    ASSERT((Quat{}.rotate(x) == x));

    // 180 degrees about z: half angle 90, sin 1, cos 0. Exact arithmetic.
    const Quat half_turn = Quat::from_axis_half(z, 1.0f, 0.0f);
    ASSERT((half_turn.rotate(x) == -x));
    ASSERT((half_turn.rotate(y) == -y));
    ASSERT((half_turn.rotate(z) == z));

    // 90 degrees about z takes x to y (within float rounding).
    const f32 s = 0.70710678f;
    const Quat quarter = Quat::from_axis_half(z, s, s);
    ASSERT(near_v(quarter.rotate(x), y));
    ASSERT(near_v(quarter.rotate(y), -x));

    // Conjugate reverses, product composes.
    ASSERT(near_v(quarter.conjugate().rotate(quarter.rotate(x)), x));
    ASSERT(near_v((quarter * quarter).rotate(x), half_turn.rotate(x)));
}

static void test_quat_nlerp() {
    const Quat a{};
    const Quat b = Quat::from_axis_half(Vec3{0.0f, 0.0f, 1.0f}, 1.0f, 0.0f);
    ASSERT((nlerp(a, b, 0.0f) == a));
    // Endpoint may come back sign-flipped, which is the same rotation.
    const Quat e = nlerp(a, b, 1.0f);
    ASSERT((e == b) || (e == Quat{-b.x, -b.y, -b.z, -b.w}));
    // Any midpoint stays unit length.
    ASSERT(near(nlerp(a, b, 0.5f).length_sq(), 1.0f));
}

static void test_mat4() {
    const Vec3 p{1.0f, 2.0f, 3.0f};

    // Identity and translation, exact.
    ASSERT((Mat4{}.transform_point(p) == p));
    const Mat4 t = Mat4::translation(Vec3{10.0f, 20.0f, 30.0f});
    ASSERT((t.transform_point(p) == Vec3{11.0f, 22.0f, 33.0f}));
    ASSERT((t.transform_dir(p) == p));

    // Scale, exact.
    ASSERT((Mat4::scale(Vec3{2.0f, 3.0f, 4.0f}).transform_point(p) == Vec3{2.0f, 6.0f, 12.0f}));

    // Product applies right-hand side first.
    const Mat4 s = Mat4::scale(Vec3{2.0f, 2.0f, 2.0f});
    ASSERT(((t * s).transform_point(p) == t.transform_point(s.transform_point(p))));

    // Rotation matrix from a quat matches quat rotation.
    const f32 h = 0.70710678f;
    const Quat q = Quat::from_axis_half(Vec3{0.0f, 1.0f, 0.0f}, h, h);
    ASSERT(near_v(Mat4::from_quat(q).transform_dir(p), q.rotate(p)));

    // trs is rotate then translate.
    const Mat4 model = Mat4::trs(Vec3{5.0f, 0.0f, 0.0f}, q);
    ASSERT(near_v(model.transform_point(p), q.rotate(p) + Vec3{5.0f, 0.0f, 0.0f}));

    // Transpose is an involution.
    ASSERT((model.transposed().transposed() == model));
}

int main() {
    test_vec3_algebra();
    test_vec3_cross_handedness();
    test_vec3_length_normalize();
    test_vec2_vec4();
    test_lerp_endpoints();
    test_quat_rotation();
    test_quat_nlerp();
    test_mat4();
    core::log_info("math_tests: all passed");
    return 0;
}
