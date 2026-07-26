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

/// Triangle mesh data in engine layout, indices into the vertex span.
struct MeshData {
    core::Span<const MeshVertex> vertices;
    core::Span<const u32> indices;
};

/// An RGBA8 image, tightly packed rows.
struct TextureData {
    u32 width = 0;
    u32 height = 0;
    core::Span<const u8> rgba;
};

/// A model imported from glTF: one merged mesh and, when the source has one, a
/// base color texture.
struct Model {
    MeshData mesh;
    TextureData base_color;
    bool has_texture = false;
};

/// Import a .gltf or .glb file. All primitives of the default scene are merged
/// into one mesh with node transforms applied. The first base color texture
/// found becomes the model's texture. Storage comes from `arena`.
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
