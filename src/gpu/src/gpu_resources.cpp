#include "gpu/gpu.h"

#include "core/assert.h"

#include <volk.h>

#include <cstring>

namespace gpu {

VkDevice Renderer::device_handle() const { return device_->handle(); }

void Renderer::init_bindless() {
    const VkDevice dev = device_->handle();

    const VkDescriptorPoolSize sizes[3] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, BINDLESS_STORAGE_COUNT},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, BINDLESS_SAMPLED_COUNT},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, BINDLESS_STORAGE_IMAGE_COUNT},
    };
    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pool_ci.maxSets = 1;
    pool_ci.poolSizeCount = 3;
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

    VkDescriptorSetLayoutBinding bindings[3]{};
    VkDescriptorBindingFlags flags[3];
    for (u32 i = 0; i < 3; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = types[i];
        bindings[i].descriptorCount = counts[i];
        bindings[i].stageFlags = VK_SHADER_STAGE_ALL;
        flags[i] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                   VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    }

    VkDescriptorSetLayoutBindingFlagsCreateInfo flag_ci{};
    flag_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flag_ci.bindingCount = 3;
    flag_ci.pBindingFlags = flags;

    VkDescriptorSetLayoutCreateInfo layout_ci{};
    layout_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_ci.pNext = &flag_ci;
    layout_ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layout_ci.bindingCount = 3;
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
        const u32 index = next_storage_index_++;
        VkDescriptorBufferInfo info{};
        info.buffer = device_buffer;
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
        vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);
        result.bindless_index = index;
    }
    return result;
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
