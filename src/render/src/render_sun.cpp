#include "render_sun.h"

#include <volk.h>

namespace render {

namespace {

struct ShadowLevelPush {
    f32 sun_view_proj[16];
    u32 vbuf;
};

// A depth image layout transition for the shadow pass.
void shadow_barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout,
                    VkImageLayout new_layout, VkPipelineStageFlags2 src_stage,
                    VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                    VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

}  // namespace

void record_sun_shadow(const SunShadowInputs& in) {
    // The sun shadow pass: depth only, from the sun's orthographic view. The
    // previous frame's read finished before this slot was reacquired, so the
    // transition can come from undefined.
    shadow_barrier(in.cmd, in.image, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                   VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                       VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    {
        VkRenderingAttachmentInfo shadow_depth{};
        shadow_depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        shadow_depth.imageView = in.view;
        shadow_depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        shadow_depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        shadow_depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        shadow_depth.clearValue.depthStencil = {1.0f, 0};

        VkRenderingInfo shadow_pass{};
        shadow_pass.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        shadow_pass.renderArea.extent = {gpu::Renderer::shadow_size(),
                                         gpu::Renderer::shadow_size()};
        shadow_pass.layerCount = 1;
        shadow_pass.pDepthAttachment = &shadow_depth;
        vkCmdBeginRendering(in.cmd, &shadow_pass);

        VkViewport sun_viewport{};
        sun_viewport.width = static_cast<f32>(gpu::Renderer::shadow_size());
        sun_viewport.height = static_cast<f32>(gpu::Renderer::shadow_size());
        sun_viewport.maxDepth = 1.0f;
        vkCmdSetViewport(in.cmd, 0, 1, &sun_viewport);
        VkRect2D sun_scissor{};
        sun_scissor.extent = shadow_pass.renderArea.extent;
        vkCmdSetScissor(in.cmd, 0, 1, &sun_scissor);

        vkCmdBindPipeline(in.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, in.level_pipeline);
        vkCmdBindDescriptorSets(in.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, in.level_layout,
                                0, 1, &in.bindless, 0, nullptr);
        vkCmdBindIndexBuffer(in.cmd, in.level_indices, 0, VK_INDEX_TYPE_UINT32);
        ShadowLevelPush sp{};
        for (u32 i = 0; i < 16; ++i) {
            sp.sun_view_proj[i] = in.sun_view_proj.m[i];
        }
        sp.vbuf = in.level_vbuf;
        vkCmdPushConstants(in.cmd, in.level_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(sp), &sp);
        vkCmdDrawIndexedIndirect(in.cmd, in.level_indirect, 0, 1,
                                 sizeof(VkDrawIndexedIndirectCommand));

        vkCmdBindPipeline(in.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, in.mesh_pipeline);
        vkCmdBindDescriptorSets(in.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, in.mesh_layout,
                                0, 1, &in.bindless, 0, nullptr);
        for (u32 i = 0; i < in.caster_count; ++i) {
            model_draw_shadow(*in.casters[i], in.cmd, in.mesh_layout, in.sun_view_proj, in.slot);
        }
        vkCmdEndRendering(in.cmd);
    }
    shadow_barrier(in.cmd, in.image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

}

}  // namespace render
