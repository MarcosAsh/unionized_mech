#include "gpu/gpu.h"

#include "core/assert.h"

#include <volk.h>

#include <cstring>

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

VkDevice Renderer::device_handle() const { return device_->handle(); }

void Renderer::init_bindless() {
    const VkDevice dev = device_->handle();

    // One default sampler, immutable in the set: trilinear, repeat, full mips.
    VkSamplerCreateInfo sampler_ci{};
    sampler_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_ci.magFilter = VK_FILTER_LINEAR;
    sampler_ci.minFilter = VK_FILTER_LINEAR;
    sampler_ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_ci.maxLod = VK_LOD_CLAMP_NONE;
    ASSERT_MSG(vkCreateSampler(dev, &sampler_ci, nullptr, &default_sampler_) == VK_SUCCESS,
               "vkCreateSampler");

    const VkDescriptorPoolSize sizes[4] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, BINDLESS_STORAGE_COUNT},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, BINDLESS_SAMPLED_COUNT},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, BINDLESS_STORAGE_IMAGE_COUNT},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 1},
    };
    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pool_ci.maxSets = 1;
    pool_ci.poolSizeCount = 4;
    pool_ci.pPoolSizes = sizes;
    ASSERT_MSG(vkCreateDescriptorPool(dev, &pool_ci, nullptr, &bindless_pool_) == VK_SUCCESS,
               "vkCreateDescriptorPool");

    const VkDescriptorType types[3] = {
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    };
    const u32 counts[3] = {BINDLESS_STORAGE_COUNT, BINDLESS_SAMPLED_COUNT,
                           BINDLESS_STORAGE_IMAGE_COUNT};

    VkDescriptorSetLayoutBinding bindings[4]{};
    VkDescriptorBindingFlags flags[4];
    for (u32 i = 0; i < 3; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = types[i];
        bindings[i].descriptorCount = counts[i];
        bindings[i].stageFlags = VK_SHADER_STAGE_ALL;
        flags[i] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                   VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    }
    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_ALL;
    bindings[3].pImmutableSamplers = &default_sampler_;
    flags[3] = 0;

    VkDescriptorSetLayoutBindingFlagsCreateInfo flag_ci{};
    flag_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flag_ci.bindingCount = 4;
    flag_ci.pBindingFlags = flags;

    VkDescriptorSetLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.pNext = &flag_ci;
    layout_ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layout_ci.bindingCount = 4;
    layout_ci.pBindings = bindings;
    ASSERT_MSG(vkCreateDescriptorSetLayout(dev, &layout_ci, nullptr, &bindless_layout_) ==
                   VK_SUCCESS,
               "vkCreateDescriptorSetLayout");

    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = bindless_pool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &bindless_layout_;
    ASSERT_MSG(vkAllocateDescriptorSets(dev, &alloc, &bindless_set_) == VK_SUCCESS,
               "vkAllocateDescriptorSets");
}

Buffer Renderer::create_device_buffer(const void* data, u64 size, VkBufferUsageFlags usage,
                                      bool storage_bindless) {
    const VkDevice dev = device_->handle();

    Allocation staging_alloc{};
    VkBuffer staging = create_buffer(dev, allocator_, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                     &staging_alloc);
    ASSERT(staging_alloc.mapped != nullptr);
    std::memcpy(staging_alloc.mapped, data, size);

    Allocation device_alloc{};
    VkBuffer device_buffer =
        create_buffer(dev, allocator_, size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &device_alloc);

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
    VkBufferCopy copy{};
    copy.size = size;
    vkCmdCopyBuffer(cmd, staging, device_buffer, 1, &copy);
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

    ASSERT_MSG(owned_count_ < MAX_OWNED_BUFFERS, "too many owned buffers");
    owned_buffers_[owned_count_++] = device_buffer;

    Buffer result;
    result.handle = device_buffer;
    result.size = size;
    if (storage_bindless) {
        result.bindless_index = register_storage_buffer(device_buffer);
    }
    return result;
}

u32 Renderer::register_storage_buffer(VkBuffer buffer) {
    const u32 index = next_storage_index_++;
    VkDescriptorBufferInfo info{};
    info.buffer = buffer;
    info.offset = 0;
    info.range = VK_WHOLE_SIZE;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = bindless_set_;
    write.dstBinding = 0;
    write.dstArrayElement = index;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &info;
    vkUpdateDescriptorSets(device_->handle(), 1, &write, 0, nullptr);
    return index;
}

Buffer Renderer::create_gpu_buffer(u64 size, VkBufferUsageFlags usage) {
    const VkDevice dev = device_->handle();
    Allocation alloc{};
    VkBuffer handle = create_buffer(dev, allocator_, size, usage | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &alloc);
    ASSERT_MSG(owned_count_ < MAX_OWNED_BUFFERS, "too many owned buffers");
    owned_buffers_[owned_count_++] = handle;

    Buffer out;
    out.handle = handle;
    out.size = size;
    out.bindless_index = register_storage_buffer(handle);
    return out;
}

Buffer Renderer::create_mapped_buffer(u64 size, void** out_mapped) {
    const VkDevice dev = device_->handle();
    Allocation alloc{};
    VkBuffer handle = create_buffer(dev, allocator_, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                    &alloc);
    ASSERT(alloc.mapped != nullptr);
    ASSERT_MSG(owned_count_ < MAX_OWNED_BUFFERS, "too many owned buffers");
    owned_buffers_[owned_count_++] = handle;
    *out_mapped = alloc.mapped;

    Buffer out;
    out.handle = handle;
    out.size = size;
    out.bindless_index = register_storage_buffer(handle);
    return out;
}

void Renderer::update_device_buffer(const Buffer& buffer, const void* data, u64 size) {
    const VkDevice dev = device_->handle();
    vkQueueWaitIdle(device_->graphics_queue());  // editor event, not the frame loop

    Allocation staging_alloc{};
    VkBuffer staging = create_buffer(dev, allocator_, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                     &staging_alloc);
    ASSERT(staging_alloc.mapped != nullptr);
    std::memcpy(staging_alloc.mapped, data, size);

    VkCommandPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_ci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_ci.queueFamilyIndex = device_->graphics_family();
    VkCommandPool pool = VK_NULL_HANDLE;
    ASSERT_MSG(vkCreateCommandPool(dev, &pool_ci, nullptr, &pool) == VK_SUCCESS, "update pool");
    VkCommandBufferAllocateInfo cba{};
    cba.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cba.commandPool = pool;
    cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cba.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    ASSERT_MSG(vkAllocateCommandBuffers(dev, &cba, &cmd) == VK_SUCCESS, "update cmd");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    VkBufferCopy copy{};
    copy.size = size;
    vkCmdCopyBuffer(cmd, staging, buffer.handle, 1, &copy);
    vkEndCommandBuffer(cmd);
    VkCommandBufferSubmitInfo cmd_info{};
    cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmd_info.commandBuffer = cmd;
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmd_info;
    vkQueueSubmit2(device_->graphics_queue(), 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(device_->graphics_queue());
    vkDestroyCommandPool(dev, pool, nullptr);
    vkDestroyBuffer(dev, staging, nullptr);
}

Texture Renderer::create_texture(const void* rgba, u32 width, u32 height) {
    const VkDevice dev = device_->handle();
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
    ici.format = VK_FORMAT_R8G8B8A8_SRGB;
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
    vci.format = VK_FORMAT_R8G8B8A8_SRGB;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_count, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    ASSERT_MSG(vkCreateImageView(dev, &vci, nullptr, &view) == VK_SUCCESS, "texture view");

    ASSERT_MSG(owned_texture_count_ < MAX_OWNED_TEXTURES, "too many owned textures");
    owned_images_[owned_texture_count_] = image;
    owned_views_[owned_texture_count_] = view;
    ++owned_texture_count_;

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
    vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);

    Texture out;
    out.image = image;
    out.view = view;
    out.mip_count = mip_count;
    out.bindless_index = index;
    return out;
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
