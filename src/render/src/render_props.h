#pragma once

// Internal to the render module. Procedural scene content: level geometry,
// placeholder characters, and the first-person viewmodel. All render-side
// state; nothing here touches the simulation.

#include "core/arena.h"
#include "core/array.h"
#include "core/quat.h"
#include "core/types.h"
#include "core/vec.h"
#include "render_model.h"

namespace render {

/// Level pass vertex, matching the Vertex struct in shaders/scene.vert. Padded
/// to vec4s so the std430 stride needs no thought.
struct LevelVertex {
    f32 pos[4];
    f32 color[4];
    f32 normal[4];
    f32 uv[4];  ///< xy are the texture coordinates; zw pad the stride.
};

/// Everything the renderer assumes about the imported character art: how big it
/// is, which way it faces, and which animation clip is which. All of it belongs
/// to the specific asset, so swapping the model is an edit here and nowhere
/// else. Clips are indexed by integer in the file's own order.
struct TrooperRig {
    f32 scale;       ///< Model units scaled into the sim's 1.8m hull.
    f32 yaw_offset;  ///< Added to sim yaw to meet the model's forward axis.
    u32 clip_death;
    u32 clip_idle;
    u32 clip_run;
    const char* gun_joint;  ///< Joint the weapon hangs from, by name.
    f32 gun_scale;          ///< Weapon model scaled to a carbine in world units.
    f32 gun_forward;        ///< Metres along the aim from the hand to the grip.
};

/// Quaternius SWAT: authored in metres at 1.82 tall so it needs almost no
/// scaling, facing +Z against our yaw-zero-faces-minus-Z convention. Clips are
/// alphabetical, which is why Run sits at 16 of 24. The weapon hangs off the
/// right wrist, positioned by the rig but aimed by the character rather than by
/// the hand, so a hand that rolls through its run cycle cannot swing the barrel
/// off the shot the sim actually fired.
constexpr TrooperRig TROOPER_RIG = {0.99f, 3.14159265f, 0, 4, 16, "Wrist.R", 0.7f, 0.15f};

/// Decorative duck placements on the plaza.
struct DuckSpot {
    core::Vec3 pos;
    f32 yaw;
    f32 scale;
};

constexpr DuckSpot DUCKS[3] = {
    {{4.0f, 0.0f, 4.0f}, 0.6f, 1.5f},
    {{-6.0f, 2.0f, 6.0f}, 2.4f, 1.0f},
    {{2.0f, 0.0f, -9.0f}, -1.2f, 2.5f},
};

/// Build the level geometry: the floor and the visible collision boxes, with
/// per-face normals and world-scaled texture coordinates.
void build_level(core::Array<LevelVertex>& verts, core::Array<u32>& indices);

/// The level pass surface texture. Loads the imported material if it is there
/// and falls back to a generated pattern if it is not, so the renderer still
/// comes up when the asset conversion has not run.
[[nodiscard]] u32 make_level_texture(gpu::Renderer& gpu, core::Arena& scratch);

/// A placeholder first-person weapon made of boxes. Establishes the viewmodel
/// system that real arm art replaces later.
[[nodiscard]] RenderModel make_viewmodel(gpu::Renderer& gpu, u32 fallback_texture);

/// A placeholder soldier made of boxes, tinted per team at queue time. Real
/// character art replaces it later.
[[nodiscard]] RenderModel make_trooper(gpu::Renderer& gpu, u32 fallback_texture);

/// The union mech chassis: a heavy biped of boxes matching the sim's mech
/// hull, tinted neutral when dormant and by team when piloted.
[[nodiscard]] RenderModel make_mech(gpu::Renderer& gpu, u32 fallback_texture);

/// A unit tracer beam: a thin bright box from the origin one unit along -Z,
/// stretched to the shot at queue time.
[[nodiscard]] RenderModel make_tracer(gpu::Renderer& gpu, u32 fallback_texture);

/// Four diagonal hit-marker ticks around the screen centre, camera space.
[[nodiscard]] RenderModel make_hitmarker(gpu::Renderer& gpu, u32 fallback_texture);

/// A full-screen quad in NDC for the overlay pass, tinted at queue time.
[[nodiscard]] RenderModel make_overlay_quad(gpu::Renderer& gpu, u32 fallback_texture);

}  // namespace render
