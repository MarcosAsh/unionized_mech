#pragma once

#include "core/arena.h"
#include "core/mat.h"
#include "core/quat.h"
#include "core/result.h"
#include "core/span.h"
#include "core/types.h"
#include "core/vec.h"

namespace anim {

/// Animation clips an imported model may carry. The importer truncates past
/// this and the renderer loads exactly this many, so it lives here rather than
/// as a literal in each: a model whose locomotion clip sits past a cap that
/// only one side knows about silently loses its walk cycle.
constexpr u32 MAX_CLIPS = 32;


/// Upper bound on joints per skeleton. Game rigs run 30 to 90; 128 leaves room.
constexpr u32 MAX_JOINTS = 128;

/// Longest joint name kept from the source rig, terminator included. Anything
/// hung off a rig names the joint it hangs from, so the names have to survive
/// import: the importer reorders joints parent-before-child, which means a
/// remembered index points somewhere else the moment the rig changes.
constexpr u32 MAX_JOINT_NAME = 32;

/// A skeleton: joint hierarchy, bind-pose inverse matrices, and the rest pose.
/// # Invariants
/// Joints are stored parent-before-child, so hierarchy walks are a single
/// forward pass. parents[i] < i for every non-root joint, root parent is -1.
struct Skeleton {
    u32 joint_count = 0;
    i16 parents[MAX_JOINTS] = {};
    /// The transform of whatever sits above the root joints. Rigs are commonly
    /// authored under an armature node that carries the scale and orientation
    /// putting the mesh in world units, and that node is not itself a joint, so
    /// the hierarchy alone loses it. It cannot be folded into the root joints'
    /// rest transforms either: animation overwrites those every frame.
    core::Mat4 root_transform;
    core::Mat4 inverse_bind[MAX_JOINTS];
    core::Vec3 rest_pos[MAX_JOINTS];
    core::Quat rest_rot[MAX_JOINTS];
    core::Vec3 rest_scale[MAX_JOINTS];
    char names[MAX_JOINTS][MAX_JOINT_NAME] = {};  ///< Source rig names, truncated.
};

/// The index of the joint called `name`, or -1 when there is no such joint.
[[nodiscard]] i32 joint_index(const Skeleton& skeleton, const char* name);

/// One animated property of one joint: sorted key times and packed values,
/// three floats per key for translation and scale, four for rotation.
struct Track {
    core::Span<const f32> times;
    core::Span<const f32> values;
};

/// An animation clip: per-joint tracks over a duration in seconds. Joints
/// without a track hold their rest transform.
struct Clip {
    f32 duration = 0.0f;
    Track translation[MAX_JOINTS];
    Track rotation[MAX_JOINTS];
    Track scale[MAX_JOINTS];
};

/// A sampled pose: local TRS per joint.
struct Pose {
    core::Vec3 pos[MAX_JOINTS];
    core::Quat rot[MAX_JOINTS];
    core::Vec3 scale[MAX_JOINTS];
};

/// Fill `out` with the skeleton's rest pose.
void rest_pose(const Skeleton& skeleton, Pose* out);

/// Sample `clip` at `time` seconds into `out`, looping past the end. Linear
/// interpolation for translation and scale, normalized lerp for rotation.
void sample_clip(const Skeleton& skeleton, const Clip& clip, f32 time, Pose* out);

/// Blend two poses joint by joint: t = 0 gives a, t = 1 gives b. The leaf
/// operation of a blend tree.
void blend_poses(const Pose& a, const Pose& b, f32 t, u32 joint_count, Pose* out);

/// Compose local poses into world-space joint matrices, one forward pass.
void pose_matrices(const Skeleton& skeleton, const Pose& pose, core::Mat4* out_world);

/// Skinning matrices: world joint matrices times the inverse bind matrices.
/// These are what vertices are transformed by.
void skinning_matrices(const Skeleton& skeleton, const core::Mat4* world, core::Mat4* out_skin);

/// Write a skeleton in the native format read by skeleton_load.
/// # Errors
/// A static message when the file cannot be written.
[[nodiscard]] core::Result<core::Unit, const char*> skeleton_save(const char* path,
                                                                  const Skeleton& skeleton);

/// Load a native skeleton written by skeleton_save.
/// # Errors
/// A static message when the file is missing, truncated, or not a skeleton.
[[nodiscard]] core::Result<Skeleton, const char*> skeleton_load(core::Arena& arena,
                                                                const char* path);

/// Write a clip in the native format read by clip_load.
/// # Errors
/// A static message when the file cannot be written.
[[nodiscard]] core::Result<core::Unit, const char*> clip_save(const char* path, const Clip& clip,
                                                              u32 joint_count);

/// Load a native clip written by clip_save, backed by `arena`.
/// # Errors
/// A static message when the file is missing, truncated, or not a clip.
[[nodiscard]] core::Result<Clip, const char*> clip_load(core::Arena& arena, const char* path);

}  // namespace anim
