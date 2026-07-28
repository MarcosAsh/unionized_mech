#include "render_props.h"

#include "asset/asset.h"

// Internal to the render module. The small procedural balls: a thrown grenade
// and the fireball it leaves. Split from render_props.cpp to keep both files
// inside the project's size limit.

namespace render {

namespace {

/// A unit sphere about the origin, smooth shaded: the normal is the position,
/// which is what a sphere at the origin gives you for free. Queued at whatever
/// radius the caller wants through the model scale.
void add_mesh_sphere(asset::MeshVertex* verts, u32* indices, u32* vert_cursor,
                     u32* index_cursor, u32 rings, u32 segments) {
    const u32 base = *vert_cursor;
    for (u32 r = 0; r <= rings; ++r) {
        const f32 phi = 3.14159265f * static_cast<f32>(r) / static_cast<f32>(rings);
        const f32 sp = __builtin_sinf(phi);
        const f32 cp = __builtin_cosf(phi);
        for (u32 s = 0; s <= segments; ++s) {
            const f32 theta =
                6.28318531f * static_cast<f32>(s) / static_cast<f32>(segments);
            const core::Vec3 p{sp * __builtin_sinf(theta), cp, sp * __builtin_cosf(theta)};
            asset::MeshVertex vert{};
            vert.pos = p;
            vert.normal = p;
            vert.u = static_cast<f32>(s) / static_cast<f32>(segments);
            vert.v = static_cast<f32>(r) / static_cast<f32>(rings);
            verts[(*vert_cursor)++] = vert;
        }
    }
    for (u32 r = 0; r < rings; ++r) {
        for (u32 s = 0; s < segments; ++s) {
            const u32 a = base + r * (segments + 1) + s;
            const u32 b = a + segments + 1;
            const u32 quad[6] = {a, b, a + 1, a + 1, b, b + 1};
            for (u32 k = 0; k < 6; ++k) {
                indices[(*index_cursor)++] = quad[k];
            }
        }
    }
}

/// One untextured submesh spanning the whole mesh, coloured by `color`, with
/// unit-cube bounds. Both ball models are unit spheres, so the bounds are the
/// same for each and the culler scales them by the queued transform.
[[nodiscard]] asset::Submesh unit_ball_submesh(u32 index_count, const f32 color[3]) {
    asset::Submesh sub{};
    sub.index_count = index_count;
    sub.texture = asset::NO_TEXTURE;
    for (u32 i = 0; i < 3; ++i) {
        sub.color[i] = color[i];
        sub.bounds_min[i] = -1.0f;
        sub.bounds_max[i] = 1.0f;
    }
    return sub;
}

}  // namespace

RenderModel make_grenade(gpu::Renderer& gpu, u32 fallback_texture) {
    static asset::MeshVertex verts[8 * 13 + 16];
    static u32 indices[7 * 12 * 6];
    u32 vc = 0;
    u32 ic = 0;
    add_mesh_sphere(verts, indices, &vc, &ic, 7, 12);
    // Dark and matte: a live grenade reads by moving against the arena, and a
    // bright one would be mistaken for a tracer.
    const f32 color[3] = {0.16f, 0.19f, 0.13f};
    static asset::Submesh sub;
    sub = unit_ball_submesh(ic, color);
    sub.roughness = 0.7f;
    asset::MeshData mesh;
    mesh.vertices = core::Span<const asset::MeshVertex>(verts, vc);
    mesh.indices = core::Span<const u32>(indices, ic);
    mesh.submeshes = core::Span<const asset::Submesh>(&sub, 1);
    return model_from_data(gpu, mesh, fallback_texture);
}

RenderModel make_blast(gpu::Renderer& gpu, u32 fallback_texture) {
    static asset::MeshVertex verts[10 * 17 + 32];
    static u32 indices[9 * 16 * 6];
    u32 vc = 0;
    u32 ic = 0;
    add_mesh_sphere(verts, indices, &vc, &ic, 9, 16);
    // Over-bright, like the tracer, so the shading cannot dim the fireball into
    // just another grey ball.
    const f32 color[3] = {6.0f, 2.4f, 0.7f};
    static asset::Submesh sub;
    sub = unit_ball_submesh(ic, color);
    asset::MeshData mesh;
    mesh.vertices = core::Span<const asset::MeshVertex>(verts, vc);
    mesh.indices = core::Span<const u32>(indices, ic);
    mesh.submeshes = core::Span<const asset::Submesh>(&sub, 1);
    return model_from_data(gpu, mesh, fallback_texture);
}

}  // namespace render
