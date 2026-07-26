#include "render_props.h"

#include "sim/sim.h"

#include <cmath>

namespace render {

namespace {

void add_level_vertex(core::Array<LevelVertex>& verts, f32 x, f32 y, f32 z, f32 r, f32 g, f32 b) {
    LevelVertex v;
    v.pos[0] = x;
    v.pos[1] = y;
    v.pos[2] = z;
    v.pos[3] = 1.0f;
    v.color[0] = r;
    v.color[1] = g;
    v.color[2] = b;
    v.color[3] = 1.0f;
    verts.push(v);
}

void add_quad(core::Array<LevelVertex>& verts, core::Array<u32>& indices, f32 x, f32 z, f32 cell,
              f32 r, f32 g, f32 b) {
    const u32 base = static_cast<u32>(verts.size());
    add_level_vertex(verts, x, 0.0f, z, r, g, b);
    add_level_vertex(verts, x + cell, 0.0f, z, r, g, b);
    add_level_vertex(verts, x + cell, 0.0f, z + cell, r, g, b);
    add_level_vertex(verts, x, 0.0f, z + cell, r, g, b);
    const u32 quad[6] = {base, base + 1, base + 2, base, base + 2, base + 3};
    for (u32 i = 0; i < 6; ++i) {
        indices.push(quad[i]);
    }
}

void add_level_box(core::Array<LevelVertex>& verts, core::Array<u32>& indices, f32 x0, f32 y0,
                   f32 z0, f32 x1, f32 y1, f32 z1, f32 r, f32 g, f32 b) {
    const u32 base = static_cast<u32>(verts.size());
    const f32 xs[8] = {x0, x1, x1, x0, x0, x1, x1, x0};
    const f32 ys[8] = {y0, y0, y1, y1, y0, y0, y1, y1};
    const f32 zs[8] = {z0, z0, z0, z0, z1, z1, z1, z1};
    for (u32 i = 0; i < 8; ++i) {
        add_level_vertex(verts, xs[i], ys[i], zs[i], r, g, b);
    }
    const u32 faces[36] = {0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6, 4, 0, 3, 4, 3, 7,
                           1, 5, 6, 1, 6, 2, 3, 2, 6, 3, 6, 7, 4, 5, 1, 4, 1, 0};
    for (u32 i = 0; i < 36; ++i) {
        indices.push(base + faces[i]);
    }
}

// A solid box with per-face normals appended to a mesh under construction.
void add_mesh_box(asset::MeshVertex* verts, u32* indices, u32* vert_cursor, u32* index_cursor,
                  core::Vec3 center, core::Vec3 half) {
    const core::Vec3 n[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    const core::Vec3 u[6] = {{0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {-1, 0, 0}};
    for (u32 f = 0; f < 6; ++f) {
        const core::Vec3 v = cross(n[f], u[f]);
        const u32 base = *vert_cursor;
        const f32 su[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
        const f32 sv[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
        for (u32 k = 0; k < 4; ++k) {
            asset::MeshVertex vert{};
            const core::Vec3 offset = n[f] * (half.x * n[f].x + half.y * n[f].y + half.z * n[f].z);
            core::Vec3 p = center;
            p += core::Vec3{n[f].x * half.x, n[f].y * half.y, n[f].z * half.z};
            p += core::Vec3{u[f].x * half.x, u[f].y * half.y, u[f].z * half.z} * su[k];
            p += core::Vec3{v.x * half.x, v.y * half.y, v.z * half.z} * sv[k];
            (void)offset;
            vert.pos = p;
            vert.normal = n[f];
            verts[(*vert_cursor)++] = vert;
        }
        const u32 quad[6] = {base, base + 1, base + 2, base, base + 2, base + 3};
        for (u32 k = 0; k < 6; ++k) {
            indices[(*index_cursor)++] = quad[k];
        }
    }
}

}  // namespace

void build_level(core::Array<LevelVertex>& verts, core::Array<u32>& indices) {
    constexpr i32 N = 50;
    constexpr f32 CELL = 2.0f;
    const f32 half = static_cast<f32>(N) * CELL * 0.5f;
    for (i32 i = 0; i < N; ++i) {
        for (i32 j = 0; j < N; ++j) {
            const f32 x = -half + static_cast<f32>(i) * CELL;
            const f32 z = -half + static_cast<f32>(j) * CELL;
            const bool light = ((i + j) & 1) != 0;
            const f32 r = light ? 0.38f : 0.24f;
            add_quad(verts, indices, x, z, CELL, r, r, r + 0.04f);
        }
    }

    const f32 palette[6][3] = {{0.85f, 0.3f, 0.3f}, {0.3f, 0.75f, 0.4f}, {0.35f, 0.5f, 0.9f},
                               {0.9f, 0.75f, 0.3f}, {0.7f, 0.4f, 0.85f}, {0.3f, 0.8f, 0.8f}};
    const core::Span<const sim::Aabb> boxes = sim::visible_boxes();
    for (u64 c = 0; c < boxes.size(); ++c) {
        const sim::Aabb& b = boxes[c];
        const f32* col = palette[c % 6];
        add_level_box(verts, indices, b.min_x, b.min_y, b.min_z, b.max_x, b.max_y, b.max_z, col[0],
                      col[1], col[2]);
    }
}

RenderModel make_viewmodel(gpu::Renderer& gpu, u32 fallback_texture) {
    // Barrel and grip in gunmetal, then the sight blade and crosshair ticks in
    // the bright accent colour.
    static asset::MeshVertex verts[7 * 24];
    static u32 indices[7 * 36];
    u32 vc = 0;
    u32 ic = 0;
    add_mesh_box(verts, indices, &vc, &ic, core::Vec3{0.0f, 0.0f, -0.10f},
                 core::Vec3{0.022f, 0.022f, 0.14f});
    add_mesh_box(verts, indices, &vc, &ic, core::Vec3{0.0f, -0.055f, 0.02f},
                 core::Vec3{0.030f, 0.050f, 0.050f});
    add_mesh_box(verts, indices, &vc, &ic, core::Vec3{0.0f, 0.034f, -0.02f},
                 core::Vec3{0.008f, 0.012f, 0.030f});

    static asset::Submesh subs[2];
    subs[0].index_offset = 0;
    subs[0].index_count = 72;
    subs[0].texture = asset::NO_TEXTURE;
    subs[0].color[0] = 0.16f;
    subs[0].color[1] = 0.16f;
    subs[0].color[2] = 0.18f;
    subs[1].index_offset = 72;
    subs[1].index_count = 36;
    subs[1].texture = asset::NO_TEXTURE;
    subs[1].color[0] = 0.90f;
    subs[1].color[1] = 0.45f;
    subs[1].color[2] = 0.15f;
    for (u32 i = 0; i < 2; ++i) {
        for (u32 c = 0; c < 3; ++c) {
            subs[i].bounds_min[c] = -0.3f;
            subs[i].bounds_max[c] = 0.3f;
        }
    }

    // The crosshair rides in the viewmodel model: these offsets cancel the
    // camera anchor so the ticks sit dead centre of the view.
    const core::Vec3 recenter{-0.17f, 0.14f, -0.87f};  // anchor is (0.17,-0.14,-0.33)
    const f32 gap = 0.012f;
    const f32 arm = 0.010f;
    const f32 thick = 0.0016f;
    add_mesh_box(verts, indices, &vc, &ic, recenter + core::Vec3{gap + arm * 0.5f, 0.0f, 0.0f},
                 core::Vec3{arm * 0.5f, thick, thick});
    add_mesh_box(verts, indices, &vc, &ic, recenter + core::Vec3{-gap - arm * 0.5f, 0.0f, 0.0f},
                 core::Vec3{arm * 0.5f, thick, thick});
    add_mesh_box(verts, indices, &vc, &ic, recenter + core::Vec3{0.0f, gap + arm * 0.5f, 0.0f},
                 core::Vec3{thick, arm * 0.5f, thick});
    add_mesh_box(verts, indices, &vc, &ic, recenter + core::Vec3{0.0f, -gap - arm * 0.5f, 0.0f},
                 core::Vec3{thick, arm * 0.5f, thick});
    subs[1].index_count = 36 + 4 * 36;  // sight blade plus the crosshair ticks

    asset::MeshData mesh;
    mesh.vertices = core::Span<const asset::MeshVertex>(verts, vc);
    mesh.indices = core::Span<const u32>(indices, ic);
    mesh.submeshes = core::Span<const asset::Submesh>(subs, 2);
    return model_from_data(gpu, mesh, fallback_texture);
}

RenderModel make_tracer(gpu::Renderer& gpu, u32 fallback_texture) {
    static asset::MeshVertex verts[24];
    static u32 indices[36];
    u32 vc = 0;
    u32 ic = 0;
    add_mesh_box(verts, indices, &vc, &ic, core::Vec3{0.0f, 0.0f, -0.5f},
                 core::Vec3{1.0f, 1.0f, 0.5f});
    static asset::Submesh sub;
    sub.index_count = 36;
    sub.texture = asset::NO_TEXTURE;
    sub.color[0] = 3.0f;  // over-bright so lighting cannot dim it much
    sub.color[1] = 2.6f;
    sub.color[2] = 1.6f;
    sub.bounds_min[0] = -1.0f;
    sub.bounds_min[1] = -1.0f;
    sub.bounds_min[2] = -1.0f;
    sub.bounds_max[0] = 1.0f;
    sub.bounds_max[1] = 1.0f;
    sub.bounds_max[2] = 0.0f;
    asset::MeshData mesh;
    mesh.vertices = core::Span<const asset::MeshVertex>(verts, vc);
    mesh.indices = core::Span<const u32>(indices, ic);
    mesh.submeshes = core::Span<const asset::Submesh>(&sub, 1);
    return model_from_data(gpu, mesh, fallback_texture);
}

RenderModel make_hitmarker(gpu::Renderer& gpu, u32 fallback_texture) {
    static asset::MeshVertex verts[4 * 24];
    static u32 indices[4 * 36];
    u32 vc = 0;
    u32 ic = 0;
    // Four ticks just outside the crosshair, each rotated onto its diagonal
    // so together they read as an X.
    const f32 d = 0.026f;
    for (u32 i = 0; i < 4; ++i) {
        const f32 sx = (i & 1) != 0 ? 1.0f : -1.0f;
        const f32 sy = (i & 2) != 0 ? 1.0f : -1.0f;
        const u32 first = vc;
        const core::Vec3 center{sx * d, sy * d, 0.0f};
        add_mesh_box(verts, indices, &vc, &ic, center, core::Vec3{0.008f, 0.0016f, 0.0016f});
        const f32 rc = 0.70710678f;
        const f32 rs = sx * sy > 0.0f ? 0.70710678f : -0.70710678f;
        for (u32 v = first; v < vc; ++v) {
            const core::Vec3 p = verts[v].pos - center;
            verts[v].pos = center + core::Vec3{p.x * rc - p.y * rs, p.x * rs + p.y * rc, p.z};
            const core::Vec3 n = verts[v].normal;
            verts[v].normal = core::Vec3{n.x * rc - n.y * rs, n.x * rs + n.y * rc, n.z};
        }
    }
    static asset::Submesh sub;
    sub.index_count = 4 * 36;
    sub.texture = asset::NO_TEXTURE;
    sub.color[0] = 3.0f;
    sub.color[1] = 0.6f;
    sub.color[2] = 0.5f;
    sub.bounds_min[0] = -0.1f;
    sub.bounds_min[1] = -0.1f;
    sub.bounds_min[2] = -0.1f;
    sub.bounds_max[0] = 0.1f;
    sub.bounds_max[1] = 0.1f;
    sub.bounds_max[2] = 0.1f;
    asset::MeshData mesh;
    mesh.vertices = core::Span<const asset::MeshVertex>(verts, vc);
    mesh.indices = core::Span<const u32>(indices, ic);
    mesh.submeshes = core::Span<const asset::Submesh>(&sub, 1);
    return model_from_data(gpu, mesh, fallback_texture);
}

RenderModel make_overlay_quad(gpu::Renderer& gpu, u32 fallback_texture) {
    static asset::MeshVertex verts[4];
    static u32 indices[6] = {0, 1, 2, 0, 2, 3};
    const f32 xs[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
    const f32 ys[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
    for (u32 i = 0; i < 4; ++i) {
        verts[i].pos = core::Vec3{xs[i], ys[i], 0.0f};
        verts[i].normal = core::Vec3{0.0f, 0.0f, 1.0f};
    }
    static asset::Submesh sub;
    sub.index_count = 6;
    sub.texture = asset::NO_TEXTURE;
    for (u32 c = 0; c < 4; ++c) {
        sub.color[c] = 1.0f;
        sub.bounds_min[c % 3] = -1.0f;
        sub.bounds_max[c % 3] = 1.0f;
    }
    asset::MeshData mesh;
    mesh.vertices = core::Span<const asset::MeshVertex>(verts, 4);
    mesh.indices = core::Span<const u32>(indices, 6);
    mesh.submeshes = core::Span<const asset::Submesh>(&sub, 1);
    return model_from_data(gpu, mesh, fallback_texture);
}

RenderModel make_trooper(gpu::Renderer& gpu, u32 fallback_texture) {
    // Legs, torso, head, and a visor stripe. Near-white so the team tint reads.
    static asset::MeshVertex verts[4 * 24];
    static u32 indices[4 * 36];
    u32 vc = 0;
    u32 ic = 0;
    add_mesh_box(verts, indices, &vc, &ic, core::Vec3{0.0f, 0.45f, 0.0f},
                 core::Vec3{0.16f, 0.45f, 0.12f});
    add_mesh_box(verts, indices, &vc, &ic, core::Vec3{0.0f, 1.15f, 0.0f},
                 core::Vec3{0.24f, 0.28f, 0.15f});
    add_mesh_box(verts, indices, &vc, &ic, core::Vec3{0.0f, 1.58f, 0.0f},
                 core::Vec3{0.11f, 0.13f, 0.11f});
    add_mesh_box(verts, indices, &vc, &ic, core::Vec3{0.0f, 1.60f, -0.09f},
                 core::Vec3{0.09f, 0.03f, 0.03f});

    static asset::Submesh subs[2];
    subs[0].index_offset = 0;
    subs[0].index_count = 3 * 36;
    subs[0].texture = asset::NO_TEXTURE;
    subs[0].color[0] = 0.85f;
    subs[0].color[1] = 0.85f;
    subs[0].color[2] = 0.85f;
    subs[1].index_offset = 3 * 36;
    subs[1].index_count = 36;
    subs[1].texture = asset::NO_TEXTURE;
    subs[1].color[0] = 0.1f;
    subs[1].color[1] = 0.1f;
    subs[1].color[2] = 0.1f;
    for (u32 i = 0; i < 2; ++i) {
        subs[i].bounds_min[0] = -0.3f;
        subs[i].bounds_min[1] = 0.0f;
        subs[i].bounds_min[2] = -0.3f;
        subs[i].bounds_max[0] = 0.3f;
        subs[i].bounds_max[1] = 1.8f;
        subs[i].bounds_max[2] = 0.3f;
    }

    asset::MeshData mesh;
    mesh.vertices = core::Span<const asset::MeshVertex>(verts, vc);
    mesh.indices = core::Span<const u32>(indices, ic);
    mesh.submeshes = core::Span<const asset::Submesh>(subs, 2);
    return model_from_data(gpu, mesh, fallback_texture);
}

}  // namespace render
