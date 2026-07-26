#pragma once

#include "core/arena.h"
#include "core/result.h"
#include "core/span.h"
#include "core/types.h"
#include "core/vec.h"

namespace asset {

/// One mesh vertex: position, normal, and texture coordinates. 32 bytes, laid
/// out to match the shader's storage buffer view of it.
struct MeshVertex {
    core::Vec3 pos;
    core::Vec3 normal;
    f32 u = 0.0f;
    f32 v = 0.0f;
};

static_assert(sizeof(MeshVertex) == 32, "MeshVertex layout must stay file-stable");

/// A contiguous index range drawn with one material. `texture` indexes the
/// model's texture list, or NO_TEXTURE when the material is untextured. The
/// bounds are the submesh's model-space box, used for culling.
struct Submesh {
    u32 index_offset = 0;
    u32 index_count = 0;
    u32 texture = 0xFFFFFFFFu;
    f32 color[4] = {1.0f, 1.0f, 1.0f, 1.0f};  ///< Base color factor.
    f32 bounds_min[3] = {0.0f, 0.0f, 0.0f};
    f32 bounds_max[3] = {0.0f, 0.0f, 0.0f};
};

constexpr u32 NO_TEXTURE = 0xFFFFFFFFu;

static_assert(sizeof(Submesh) == 52, "Submesh layout must stay file-stable");

/// Triangle mesh data in engine layout: one vertex and index pool, drawn as
/// per-material submesh ranges.
struct MeshData {
    core::Span<const MeshVertex> vertices;
    core::Span<const u32> indices;
    core::Span<const Submesh> submeshes;
};

/// An RGBA8 image, tightly packed rows.
struct TextureData {
    u32 width = 0;
    u32 height = 0;
    core::Span<const u8> rgba;
};

/// A model imported from glTF: one merged mesh in per-material submeshes, and
/// the base color textures those submeshes reference.
struct Model {
    MeshData mesh;
    core::Span<const TextureData> textures;
};

/// Import a .gltf or .glb file. All primitives of the default scene are merged
/// into one vertex and index pool with node transforms applied, grouped into
/// one submesh per material. Base color textures load from buffer views or
/// from URIs beside the file. Storage comes from `arena`.
/// # Errors
/// A static message naming the failing stage.
[[nodiscard]] core::Result<Model, const char*> import_gltf(core::Arena& arena, const char* path);

/// Write a mesh in the native format read by mesh_load.
/// # Errors
/// A static message when the file cannot be written.
[[nodiscard]] core::Result<core::Unit, const char*> mesh_save(const char* path,
                                                              const MeshData& mesh);

/// Load a native mesh written by mesh_save, backed by `arena`.
/// # Errors
/// A static message when the file is missing, truncated, or not a mesh.
[[nodiscard]] core::Result<MeshData, const char*> mesh_load(core::Arena& arena, const char* path);

/// Write a texture in the native format read by texture_load.
/// # Errors
/// A static message when the file cannot be written.
[[nodiscard]] core::Result<core::Unit, const char*> texture_save(const char* path,
                                                                 const TextureData& texture);

/// Load a native texture written by texture_save, backed by `arena`.
/// # Errors
/// A static message when the file is missing, truncated, or not a texture.
[[nodiscard]] core::Result<TextureData, const char*> texture_load(core::Arena& arena,
                                                                  const char* path);

}  // namespace asset
