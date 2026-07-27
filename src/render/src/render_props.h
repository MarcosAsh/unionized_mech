#pragma once

// Internal to the render module. Procedural scene content: level geometry,
// placeholder characters, and the first-person viewmodel. All render-side
// state; nothing here touches the simulation.

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

/// A tiling surface detail texture for the level pass, generated rather than
/// loaded so the build stays free of binary content. Real material art drops
/// into the same slot.
[[nodiscard]] u32 make_level_texture(gpu::Renderer& gpu);

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
