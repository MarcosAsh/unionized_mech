#include "gpu/gpu.h"

#include "core/assert.h"

#include <volk.h>

#include <cstring>

// Images: sampled textures with their mip chains, the shadow map, and the depth
// buffer. Split from gpu_resources.cpp, which held buffers and images together
// and had grown past the project's file limit.

namespace gpu {

namespace {

// A layout transition for a run of mip levels of a colour image.
VkImageMemoryBarrier2 mip_barrier(VkImage image, u32 base_mip, u32 mip_count,
                                  VkImageLayout old_layout, VkImageLayout new_layout,
                                  VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                                  VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
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
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, base_mip, mip_count, 0, 1};
    return b;
}

void mip_transition(VkCommandBuffer cmd, const VkImageMemoryBarrier2& barrier) {
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

}  // namespace

Texture Renderer::create_texture(const void* rgba, u32 width, u32 height, bool srgb) {
    const VkDevice dev = device_->handle();
    // Colour maps are authored in sRGB and want the hardware decode. Normal and
    // roughness maps are numbers that happen to be stored in an image, and
    // decoding those bends every value on the way in.
    const VkFormat format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    const u64 byte_count = static_cast<u64>(width) * height * 4u;

    u32 mip_count = 1;
    for (u32 size = width > height ? width : height; size > 1; size /= 2) {
        ++mip_count;
    }

    Allocation staging_alloc{};
    VkBuffer staging = create_buffer(dev, allocator_, byte_count, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                     &staging_alloc);
    ASSERT(staging_alloc.mapped != nullptr);
    std::memcpy(staging_alloc.mapped, rgba, byte_count);

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {width, height, 1};
    ici.mipLevels = mip_count;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    ASSERT_MSG(vkCreateImage(dev, &ici, nullptr, &image) == VK_SUCCESS, "vkCreateImage texture");

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, image, &req);
    const Allocation alloc = allocator_.allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    ASSERT_MSG(vkBindImageMemory(dev, image, alloc.memory, alloc.offset) == VK_SUCCESS,
               "vkBindImageMemory texture");

    VkCommandPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_ci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_ci.queueFamilyIndex = device_->graphics_family();
    VkCommandPool upload_pool = VK_NULL_HANDLE;
    ASSERT_MSG(vkCreateCommandPool(dev, &pool_ci, nullptr, &upload_pool) == VK_SUCCESS,
               "upload pool");
    VkCommandBufferAllocateInfo cba{};
    cba.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cba.commandPool = upload_pool;
    cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cba.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    ASSERT_MSG(vkAllocateCommandBuffers(dev, &cba, &cmd) == VK_SUCCESS, "upload cmd");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    // Upload mip 0, then blit each level from the one above it.
    mip_transition(cmd, mip_barrier(image, 0, mip_count, VK_IMAGE_LAYOUT_UNDEFINED,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                                    VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                    VK_ACCESS_2_TRANSFER_WRITE_BIT));

    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    i32 src_w = static_cast<i32>(width);
    i32 src_h = static_cast<i32>(height);
    for (u32 mip = 1; mip < mip_count; ++mip) {
        mip_transition(cmd, mip_barrier(image, mip - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                        VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                        VK_ACCESS_2_TRANSFER_READ_BIT));

        const i32 dst_w = src_w > 1 ? src_w / 2 : 1;
        const i32 dst_h = src_h > 1 ? src_h / 2 : 1;
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 0, 1};
        blit.srcOffsets[1] = {src_w, src_h, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
        blit.dstOffsets[1] = {dst_w, dst_h, 1};
        vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        src_w = dst_w;
        src_h = dst_h;
    }

    // All levels to shader-read: the blitted-from levels are TRANSFER_SRC, the
    // last level is still TRANSFER_DST.
    if (mip_count > 1) {
        mip_transition(cmd, mip_barrier(image, 0, mip_count - 1,
                                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                        VK_ACCESS_2_TRANSFER_READ_BIT,
                                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT));
    }
    mip_transition(cmd, mip_barrier(image, mip_count - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                    VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT));

    vkEndCommandBuffer(cmd);
    VkCommandBufferSubmitInfo cmd_info{};
    cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmd_info.commandBuffer = cmd;
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmd_info;
    vkQueueSubmit2(device_->graphics_queue(), 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(device_->graphics_queue());  // init-time upload, not the frame loop

    vkDestroyCommandPool(dev, upload_pool, nullptr);
    vkDestroyBuffer(dev, staging, nullptr);

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_count, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    ASSERT_MSG(vkCreateImageView(dev, &vci, nullptr, &view) == VK_SUCCESS, "texture view");

    ASSERT_MSG(owned_texture_count_ < MAX_OWNED_TEXTURES, "too many owned textures");
    owned_images_[owned_texture_count_] = image;
    owned_views_[owned_texture_count_] = view;
    ++owned_texture_count_;

    Texture out;
    out.image = image;
    out.view = view;
    out.mip_count = mip_count;
    out.bindless_index = register_sampled_image(view);
    return out;
}

u32 Renderer::register_sampled_image(VkImageView view) {
    const u32 index = next_sampled_index_++;
    VkDescriptorImageInfo info{};
    info.imageView = view;
    info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = bindless_set_;
    write.dstBinding = 1;
    write.dstArrayElement = index;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(device_->handle(), 1, &write, 0, nullptr);
    return index;
}

void Renderer::create_shadow_map() {
    const VkDevice dev = device_->handle();

    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = DEPTH_FORMAT;
    ci.extent = {shadow_size(), shadow_size(), 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ASSERT_MSG(vkCreateImage(dev, &ci, nullptr, &shadow_image_) == VK_SUCCESS, "shadow image");

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, shadow_image_, &req);
    const Allocation alloc = allocator_.allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    ASSERT_MSG(vkBindImageMemory(dev, shadow_image_, alloc.memory, alloc.offset) == VK_SUCCESS,
               "shadow bind");

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = shadow_image_;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = DEPTH_FORMAT;
    vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    ASSERT_MSG(vkCreateImageView(dev, &vci, nullptr, &shadow_view_) == VK_SUCCESS, "shadow view");

    shadow_bindless_ = register_sampled_image(shadow_view_);
}

void Renderer::create_depth() {
    const VkDevice dev = device_->handle();

    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = DEPTH_FORMAT;
    ci.extent = {extent_.width, extent_.height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ASSERT_MSG(vkCreateImage(dev, &ci, nullptr, &depth_image_) == VK_SUCCESS, "vkCreateImage depth");

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, depth_image_, &req);
    const Allocation a = allocator_.allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    ASSERT_MSG(vkBindImageMemory(dev, depth_image_, a.memory, a.offset) == VK_SUCCESS,
               "vkBindImageMemory depth");

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = depth_image_;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = DEPTH_FORMAT;
    vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    ASSERT_MSG(vkCreateImageView(dev, &vci, nullptr, &depth_view_) == VK_SUCCESS,
               "vkCreateImageView depth");
}

}  // namespace gpu
