#pragma once

// Internal to the render module. A GPU-resident imported model: one vertex and
// index pool, per-material submeshes, and the textures they reference.

#include "asset/asset.h"
#include "core/arena.h"
#include "core/mat.h"
#include "core/quat.h"
#include "core/types.h"
#include "core/vec.h"
#include "gpu/gpu.h"

namespace render {

constexpr u32 MAX_MODEL_SUBMESHES = 64;
constexpr u32 MAX_MODEL_TEXTURES = 64;

/// Push constants of the mesh pass, matching shaders/mesh.vert and .frag.
struct MeshPush {
    f32 mvp[16];
    f32 rot[4];    ///< Model rotation quaternion for world-space normals.
    f32 color[4];  ///< Base color factor.
    u32 vbuf;
    u32 tex;
};

/// An imported model uploaded to the GPU, drawn submesh by submesh through the
/// indirect buffer.
struct RenderModel {
    gpu::Buffer vertices;
    gpu::Buffer indices;
    gpu::Buffer indirect;  ///< One VkDrawIndexedIndirectCommand per submesh.
    asset::Submesh submeshes[MAX_MODEL_SUBMESHES];
    u32 texture_slots[MAX_MODEL_TEXTURES];  ///< Bindless indices per model texture.
    u32 submesh_count = 0;
    u32 texture_count = 0;
    bool loaded = false;
};

/// Load `<base>.umesh` and its `<base>.<i>.utex` textures onto the GPU.
/// `fallback_texture` is the bindless slot used by untextured submeshes.
/// Returns an empty model (loaded false) when files are missing, so callers
/// can degrade gracefully. `scratch` is used only during the upload.
[[nodiscard]] RenderModel model_load(gpu::Renderer& gpu, core::Arena& scratch, const char* base,
                                     u32 fallback_texture);

/// Record every submesh of `model` at the given placement.
void model_draw(const RenderModel& model, VkCommandBuffer cmd, VkPipelineLayout layout,
                const core::Mat4& view_proj, core::Vec3 pos, core::Quat rot, f32 scale);

}  // namespace render
