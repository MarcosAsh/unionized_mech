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
    constexpr i32 N = 100;
    constexpr f32 CELL = 2.0f;
    const f32 half = static_cast<f32>(N) * CELL * 0.5f;
    for (i32 i = 0; i < N; ++i) {
        for (i32 j = 0; j < N; ++j) {
            const f32 x = -half + static_cast<f32>(i) * CELL;
            const f32 z = -half + static_cast<f32>(j) * CELL;
            const bool light = ((i + j) & 1) != 0;
            f32 r = light ? 0.38f : 0.24f;
            f32 g = r;
            f32 b = r + 0.04f;
            // A warm walkway from the plaza south to the Sponza atrium.
            if ((i == 49 || i == 50) && z < -16.0f) {
                r = light ? 0.52f : 0.44f;
                g = light ? 0.42f : 0.34f;
                b = 0.26f;
            }
            add_quad(verts, indices, x, z, CELL, r, g, b);
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
    // Three boxes: barrel and grip in gunmetal, a sight blade in accent orange.
    static asset::MeshVertex verts[3 * 24];
    static u32 indices[3 * 36];
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

    asset::MeshData mesh;
    mesh.vertices = core::Span<const asset::MeshVertex>(verts, vc);
    mesh.indices = core::Span<const u32>(indices, ic);
    mesh.submeshes = core::Span<const asset::Submesh>(subs, 2);
    return model_from_data(gpu, mesh, fallback_texture);
}

void fox_companion_update(FoxCompanion& fox, f32 player_x, f32 player_z, f32 dt) {
    const f32 dx = player_x - fox.x;
    const f32 dz = player_z - fox.z;
    const f32 dist = std::sqrt(dx * dx + dz * dz);

    // Approach until comfortably near, sprint when far behind.
    f32 desired = (dist - 2.2f) * 1.4f;
    if (desired < 0.0f) {
        desired = 0.0f;
    }
    if (desired > 5.5f) {
        desired = 5.5f;
    }
    fox.speed += (desired - fox.speed) * (1.0f - std::exp(-6.0f * dt));

    if (dist > 0.05f && fox.speed > 0.05f) {
        const f32 target_yaw = std::atan2(dx / dist, dz / dist);
        fox.yaw += core::angle_lerp(fox.yaw, target_yaw, 1.0f - std::exp(-8.0f * dt)) - fox.yaw;
        fox.x += std::sin(fox.yaw) * fox.speed * dt;
        fox.z += std::cos(fox.yaw) * fox.speed * dt;
    }
}

void fox_gait(f32 speed, u32* clip_a, u32* clip_b, f32* blend) {
    // Fox clip order: 0 Survey (idle), 1 Walk, 2 Run.
    if (speed < 0.25f) {
        *clip_a = 0;
        *clip_b = 0;
        *blend = 0.0f;
    } else if (speed < 1.6f) {
        *clip_a = 0;
        *clip_b = 1;
        *blend = (speed - 0.25f) / 1.35f;
    } else {
        *clip_a = 1;
        *clip_b = 2;
        f32 t = (speed - 1.6f) / 2.9f;
        *blend = t > 1.0f ? 1.0f : t;
    }
}

}  // namespace render
