#include "asset_bounds.h"

#include "core/mat.h"

// Posed bounds. Split from asset_import.cpp, which had grown past the project's
// file limit.

namespace asset {

namespace {

// Samples per clip when sweeping the posed bounds. Twelve catches the extremes
// of the kicks and rolls in the character set; the sweep is the whole mesh per
// sample, so this is also what the import costs.
constexpr u32 POSE_SAMPLES = 12;

// Skin every vertex by one pose.
void pose_positions(const anim::Skeleton& skeleton, const anim::Pose& pose,
                    core::Span<const MeshVertex> vertices, core::Span<const SkinVertex> skin,
                    core::Span<core::Vec3> out) {
    core::Mat4 world[anim::MAX_JOINTS];
    core::Mat4 matrices[anim::MAX_JOINTS];
    anim::pose_matrices(skeleton, pose, world);
    anim::skinning_matrices(skeleton, world, matrices);

    for (u64 i = 0; i < vertices.size(); ++i) {
        const core::Vec3 bind = vertices[i].pos;
        core::Vec3 p{0.0f, 0.0f, 0.0f};
        for (u32 k = 0; k < 4; ++k) {
            if (skin[i].weights[k] == 0.0f) {
                continue;
            }
            p += matrices[skin[i].joints[k]].transform_point(bind) * skin[i].weights[k];
        }
        out[i] = p;
    }
}

// Grow each submesh's box over the vertices its own indices reach. `seed`
// replaces the box instead of growing it, for the first pose of a sweep.
void grow_submesh_bounds(core::Span<const u32> indices, core::Span<const core::Vec3> posed,
                         bool seed, core::Span<Submesh> submeshes) {
    for (u64 s = 0; s < submeshes.size(); ++s) {
        Submesh& sub = submeshes[s];
        for (u64 k = 0; k < sub.index_count; ++k) {
            const core::Vec3 p = posed[indices[sub.index_offset + k]];
            if (seed && k == 0) {
                sub.bounds_min[0] = p.x;
                sub.bounds_min[1] = p.y;
                sub.bounds_min[2] = p.z;
                sub.bounds_max[0] = p.x;
                sub.bounds_max[1] = p.y;
                sub.bounds_max[2] = p.z;
            }
            sub.bounds_min[0] = p.x < sub.bounds_min[0] ? p.x : sub.bounds_min[0];
            sub.bounds_min[1] = p.y < sub.bounds_min[1] ? p.y : sub.bounds_min[1];
            sub.bounds_min[2] = p.z < sub.bounds_min[2] ? p.z : sub.bounds_min[2];
            sub.bounds_max[0] = p.x > sub.bounds_max[0] ? p.x : sub.bounds_max[0];
            sub.bounds_max[1] = p.y > sub.bounds_max[1] ? p.y : sub.bounds_max[1];
            sub.bounds_max[2] = p.z > sub.bounds_max[2] ? p.z : sub.bounds_max[2];
        }
    }
}

// Replace a rigged model's bind-pose boxes with the box its vertices actually
// occupy once posed, swept over the rest pose and every clip. A fully skinned
// mesh's bind-pose box is near-degenerate — the joints are what place its
}  // namespace

// vertices — so culling against it hides the model from almost everywhere, and
// no amount of guessed padding is the real answer.
void sweep_posed_bounds(core::Arena& arena, const anim::Skeleton& skeleton,
                        core::Span<const anim::Clip> clips, core::Span<const MeshVertex> vertices,
                        core::Span<const SkinVertex> skin, core::Span<const u32> indices,
                        core::Span<Submesh> submeshes) {
    const core::Span<core::Vec3> scratch = arena.alloc_n<core::Vec3>(vertices.size());
    const core::Span<const core::Vec3> posed(scratch.data(), scratch.size());
    anim::Pose pose;

    anim::rest_pose(skeleton, &pose);
    pose_positions(skeleton, pose, vertices, skin, scratch);
    grow_submesh_bounds(indices, posed, true, submeshes);

    for (u64 c = 0; c < clips.size(); ++c) {
        for (u32 s = 0; s < POSE_SAMPLES; ++s) {
            const f32 t = clips[c].duration * (static_cast<f32>(s) / POSE_SAMPLES);
            anim::sample_clip(skeleton, clips[c], t, &pose);
            pose_positions(skeleton, pose, vertices, skin, scratch);
            grow_submesh_bounds(indices, posed, false, submeshes);
        }
    }
}

// Decode one glTF image, from its buffer view or from a URI beside the file.

}  // namespace asset
