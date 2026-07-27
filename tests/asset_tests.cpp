// Round-trip tests for the native asset formats, and glTF import. Checks are
// plain ASSERTs.

#include "anim/anim.h"
#include "asset/asset.h"
#include "core/arena.h"
#include "core/assert.h"
#include "core/file.h"
#include "core/log.h"
#include "core/types.h"

using namespace asset;

static bool near(f32 a, f32 b) {
    const f32 d = a - b;
    return (d < 0.0f ? -d : d) <= 1e-4f;
}

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

    Submesh submeshes[2] = {};
    submeshes[0].index_offset = 0;
    submeshes[0].index_count = 3;
    submeshes[0].texture = 0;
    submeshes[0].color[0] = 0.5f;
    submeshes[0].bounds_min[1] = -2.0f;
    submeshes[0].bounds_max[0] = 7.0f;
    submeshes[1].index_offset = 3;
    submeshes[1].index_count = 0;
    submeshes[1].texture = NO_TEXTURE;

    MeshData mesh;
    mesh.vertices = core::Span<const MeshVertex>(verts, 3);
    mesh.indices = core::Span<const u32>(indices, 3);
    mesh.submeshes = core::Span<const Submesh>(submeshes, 2);

    const char* path = "asset_tests_mesh.tmp";
    ASSERT(mesh_save(path, mesh).is_ok());
    core::Result<MeshData, const char*> loaded = mesh_load(arena, path);
    ASSERT(loaded.is_ok());
    const MeshData& back = loaded.value();

    ASSERT(back.vertices.size() == 3);
    ASSERT(back.indices.size() == 3);
    ASSERT(back.submeshes.size() == 2);
    for (u32 i = 0; i < 3; ++i) {
        ASSERT(back.vertices[i].pos == verts[i].pos);
        ASSERT(back.vertices[i].normal == verts[i].normal);
        ASSERT(back.vertices[i].u == verts[i].u);
        ASSERT(back.vertices[i].v == verts[i].v);
        ASSERT(back.indices[i] == indices[i]);
    }
    ASSERT(back.submeshes[0].index_count == 3);
    ASSERT(back.submeshes[0].texture == 0);
    ASSERT(back.submeshes[0].color[0] == 0.5f);
    ASSERT(back.submeshes[0].bounds_min[1] == -2.0f);
    ASSERT(back.submeshes[0].bounds_max[0] == 7.0f);
    ASSERT(back.submeshes[1].texture == NO_TEXTURE);
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

// A rig shaped like the ones exporters actually emit: the mesh and the joints
// are authored in centimetres under an armature node scaled by 100, and that
// node is not itself a joint. Two joints, a triangle bound one vertex to the
// root and two to the tip, no animation. Posed, it must measure a metre.
static const char* const RIG_GLTF = R"gltf({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0, 3]}],
  "nodes": [
    {"name": "Armature", "scale": [100, 100, 100], "children": [1]},
    {"name": "Root", "children": [2]},
    {"name": "Tip", "translation": [0, 0.01, 0]},
    {"name": "Mesh", "mesh": 0, "skin": 0, "scale": [100, 100, 100]}
  ],
  "skins": [{"joints": [1, 2], "skeleton": 1, "inverseBindMatrices": 4}],
  "meshes": [{"primitives": [{
    "attributes": {"POSITION": 0, "JOINTS_0": 1, "WEIGHTS_0": 2}, "indices": 3}]}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
     "min": [0, 0, 0], "max": [0.005, 0.01, 0]},
    {"bufferView": 1, "componentType": 5123, "count": 3, "type": "VEC4"},
    {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC4"},
    {"bufferView": 3, "componentType": 5123, "count": 3, "type": "SCALAR"},
    {"bufferView": 4, "componentType": 5126, "count": 2, "type": "MAT4"}
  ],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 36},
    {"buffer": 0, "byteOffset": 36, "byteLength": 24},
    {"buffer": 0, "byteOffset": 60, "byteLength": 48},
    {"buffer": 0, "byteOffset": 108, "byteLength": 6},
    {"buffer": 0, "byteOffset": 116, "byteLength": 128}
  ],
  "buffers": [{"byteLength": 244, "uri": "data:application/octet-stream;base64,)gltf"
                                   "AAAAAAAAAAAAAAAAAAAAAArXIzwAAAAACtejOwrXIzwAAAAAAAAAAAAAAAABAAAA"
                                   "AAAAAAEAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/"
                                   "AAAAAAAAAAAAAAAAAAABAAIAAAAAAIA/AAAAAAAAAAAAAAAAAAAAAAAAgD8AAAAA"
                                   "AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAAAAAAIA/AACAPwAAAAAAAAAA"
                                   "AAAAAAAAAAAAAIA/AAAAAAAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAArXI7wAAAAA"
                                   "AACAPw==" R"gltf("}]
})gltf";

static void test_gltf_rig_import() {
    const char* path = "asset_tests_rig.tmp";
    ASSERT(core::write_entire_file(path,
                                   core::Span<const u8>(
                                       reinterpret_cast<const u8*>(RIG_GLTF),
                                       __builtin_strlen(RIG_GLTF)))
               .is_ok());

    core::Arena arena = core::Arena::with_capacity(1u << 20);
    core::Result<Model, const char*> imported = import_gltf(arena, path);
    ASSERT(imported.is_ok());
    const Model& model = imported.value();

    ASSERT(model.skeleton.joint_count == 2);
    ASSERT(model.skeleton.parents[0] == -1);
    ASSERT(model.skeleton.parents[1] == 0);
    // Engine joint order is topological, not the file's, so an attachment that
    // remembered an index would drift. Names are what survive.
    ASSERT(joint_index(model.skeleton, "Root") == 0);
    ASSERT(joint_index(model.skeleton, "Tip") == 1);
    ASSERT(model.skin.size() == 3);

    // The armature is not a joint, so only the skeleton's root transform can
    // carry it. Drop it and the model imports a hundredth of its size.
    ASSERT(near(model.skeleton.root_transform.m[0], 100.0f));
    ASSERT(near(model.skeleton.root_transform.m[5], 100.0f));

    // The skinned mesh node's own transform is ignored per the glTF spec, so
    // the vertices stay in bind space: a hundredth of what they draw at.
    ASSERT(near(model.mesh.vertices[1].pos.y, 0.01f));

    // The submesh box is the posed one, which is what culling needs. Bind-pose
    // boxes would make this 0.01 and cull the model from almost everywhere.
    ASSERT(model.mesh.submeshes.size() == 1);
    ASSERT(near(model.mesh.submeshes[0].bounds_min[1], 0.0f));
    ASSERT(near(model.mesh.submeshes[0].bounds_max[1], 1.0f));
    ASSERT(near(model.mesh.submeshes[0].bounds_max[0], 0.5f));
}

int main() {
    test_mesh_round_trip();
    test_texture_round_trip();
    test_load_rejects_garbage();
    test_gltf_rig_import();
    core::log_info("asset_tests: all passed");
    return 0;
}
