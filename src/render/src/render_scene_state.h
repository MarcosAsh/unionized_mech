#pragma once

// Internal to the render module. The scene's model set and shared GPU
// resources, visible to both scene setup and the per-frame draw.

#include "render_model.h"
#include "render_text.h"

#include <gpu/gpu.h>

#include <cmath>

namespace render {

// Presentation tuning and helpers shared by the queue phase and the passes.
// Kept together so the camera, the bodies and the viewmodel cannot drift into
// disagreeing about which way a wallrunner leans or how fast the lean arrives.

/// How far a wallrunning body banks into the wall, in radians. The rig has no
/// wallrun clip, so the run cycle keeps doing the legs and the lean carries the
/// read. Larger than the camera's own tilt because a body seen from outside has
/// to sell the pose on its silhouette alone.
constexpr f32 WALLRUN_BANK = 0.40f;

/// How fast the wall leans ease in and out. Shared so the camera, the bodies
/// and the viewmodel all arrive together.
constexpr f32 LEAN_EASE = 0.2f;

/// Which side of a character a nearby wall is on: positive to their left,
/// negative to their right, tapering to zero as they turn to face it head on.
/// Every wall lean comes through here, so the camera, the bodies, and the
/// viewmodel cannot disagree about which way to tilt.
inline f32 wall_side(f32 yaw, f32 wall_nx, f32 wall_nz) {
    const f32 facing_x = std::sin(yaw);
    const f32 facing_z = -std::cos(yaw);
    return facing_x * wall_nz - facing_z * wall_nx;
}

/// One character's animation state for a frame: which two clips it is between
/// and how far. Written by the queue phase and read afterwards by the skinning
/// and acceleration-structure work, so it outlives the queue and lives here.
struct TrooperPose {
    u32 clip_a = 0;
    u32 clip_b = 0;
    f32 blend = 0.0f;
    f32 time = 0.0f;
    bool posed = false;
};

/// The imported models and shared buffers, held in the permanent arena so the
/// public header can keep them behind a forward declaration.
struct SceneModels {
    RenderModel gun;
    RenderModel bot_gun;  ///< The same weapon in world space, held by bots.
    RenderModel viewmodel;
    SkinnedModel trooper;
    RenderModel mech;
    RenderModel tracer;
    RenderModel tracer_vm;
    RenderModel grenade;
    RenderModel blast;
    RenderModel hitmarker;
    RenderModel overlay;
    gpu::Blas level_blas;
    gpu::Blas trooper_blas;
    gpu::Blas mech_blas;
    gpu::Buffer globals;
    SceneGlobals* globals_mapped = nullptr;
    Font font{};
    u32 white_texture = 0;
};

/// Modification time of `path` in nanoseconds, zero when missing. Drives the
/// shader and map live-reload polls.
[[nodiscard]] i64 scene_file_mtime(const char* path);

}  // namespace render
