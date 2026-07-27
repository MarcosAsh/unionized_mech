#include "anim/anim.h"

namespace anim {

namespace {

// Index of the last key at or before `time`, given sorted key times.
u64 key_before(core::Span<const f32> times, f32 time) {
    u64 index = 0;
    while (index + 1 < times.size() && times[index + 1] <= time) {
        ++index;
    }
    return index;
}

// Interpolation factor between key k and k+1 at `time`.
f32 key_alpha(core::Span<const f32> times, u64 k, f32 time) {
    if (k + 1 >= times.size()) {
        return 0.0f;
    }
    const f32 span = times[k + 1] - times[k];
    if (span <= 0.0f) {
        return 0.0f;
    }
    f32 alpha = (time - times[k]) / span;
    if (alpha < 0.0f) {
        alpha = 0.0f;
    }
    if (alpha > 1.0f) {
        alpha = 1.0f;
    }
    return alpha;
}

core::Vec3 sample_vec3(const Track& track, f32 time, core::Vec3 fallback) {
    if (track.times.empty()) {
        return fallback;
    }
    const u64 k = key_before(track.times, time);
    const f32 a = key_alpha(track.times, k, time);
    const core::Vec3 v0{track.values[k * 3], track.values[k * 3 + 1], track.values[k * 3 + 2]};
    if (a == 0.0f) {
        return v0;
    }
    const core::Vec3 v1{track.values[(k + 1) * 3], track.values[(k + 1) * 3 + 1],
                        track.values[(k + 1) * 3 + 2]};
    return core::lerp(v0, v1, a);
}

core::Quat sample_quat(const Track& track, f32 time, core::Quat fallback) {
    if (track.times.empty()) {
        return fallback;
    }
    const u64 k = key_before(track.times, time);
    const f32 a = key_alpha(track.times, k, time);
    const core::Quat q0{track.values[k * 4], track.values[k * 4 + 1], track.values[k * 4 + 2],
                        track.values[k * 4 + 3]};
    if (a == 0.0f) {
        return q0;
    }
    const core::Quat q1{track.values[(k + 1) * 4], track.values[(k + 1) * 4 + 1],
                        track.values[(k + 1) * 4 + 2], track.values[(k + 1) * 4 + 3]};
    return nlerp(q0, q1, a);
}

}  // namespace

void rest_pose(const Skeleton& skeleton, Pose* out) {
    for (u32 j = 0; j < skeleton.joint_count; ++j) {
        out->pos[j] = skeleton.rest_pos[j];
        out->rot[j] = skeleton.rest_rot[j];
        out->scale[j] = skeleton.rest_scale[j];
    }
}

void sample_clip(const Skeleton& skeleton, const Clip& clip, f32 time, Pose* out) {
    f32 t = time;
    if (clip.duration > 0.0f) {
        while (t >= clip.duration) {
            t -= clip.duration;
        }
        while (t < 0.0f) {
            t += clip.duration;
        }
    }
    for (u32 j = 0; j < skeleton.joint_count; ++j) {
        out->pos[j] = sample_vec3(clip.translation[j], t, skeleton.rest_pos[j]);
        out->rot[j] = sample_quat(clip.rotation[j], t, skeleton.rest_rot[j]);
        out->scale[j] = sample_vec3(clip.scale[j], t, skeleton.rest_scale[j]);
    }
}

void blend_poses(const Pose& a, const Pose& b, f32 t, u32 joint_count, Pose* out) {
    for (u32 j = 0; j < joint_count; ++j) {
        out->pos[j] = core::lerp(a.pos[j], b.pos[j], t);
        out->rot[j] = nlerp(a.rot[j], b.rot[j], t);
        out->scale[j] = core::lerp(a.scale[j], b.scale[j], t);
    }
}

void pose_matrices(const Skeleton& skeleton, const Pose& pose, core::Mat4* out_world) {
    for (u32 j = 0; j < skeleton.joint_count; ++j) {
        core::Mat4 local = core::Mat4::trs(pose.pos[j], pose.rot[j]);
        local = local * core::Mat4::scale(pose.scale[j]);
        // Parents precede children, so the parent's world matrix is ready. A
        // root joint stands on the transform the rig hangs from instead.
        out_world[j] = skeleton.parents[j] >= 0
                           ? out_world[skeleton.parents[j]] * local
                           : skeleton.root_transform * local;
    }
}

void skinning_matrices(const Skeleton& skeleton, const core::Mat4* world, core::Mat4* out_skin) {
    for (u32 j = 0; j < skeleton.joint_count; ++j) {
        out_skin[j] = world[j] * skeleton.inverse_bind[j];
    }
}

}  // namespace anim
