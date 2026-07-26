#pragma once

// Internal to the render module. The sun shadow pass: a depth-only render of
// the level and every caster from the sun's orthographic view.

#include "core/mat.h"
#include "core/types.h"
#include "render_model.h"

namespace render {

/// Everything the shadow pass records from. Borrowed handles, valid for the
/// duration of the call.
struct SunShadowInputs {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkPipeline level_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout level_layout = VK_NULL_HANDLE;
    VkPipeline mesh_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout mesh_layout = VK_NULL_HANDLE;
    VkDescriptorSet bindless = VK_NULL_HANDLE;
    VkBuffer level_indices = VK_NULL_HANDLE;
    VkBuffer level_indirect = VK_NULL_HANDLE;
    u32 level_vbuf = 0;
    const RenderModel* casters[4] = {};
    u32 caster_count = 0;
    core::Mat4 sun_view_proj;
    u32 slot = 0;
};

/// Record the whole pass: transition in, render depth, transition to sampled.
void record_sun_shadow(const SunShadowInputs& in);

/// An instance entry for the ray traced path.
[[nodiscard]] VkAccelerationStructureInstanceKHR make_rt_instance(const core::Mat4& world,
                                                                  u64 blas_address);

/// Ray traced sun prep: rebuild the TLAS from `instances` and make it visible
/// to fragment shaders. Records onto the frame command buffer after the
/// compute phase.
void record_rt_sun(gpu::Renderer& gpu, VkCommandBuffer cmd,
                   core::Span<const VkAccelerationStructureInstanceKHR> instances, u32 slot);

}  // namespace render
