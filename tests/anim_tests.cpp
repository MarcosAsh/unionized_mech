// Tests for the anim module: sampling, hierarchy, skinning, and file formats.

#include "anim/anim.h"
#include "core/arena.h"
#include "core/assert.h"
#include "core/log.h"
#include "core/types.h"

using namespace anim;
using core::Mat4;
using core::Quat;
using core::Vec3;

namespace {

bool near(f32 a, f32 b) {
    const f32 d = a - b;
    return (d < 0.0f ? -d : d) <= 1e-5f;
}

bool near_v(Vec3 a, Vec3 b) { return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z); }

// Two joints: a root and a child one unit up along Y in rest.
Skeleton two_joint_skeleton() {
    Skeleton s;
    s.joint_count = 2;
    s.parents[0] = -1;
    s.parents[1] = 0;
    s.rest_pos[0] = Vec3{};
    s.rest_pos[1] = Vec3{0.0f, 1.0f, 0.0f};
    s.rest_rot[0] = Quat{};
    s.rest_rot[1] = Quat{};
    s.rest_scale[0] = Vec3{1.0f, 1.0f, 1.0f};
    s.rest_scale[1] = Vec3{1.0f, 1.0f, 1.0f};
    // Inverse bind undoes the rest world transform, so bind pose skins to
    // identity.
    s.inverse_bind[0] = Mat4{};
    s.inverse_bind[1] = Mat4::translation(Vec3{0.0f, -1.0f, 0.0f});
    return s;
}

}  // namespace

static void test_sampling() {
    const Skeleton skel = two_joint_skeleton();

    const f32 times[3] = {0.0f, 1.0f, 2.0f};
    const f32 values[9] = {0.0f, 0.0f, 0.0f, 4.0f, 0.0f, 0.0f, 4.0f, 8.0f, 0.0f};
    Clip clip;
    clip.duration = 2.0f;
    clip.translation[0].times = core::Span<const f32>(times, 3);
    clip.translation[0].values = core::Span<const f32>(values, 9);

    Pose pose;
    // Exactly on keys.
    sample_clip(skel, clip, 0.0f, &pose);
    ASSERT(near_v(pose.pos[0], Vec3{0.0f, 0.0f, 0.0f}));
    sample_clip(skel, clip, 1.0f, &pose);
    ASSERT(near_v(pose.pos[0], Vec3{4.0f, 0.0f, 0.0f}));
    // Between keys.
    sample_clip(skel, clip, 0.5f, &pose);
    ASSERT(near_v(pose.pos[0], Vec3{2.0f, 0.0f, 0.0f}));
    sample_clip(skel, clip, 1.5f, &pose);
    ASSERT(near_v(pose.pos[0], Vec3{4.0f, 4.0f, 0.0f}));
    // Looped past the end: t = 2.5 wraps to 0.5.
    sample_clip(skel, clip, 2.5f, &pose);
    ASSERT(near_v(pose.pos[0], Vec3{2.0f, 0.0f, 0.0f}));
    // The untracked joint holds its rest transform.
    ASSERT(near_v(pose.pos[1], Vec3{0.0f, 1.0f, 0.0f}));
}

static void test_hierarchy_and_skinning() {
    const Skeleton skel = two_joint_skeleton();

    Pose pose;
    rest_pose(skel, &pose);

    Mat4 world[MAX_JOINTS];
    pose_matrices(skel, pose, world);
    // Child world = root world * child local.
    ASSERT(near_v(world[1].transform_point(Vec3{}), Vec3{0.0f, 1.0f, 0.0f}));

    // At bind pose, skinning matrices are identity.
    Mat4 skin[MAX_JOINTS];
    skinning_matrices(skel, world, skin);
    const Vec3 p{0.3f, 1.7f, -0.4f};
    ASSERT(near_v(skin[0].transform_point(p), p));
    ASSERT(near_v(skin[1].transform_point(p), p));

    // Move the root: everything downstream follows.
    pose.pos[0] = Vec3{5.0f, 0.0f, 0.0f};
    pose_matrices(skel, pose, world);
    ASSERT(near_v(world[1].transform_point(Vec3{}), Vec3{5.0f, 1.0f, 0.0f}));
    skinning_matrices(skel, world, skin);
    ASSERT(near_v(skin[1].transform_point(p), p + Vec3{5.0f, 0.0f, 0.0f}));
}

static void test_blend() {
    Pose a;
    Pose b;
    a.pos[0] = Vec3{0.0f, 0.0f, 0.0f};
    b.pos[0] = Vec3{2.0f, 4.0f, 6.0f};
    a.rot[0] = Quat{};
    b.rot[0] = Quat{};
    a.scale[0] = Vec3{1.0f, 1.0f, 1.0f};
    b.scale[0] = Vec3{1.0f, 1.0f, 1.0f};

    Pose out;
    blend_poses(a, b, 0.0f, 1, &out);
    ASSERT(near_v(out.pos[0], a.pos[0]));
    blend_poses(a, b, 1.0f, 1, &out);
    ASSERT(near_v(out.pos[0], b.pos[0]));
    blend_poses(a, b, 0.5f, 1, &out);
    ASSERT(near_v(out.pos[0], Vec3{1.0f, 2.0f, 3.0f}));
}

static void test_round_trips() {
    core::Arena arena = core::Arena::with_capacity(1u << 20);
    const Skeleton skel = two_joint_skeleton();

    ASSERT(skeleton_save("anim_tests_skel.tmp", skel).is_ok());
    core::Result<Skeleton, const char*> skel_back = skeleton_load(arena, "anim_tests_skel.tmp");
    ASSERT(skel_back.is_ok());
    ASSERT(skel_back.value().joint_count == 2);
    ASSERT(skel_back.value().parents[1] == 0);
    ASSERT(near_v(skel_back.value().rest_pos[1], Vec3{0.0f, 1.0f, 0.0f}));

    const f32 times[2] = {0.0f, 1.0f};
    const f32 tvals[6] = {0.0f, 0.0f, 0.0f, 1.0f, 2.0f, 3.0f};
    const f32 rvals[8] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    Clip clip;
    clip.duration = 1.0f;
    clip.translation[1].times = core::Span<const f32>(times, 2);
    clip.translation[1].values = core::Span<const f32>(tvals, 6);
    clip.rotation[1].times = core::Span<const f32>(times, 2);
    clip.rotation[1].values = core::Span<const f32>(rvals, 8);

    ASSERT(clip_save("anim_tests_clip.tmp", clip, 2).is_ok());
    core::Result<Clip, const char*> clip_back = clip_load(arena, "anim_tests_clip.tmp");
    ASSERT(clip_back.is_ok());
    const Clip& back = clip_back.value();
    ASSERT(back.duration == 1.0f);
    ASSERT(back.translation[1].times.size() == 2);
    ASSERT(back.translation[1].values[4] == 2.0f);
    ASSERT(back.rotation[1].values.size() == 8);
    ASSERT(back.translation[0].times.empty());

    // A skeleton file is not a clip and must be rejected by magic.
    ASSERT(clip_load(arena, "anim_tests_skel.tmp").is_err());
    ASSERT(skeleton_load(arena, "anim_tests_clip.tmp").is_err());
}

int main() {
    test_sampling();
    test_hierarchy_and_skinning();
    test_blend();
    test_round_trips();
    core::log_info("anim_tests: all passed");
    return 0;
}
