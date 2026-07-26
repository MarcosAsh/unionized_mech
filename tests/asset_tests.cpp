// Round-trip tests for the native asset formats. Checks are plain ASSERTs.

#include "asset/asset.h"
#include "core/arena.h"
#include "core/assert.h"
#include "core/log.h"
#include "core/types.h"

using namespace asset;

static void test_mesh_round_trip() {
    core::Arena arena = core::Arena::with_capacity(1u << 20);

    MeshVertex verts[3] = {};
    verts[0].pos = {0.0f, 0.0f, 0.0f};
    verts[1].pos = {1.0f, 0.0f, 0.0f};
    verts[2].pos = {0.0f, 1.0f, 0.0f};
    for (u32 i = 0; i < 3; ++i) {
        verts[i].normal = {0.0f, 0.0f, 1.0f};
        verts[i].u = static_cast<f32>(i) * 0.5f;
        verts[i].v = 1.0f - static_cast<f32>(i) * 0.5f;
    }
    const u32 indices[3] = {0, 1, 2};

    MeshData mesh;
    mesh.vertices = core::Span<const MeshVertex>(verts, 3);
    mesh.indices = core::Span<const u32>(indices, 3);

    const char* path = "asset_tests_mesh.tmp";
    ASSERT(mesh_save(path, mesh).is_ok());
    core::Result<MeshData, const char*> loaded = mesh_load(arena, path);
    ASSERT(loaded.is_ok());
    const MeshData& back = loaded.value();

    ASSERT(back.vertices.size() == 3);
    ASSERT(back.indices.size() == 3);
    for (u32 i = 0; i < 3; ++i) {
        ASSERT(back.vertices[i].pos == verts[i].pos);
        ASSERT(back.vertices[i].normal == verts[i].normal);
        ASSERT(back.vertices[i].u == verts[i].u);
        ASSERT(back.vertices[i].v == verts[i].v);
        ASSERT(back.indices[i] == indices[i]);
    }
}

static void test_texture_round_trip() {
    core::Arena arena = core::Arena::with_capacity(1u << 20);

    u8 pixels[2 * 2 * 4];
    for (u32 i = 0; i < sizeof(pixels); ++i) {
        pixels[i] = static_cast<u8>(i * 7);
    }
    TextureData tex;
    tex.width = 2;
    tex.height = 2;
    tex.rgba = core::Span<const u8>(pixels, sizeof(pixels));

    const char* path = "asset_tests_tex.tmp";
    ASSERT(texture_save(path, tex).is_ok());
    core::Result<TextureData, const char*> loaded = texture_load(arena, path);
    ASSERT(loaded.is_ok());
    const TextureData& back = loaded.value();

    ASSERT(back.width == 2);
    ASSERT(back.height == 2);
    ASSERT(back.rgba.size() == sizeof(pixels));
    for (u32 i = 0; i < sizeof(pixels); ++i) {
        ASSERT(back.rgba[i] == pixels[i]);
    }
}

static void test_load_rejects_garbage() {
    core::Arena arena = core::Arena::with_capacity(1u << 20);
    ASSERT(mesh_load(arena, "does_not_exist.umesh").is_err());
    // A texture file is not a mesh and must be rejected by magic.
    ASSERT(mesh_load(arena, "asset_tests_tex.tmp").is_err());
    ASSERT(texture_load(arena, "asset_tests_mesh.tmp").is_err());
}

int main() {
    test_mesh_round_trip();
    test_texture_round_trip();
    test_load_rejects_garbage();
    core::log_info("asset_tests: all passed");
    return 0;
}
