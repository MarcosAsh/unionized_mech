#pragma once

// Internal to the gpu module. Small helpers shared by the renderer's setup and
// per-frame paths.

#include "core/assert.h"
#include "core/types.h"

#include <volk.h>

namespace gpu {

/// A fresh binary semaphore.
[[nodiscard]] inline VkSemaphore make_binary_semaphore(VkDevice device) {
    VkSemaphoreCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore s = VK_NULL_HANDLE;
    ASSERT_MSG(vkCreateSemaphore(device, &ci, nullptr, &s) == VK_SUCCESS, "vkCreateSemaphore");
    return s;
}

/// An image layout transition for vkCmdPipelineBarrier2.
[[nodiscard]] inline VkImageMemoryBarrier2 image_barrier(
    VkImage image, VkImageAspectFlags aspect, VkImageLayout old_layout, VkImageLayout new_layout,
    VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
    VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = old_layout;
    b.newLayout = new_layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    return b;
}

/// Record a single image barrier.
inline void submit_barrier(VkCommandBuffer cmd, const VkImageMemoryBarrier2& barrier) {
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

}  // namespace gpu
