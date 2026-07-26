#pragma once

// Internal to the render module. Procedural scene content and the cosmetic
// actors that dress the world: level geometry, blob shadows, the fox companion
// steering, and the first-person viewmodel. All render-side state; nothing
// here touches the simulation.

#include "core/array.h"
#include "core/quat.h"
#include "core/types.h"
#include "core/vec.h"
#include "render_model.h"

namespace render {

/// Level pass vertex: position and colour, matching shaders/scene.vert.
struct LevelVertex {
    f32 pos[4];
    f32 color[4];
};

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

/// The Sponza atrium, placed south of the plaza. Collision boxes matching its
/// main walls live in sim's level so it is walkable and wallrunnable.
constexpr core::Vec3 SPONZA_POS{0.0f, 0.0f, -90.0f};

/// Build the level geometry: the checkered floor with its walkway and the
/// visible collision boxes.
void build_level(core::Array<LevelVertex>& verts, core::Array<u32>& indices);

/// A placeholder first-person weapon made of boxes. Establishes the viewmodel
/// system that real arm art replaces later.
[[nodiscard]] RenderModel make_viewmodel(gpu::Renderer& gpu, u32 fallback_texture);

/// A placeholder soldier made of boxes, tinted per team at queue time. Real
/// character art replaces it later.
[[nodiscard]] RenderModel make_trooper(gpu::Renderer& gpu, u32 fallback_texture);

/// A unit tracer beam: a thin bright box from the origin one unit along -Z,
/// stretched to the shot at queue time.
[[nodiscard]] RenderModel make_tracer(gpu::Renderer& gpu, u32 fallback_texture);

/// Four diagonal hit-marker ticks around the screen centre, camera space.
[[nodiscard]] RenderModel make_hitmarker(gpu::Renderer& gpu, u32 fallback_texture);

/// A full-screen quad in NDC for the overlay pass, tinted at queue time.
[[nodiscard]] RenderModel make_overlay_quad(gpu::Renderer& gpu, u32 fallback_texture);

/// The fox companion's steering state, advanced per frame on the render side.
struct FoxCompanion {
    f32 x = 6.0f;
    f32 z = 8.0f;
    f32 yaw = 0.0f;
    f32 speed = 0.0f;
};

/// Steer the fox toward the player: approach to a comfortable distance, ease
/// speed and heading. `dt` is the frame's animation-time step.
void fox_companion_update(FoxCompanion& fox, f32 player_x, f32 player_z, f32 dt);

/// Map the fox's speed onto its clip pair and blend: Survey when idle, Walk
/// when strolling, Run when chasing. The 1D blend space of a blend tree.
void fox_gait(f32 speed, u32* clip_a, u32* clip_b, f32* blend);

}  // namespace render
