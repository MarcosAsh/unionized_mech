#include "render_props.h"

#include "asset/asset.h"
#include "sim/sim.h"

#include <cmath>

namespace render {

namespace {

// One texture repeat every two metres, the scale the arena is laid out on.
constexpr f32 LEVEL_UV_SCALE = 0.5f;

void add_level_vertex(core::Array<LevelVertex>& verts, core::Vec3 p, const f32 rgb[3],
                      core::Vec3 n, f32 u, f32 v) {
    LevelVertex out;
    out.pos[0] = p.x;
    out.pos[1] = p.y;
    out.pos[2] = p.z;
    out.pos[3] = 1.0f;
    out.color[0] = rgb[0];
    out.color[1] = rgb[1];
    out.color[2] = rgb[2];
    out.color[3] = 1.0f;
    out.normal[0] = n.x;
    out.normal[1] = n.y;
    out.normal[2] = n.z;
    out.normal[3] = 0.0f;
    out.uv[0] = u;
    out.uv[1] = v;
    out.uv[2] = 0.0f;
    out.uv[3] = 0.0f;
    verts.push(out);
}

void push_quad(core::Array<u32>& indices, u32 base) {
    const u32 quad[6] = {base, base + 1, base + 2, base, base + 2, base + 3};
    for (u32 i = 0; i < 6; ++i) {
        indices.push(quad[i]);
    }
}

// A floor cell. Its texture coordinates come from world position, so adjacent
// cells line up into one continuous surface instead of each restarting at zero.
void add_quad(core::Array<LevelVertex>& verts, core::Array<u32>& indices, f32 x, f32 z, f32 cell,
              f32 r, f32 g, f32 b) {
    const u32 base = static_cast<u32>(verts.size());
    const f32 rgb[3] = {r, g, b};
    const core::Vec3 up{0.0f, 1.0f, 0.0f};
    const f32 x1 = x + cell;
    const f32 z1 = z + cell;
    // v runs along -Z, which is cross(normal, u) for an upward face. The boxes
    // build their faces from that same rule and the shader rebuilds the tangent
    // frame from it, so the floor cannot be the one surface that disagrees.
    add_level_vertex(verts, {x, 0.0f, z}, rgb, up, x * LEVEL_UV_SCALE, -z * LEVEL_UV_SCALE);
    add_level_vertex(verts, {x1, 0.0f, z}, rgb, up, x1 * LEVEL_UV_SCALE, -z * LEVEL_UV_SCALE);
    add_level_vertex(verts, {x1, 0.0f, z1}, rgb, up, x1 * LEVEL_UV_SCALE, -z1 * LEVEL_UV_SCALE);
    add_level_vertex(verts, {x, 0.0f, z1}, rgb, up, x * LEVEL_UV_SCALE, -z1 * LEVEL_UV_SCALE);
    push_quad(indices, base);
}

// A box as six separate faces. Sharing eight corners between them was cheaper
// but left nowhere to put a per-face normal or a sane texture coordinate, which
// is why the level used to shade flat.
void add_level_box(core::Array<LevelVertex>& verts, core::Array<u32>& indices, f32 x0, f32 y0,
                   f32 z0, f32 x1, f32 y1, f32 z1, f32 r, f32 g, f32 b) {
    const f32 rgb[3] = {r, g, b};
    const core::Vec3 center{(x0 + x1) * 0.5f, (y0 + y1) * 0.5f, (z0 + z1) * 0.5f};
    const core::Vec3 half{(x1 - x0) * 0.5f, (y1 - y0) * 0.5f, (z1 - z0) * 0.5f};
    const core::Vec3 n[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    const core::Vec3 uax[6] = {{0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {-1, 0, 0}};
    const f32 su[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
    const f32 sv[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
    for (u32 f = 0; f < 6; ++f) {
        const core::Vec3 vax = cross(n[f], uax[f]);
        const u32 base = static_cast<u32>(verts.size());
        // Half extent along each of the face's own axes, so the texture keeps a
        // constant world density however large the box is.
        const f32 hu = std::fabs(uax[f].x) * half.x + std::fabs(uax[f].y) * half.y +
                       std::fabs(uax[f].z) * half.z;
        const f32 hv = std::fabs(vax.x) * half.x + std::fabs(vax.y) * half.y +
                       std::fabs(vax.z) * half.z;
        for (u32 k = 0; k < 4; ++k) {
            core::Vec3 p = center;
            p += core::Vec3{n[f].x * half.x, n[f].y * half.y, n[f].z * half.z};
            p += core::Vec3{uax[f].x * half.x, uax[f].y * half.y, uax[f].z * half.z} * su[k];
            p += core::Vec3{vax.x * half.x, vax.y * half.y, vax.z * half.z} * sv[k];
            add_level_vertex(verts, p, rgb, n[f], su[k] * hu * LEVEL_UV_SCALE,
                             sv[k] * hv * LEVEL_UV_SCALE);
        }
        push_quad(indices, base);
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

u32 make_level_data_texture(gpu::Renderer& gpu, core::Arena& scratch, const char* path,
                            const u8 fallback[4]) {
    const u64 marker = scratch.marker();
    core::Result<asset::TextureData, const char*> loaded = asset::texture_load(scratch, path);
    u32 slot = 0;
    if (loaded.is_ok()) {
        const asset::TextureData& tex = loaded.value();
        // Linear, not sRGB: these are numbers, not colour.
        slot = gpu.create_texture(tex.rgba.data(), tex.width, tex.height, false).bindless_index;
    } else {
        // One flat texel standing for "no detail here", so a missing map costs
        // the surface its relief and nothing else.
        slot = gpu.create_texture(fallback, 1, 1, false).bindless_index;
    }
    scratch.rewind(marker);
    return slot;
}

u32 make_level_texture(gpu::Renderer& gpu, core::Arena& scratch) {
    const u64 marker = scratch.marker();
    core::Result<asset::TextureData, const char*> loaded =
        asset::texture_load(scratch, ASSET_DIR "/ground.utex");
    if (loaded.is_ok()) {
        const asset::TextureData& tex = loaded.value();
        const u32 slot = gpu.create_texture(tex.rgba.data(), tex.width, tex.height).bindless_index;
        scratch.rewind(marker);
        return slot;
    }
    scratch.rewind(marker);

    // A tiling surface: a recessed seam along two edges so tiling reads as
    // panelling, plus a fine deterministic grain so a large flat face does not
    // look like poured plastic. Generated, so nothing binary enters the build.
    constexpr u32 N = 64;
    static u8 px[N * N * 4];
    for (u32 y = 0; y < N; ++y) {
        for (u32 x = 0; x < N; ++x) {
            const bool seam = x < 2 || y < 2;
            u32 h = (x * 73856093u) ^ (y * 19349663u);
            h ^= h >> 13;
            h *= 0x5bd1e995u;
            h ^= h >> 15;
            i32 v = (seam ? 150 : 226) + static_cast<i32>(h & 15u) - 8;
            v = v < 0 ? 0 : (v > 255 ? 255 : v);
            u8* p = &px[(y * N + x) * 4];
            p[0] = static_cast<u8>(v);
            p[1] = static_cast<u8>(v);
            p[2] = static_cast<u8>(v);
            p[3] = 255;
        }
    }
    return gpu.create_texture(px, N, N).bindless_index;
}

RenderModel make_viewmodel(gpu::Renderer& gpu, u32 fallback_texture) {
    // Just the crosshair now: four ticks in camera space, dead centre. The
    // weapon itself is the imported blaster model.
    static asset::MeshVertex verts[4 * 24];
    static u32 indices[4 * 36];
    u32 vc = 0;
    u32 ic = 0;
    const core::Vec3 center{0.0f, 0.0f, -1.2f};
    const f32 gap = 0.012f;
    const f32 arm = 0.010f;
    const f32 thick = 0.0016f;
    add_mesh_box(verts, indices, &vc, &ic, center + core::Vec3{gap + arm * 0.5f, 0.0f, 0.0f},
                 core::Vec3{arm * 0.5f, thick, thick});
    add_mesh_box(verts, indices, &vc, &ic, center + core::Vec3{-gap - arm * 0.5f, 0.0f, 0.0f},
                 core::Vec3{arm * 0.5f, thick, thick});
    add_mesh_box(verts, indices, &vc, &ic, center + core::Vec3{0.0f, gap + arm * 0.5f, 0.0f},
                 core::Vec3{thick, arm * 0.5f, thick});
    add_mesh_box(verts, indices, &vc, &ic, center + core::Vec3{0.0f, -gap - arm * 0.5f, 0.0f},
                 core::Vec3{thick, arm * 0.5f, thick});
    static asset::Submesh sub;
    sub.index_count = 4 * 36;
    sub.texture = asset::NO_TEXTURE;
    sub.color[0] = 0.90f;
    sub.color[1] = 0.45f;
    sub.color[2] = 0.15f;
    sub.bounds_min[0] = -0.3f;
    sub.bounds_min[1] = -0.3f;
    sub.bounds_min[2] = -1.5f;
    sub.bounds_max[0] = 0.3f;
    sub.bounds_max[1] = 0.3f;
    sub.bounds_max[2] = 0.3f;
    asset::MeshData mesh;
    mesh.vertices = core::Span<const asset::MeshVertex>(verts, vc);
    mesh.indices = core::Span<const u32>(indices, ic);
    mesh.submeshes = core::Span<const asset::Submesh>(&sub, 1);
    return model_from_data(gpu, mesh, fallback_texture);
}

RenderModel make_mech(gpu::Renderer& gpu, u32 fallback_texture) {
    // Legs, torso, head, cannon. The silhouette fills the sim's 3.2 x 4.4m
    // mech hull so what you see is what gets hit.
    static asset::MeshVertex verts[5 * 24];
    static u32 indices[5 * 36];
    u32 vc = 0;
    u32 ic = 0;
    add_mesh_box(verts, indices, &vc, &ic, core::Vec3{-0.62f, 1.0f, 0.0f},
                 core::Vec3{0.38f, 1.0f, 0.5f});
    add_mesh_box(verts, indices, &vc, &ic, core::Vec3{0.62f, 1.0f, 0.0f},
                 core::Vec3{0.38f, 1.0f, 0.5f});
    add_mesh_box(verts, indices, &vc, &ic, core::Vec3{0.0f, 2.8f, 0.0f},
                 core::Vec3{1.25f, 0.85f, 0.8f});
    add_mesh_box(verts, indices, &vc, &ic, core::Vec3{0.0f, 4.05f, 0.0f},
                 core::Vec3{0.4f, 0.32f, 0.4f});
    add_mesh_box(verts, indices, &vc, &ic, core::Vec3{0.85f, 3.0f, -1.0f},
                 core::Vec3{0.2f, 0.2f, 1.0f});
    static asset::Submesh sub;
    sub.index_count = 5 * 36;
    sub.texture = asset::NO_TEXTURE;
    sub.color[0] = 0.62f;
    sub.color[1] = 0.64f;
    sub.color[2] = 0.68f;
    sub.bounds_min[0] = -2.2f;
    sub.bounds_min[1] = 0.0f;
    sub.bounds_min[2] = -2.2f;
    sub.bounds_max[0] = 2.2f;
    sub.bounds_max[1] = 4.6f;
    sub.bounds_max[2] = 2.2f;
    asset::MeshData mesh;
    mesh.vertices = core::Span<const asset::MeshVertex>(verts, vc);
    mesh.indices = core::Span<const u32>(indices, ic);
    mesh.submeshes = core::Span<const asset::Submesh>(&sub, 1);
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
        // NDC y grows downward, so the top of the screen samples v = 0.
        verts[i].u = (xs[i] + 1.0f) * 0.5f;
        verts[i].v = (ys[i] + 1.0f) * 0.5f;
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
