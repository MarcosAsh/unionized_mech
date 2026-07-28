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
    /// The four locomotion clips, in the order the movement angle sweeps them:
    /// forward, right, back, left. A character aims where it looks and moves
    /// where it is going, and those are rarely the same direction, so running
    /// is chosen by the angle between them rather than by speed alone.
    u32 clip_run[4];
    const char* gun_joint;  ///< Joint the weapon hangs from, by name.
    f32 gun_scale;          ///< Weapon model scaled to a carbine in world units.
    f32 gun_forward;        ///< Metres along the aim from the hand to the grip.
};

/// Quaternius SWAT: authored in metres at 1.82 tall so it needs almost no
/// scaling, facing +Z against our yaw-zero-faces-minus-Z convention. Clips are
/// alphabetical, which is why Run sits at 16 of 24 and its Back, Left and Right
/// variants follow it. The weapon hangs off the right wrist, positioned by the
/// rig but aimed by the character rather than by the hand, so a hand that rolls
/// through its run cycle cannot swing the barrel off the shot the sim fired.
constexpr TrooperRig TROOPER_RIG = {0.99f, 3.14159265f, 0,     4,      {16, 19, 17, 18},
                                    "Wrist.R", 0.7f,    0.15f};

/// Build the level geometry: the floor and the visible collision boxes, with
/// per-face normals and world-scaled texture coordinates.
void build_level(core::Array<LevelVertex>& verts, core::Array<u32>& indices);

/// The level pass surface texture. Loads the imported material if it is there
/// and falls back to a generated pattern if it is not, so the renderer still
/// comes up when the asset conversion has not run.
[[nodiscard]] u32 make_level_texture(gpu::Renderer& gpu, core::Arena& scratch);

/// Load one of the level material's data maps as a linear texture, falling back
/// to a single flat texel when the conversion has not run. `fallback` is the
/// RGBA of that texel: a flat normal, or the roughness the surface should take.
[[nodiscard]] u32 make_level_data_texture(gpu::Renderer& gpu, core::Arena& scratch,
                                          const char* path, const u8 fallback[4]);

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

/// A thrown grenade: a small dark unit sphere, scaled at queue time.
[[nodiscard]] RenderModel make_grenade(gpu::Renderer& gpu, u32 fallback_texture);

/// The fireball a grenade leaves: an over-bright unit sphere, expanded at queue
/// time over the life of the blast.
[[nodiscard]] RenderModel make_blast(gpu::Renderer& gpu, u32 fallback_texture);

/// A full-screen quad in NDC for the overlay pass, tinted at queue time.
[[nodiscard]] RenderModel make_overlay_quad(gpu::Renderer& gpu, u32 fallback_texture);

}  // namespace render
