#pragma once

// Internal to the render module. A GPU-resident imported model drawn through
// the GPU-driven path: the CPU queues per-draw records, a compute pass frustum
// culls them into an indirect command buffer, and one counted indirect call
// draws the survivors.

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
constexpr u32 MAX_MODEL_DRAWS = 256;  // records per model per frame

/// Per-draw data read by the cull and vertex shaders. Layout matches the
/// DrawRecord struct in shaders/cull.comp and shaders/mesh.vert (std430).
struct DrawRecord {
    f32 model[16];
    f32 rot[4];
    f32 color[4];
    f32 bounds_min[4];
    f32 bounds_max[4];
    u32 vbuf;
    u32 tex;
    u32 index_count;
    u32 first_index;
    u32 vertex_base;  ///< vertexOffset, selecting a per-frame skinned slice.
    u32 pad[3];
};

static_assert(sizeof(DrawRecord) == 160, "DrawRecord must match the shader layout");

/// Push constants of the skinning pass, matching shaders/skin.comp.
struct SkinPush {
    u32 vertex_count;
    u32 in_vbuf;
    u32 skin_buf;
    u32 matrices_buf;
    u32 matrices_base;
    u32 out_vbuf;
    u32 out_base;
};

/// Push constants of the cull pass, matching shaders/cull.comp.
struct CullPush {
    f32 planes[6][4];
    u32 draw_count;
    u32 records_buf;
    u32 records_base;
    u32 commands_buf;
    u32 commands_base;
    u32 count_buf;
    u32 count_slot;
};

/// Push constants of the mesh pass, matching shaders/mesh.vert.
struct MeshPush {
    f32 view_proj[16];
    u32 records_buf;
    u32 records_base;
};

/// An imported model with its geometry, textures, and GPU-driven draw buffers.
struct RenderModel {
    gpu::Buffer vertices;
    gpu::Buffer indices;
    gpu::Buffer records;   ///< Mapped: frames_in_flight slices of MAX_MODEL_DRAWS.
    gpu::Buffer commands;  ///< GPU-written indirect commands, same slicing.
    gpu::Buffer counts;    ///< One u32 survivor count per frame slot.
    DrawRecord* records_mapped = nullptr;
    asset::Submesh submeshes[MAX_MODEL_SUBMESHES];
    u32 texture_slots[MAX_MODEL_TEXTURES];  ///< Bindless indices per model texture.
    u32 submesh_count = 0;
    u32 texture_count = 0;
    u32 queued = 0;  ///< Records queued this frame.
    bool loaded = false;
};

/// A rigged model: the base model plus skin weights, skeleton, clips, and the
/// per-frame skinning plumbing. Records point at the skinned output vertices.
struct SkinnedModel {
    RenderModel base;
    gpu::Buffer skin_verts;     ///< Static per-vertex joints and weights.
    gpu::Buffer matrices;       ///< Mapped: frames x MAX_JOINTS skinning matrices.
    gpu::Buffer skinned_verts;  ///< GPU-written skinned vertices, frames slices.
    core::Mat4* matrices_mapped = nullptr;
    u32 vertex_count = 0;
    anim::Skeleton skeleton;
    anim::Clip clips[8];
    u32 clip_count = 0;
};

/// Load `<base>.umesh` and its `<base>.<i>.utex` textures onto the GPU.
/// `fallback_texture` is the bindless slot used by untextured submeshes.
/// Returns an empty model (loaded false) when files are missing, so callers
/// can degrade gracefully. `scratch` is used only during the upload.
[[nodiscard]] RenderModel model_load(gpu::Renderer& gpu, core::Arena& scratch, const char* base,
                                     u32 fallback_texture);

/// Load a rigged model: model_load plus `<base>.uskel`, `<base>.uskin`, and
/// `<base>.<i>.uclip`, with the skinning buffers created. Clip storage comes
/// from `permanent`, which must outlive the model.
[[nodiscard]] SkinnedModel skinned_model_load(gpu::Renderer& gpu, core::Arena& permanent,
                                              core::Arena& scratch, const char* base,
                                              u32 fallback_texture);

/// Sample `clip` at `time`, write this frame's skinning matrices, and record
/// the skinning dispatch. Call in the compute phase, before rendering begins.
void skinned_model_update(SkinnedModel& model, VkCommandBuffer cmd, VkPipeline pipeline,
                          VkPipelineLayout layout, VkDescriptorSet bindless, u32 clip, f32 time,
                          u32 slot);

/// Queue one instance of the skinned model. Draws this frame's skinned slice.
void skinned_model_queue(SkinnedModel& model, u32 slot, core::Vec3 pos, core::Quat rot, f32 scale);

/// Start a frame: clear the model's queue for frame `slot`.
void model_begin(RenderModel& model);

/// Queue one instance of the model at a placement, writing one record per
/// submesh into this frame's slice.
void model_queue(RenderModel& model, u32 slot, core::Vec3 pos, core::Quat rot, f32 scale);

/// Record the cull dispatch for this frame's queued records. The caller wraps
/// the fill and dispatch in the appropriate barriers.
void model_cull(const RenderModel& model, VkCommandBuffer cmd, VkPipeline pipeline,
                VkPipelineLayout layout, VkDescriptorSet bindless, const f32 planes[6][4],
                u32 slot);

/// Record the counted indirect draw of the culled survivors.
void model_draw_culled(const RenderModel& model, VkCommandBuffer cmd, VkPipelineLayout layout,
                       const core::Mat4& view_proj, u32 slot);

}  // namespace render
