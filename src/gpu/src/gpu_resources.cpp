#include "gpu/gpu.h"

#include "core/assert.h"

#include <volk.h>

#include <cstring>

// Buffers and the bindless descriptor set. Images live in gpu_textures.cpp.

namespace gpu {

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

    // The shadow comparison sampler: hardware PCF per tap, clamped to border
    // white so samples outside the sun volume read as lit.
    VkSamplerCreateInfo shadow_ci{};
    shadow_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    shadow_ci.magFilter = VK_FILTER_LINEAR;
    shadow_ci.minFilter = VK_FILTER_LINEAR;
    shadow_ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    shadow_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    shadow_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    shadow_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    shadow_ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    shadow_ci.compareEnable = VK_TRUE;
    shadow_ci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    ASSERT_MSG(vkCreateSampler(dev, &shadow_ci, nullptr, &shadow_sampler_) == VK_SUCCESS,
               "shadow sampler");

    const VkDescriptorPoolSize sizes[5] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, BINDLESS_STORAGE_COUNT},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, BINDLESS_SAMPLED_COUNT},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, BINDLESS_STORAGE_IMAGE_COUNT},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 2},
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, FRAMES_IN_FLIGHT},
    };
    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pool_ci.maxSets = 1;
    pool_ci.poolSizeCount = device_->supports_ray_tracing() ? 5 : 4;
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

    VkDescriptorSetLayoutBinding bindings[6]{};
    VkDescriptorBindingFlags flags[6];
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
    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_ALL;
    bindings[4].pImmutableSamplers = &shadow_sampler_;
    flags[4] = 0;
    u32 binding_count = 5;
    if (device_->supports_ray_tracing()) {
        // One TLAS per frame slot for the ray-query shaders.
        bindings[5].binding = 5;
        bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        bindings[5].descriptorCount = FRAMES_IN_FLIGHT;
        bindings[5].stageFlags = VK_SHADER_STAGE_ALL;
        flags[5] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        binding_count = 6;
    }

    VkDescriptorSetLayoutBindingFlagsCreateInfo flag_ci{};
    flag_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flag_ci.bindingCount = binding_count;
    flag_ci.pBindingFlags = flags;

    VkDescriptorSetLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.pNext = &flag_ci;
    layout_ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layout_ci.bindingCount = binding_count;
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

    // On RT hardware every device buffer can feed acceleration structure
    // builds and be read by address.
    if (device_->supports_ray_tracing()) {
        usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
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
    if (device_->supports_ray_tracing()) {
        usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
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

}  // namespace gpu
