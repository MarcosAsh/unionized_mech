#pragma once

#include "anim/anim.h"
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
    /// Metallic and roughness, as the shading model reads them. The defaults
    /// are a matte dielectric rather than glTF's own metallic 1: a material
    /// that never said is far likelier to be painted than to be bare metal, and
    /// the procedural props set no material at all.
    f32 metallic = 0.0f;
    f32 roughness = 0.6f;
};

constexpr u32 NO_TEXTURE = 0xFFFFFFFFu;

static_assert(sizeof(Submesh) == 60, "Submesh layout must stay file-stable");

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

/// Per-vertex skinning data: four joint indices and weights, parallel to the
/// mesh's vertex array.
struct SkinVertex {
    u16 joints[4] = {0, 0, 0, 0};
    f32 weights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

static_assert(sizeof(SkinVertex) == 24, "SkinVertex layout must stay file-stable");

/// A model imported from glTF: one merged mesh in per-material submeshes, the
/// base color textures those submeshes reference, and, when the source is
/// rigged, its skin weights, skeleton, and animation clips.
struct Model {
    MeshData mesh;
    core::Span<const TextureData> textures;
    core::Span<const SkinVertex> skin;  ///< Empty when the model is unskinned.
    core::Span<const anim::Clip> clips;
    anim::Skeleton skeleton;  ///< joint_count 0 when the model is unskinned.
};

/// Write skin vertices in the native format read by skin_load.
/// # Errors
/// A static message when the file cannot be written.
[[nodiscard]] core::Result<core::Unit, const char*> skin_save(const char* path,
                                                              core::Span<const SkinVertex> skin);

/// Load native skin vertices written by skin_save, backed by `arena`.
/// # Errors
/// A static message when the file is missing, truncated, or not a skin.
[[nodiscard]] core::Result<core::Span<const SkinVertex>, const char*> skin_load(core::Arena& arena,
                                                                                const char* path);

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

/// Decode a standalone PNG or JPEG file into RGBA8, backed by `arena`. The
/// glTF path already decoded embedded images; this exposes the same thing for
/// loose texture files so they can be converted offline like everything else.
/// # Errors
/// A static message when the file is missing or is not a decodable image.
[[nodiscard]] core::Result<TextureData, const char*> image_import(core::Arena& arena,
                                                                  const char* path);

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
