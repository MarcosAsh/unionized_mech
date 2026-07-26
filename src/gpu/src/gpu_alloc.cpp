#include "gpu/gpu.h"

#include "core/assert.h"

#include <volk.h>

namespace gpu {

void Allocator::init(VkPhysicalDevice pd, VkDevice device) {
    pd_ = pd;
    device_ = device;
    vkGetPhysicalDeviceMemoryProperties(pd, &mem_props_);
}

void Allocator::shutdown() {
    for (u32 i = 0; i < block_count_; ++i) {
        if (blocks_[i].mapped != nullptr) {
            vkUnmapMemory(device_, blocks_[i].memory);
        }
        vkFreeMemory(device_, blocks_[i].memory, nullptr);
    }
    block_count_ = 0;
}

u32 Allocator::find_memory_type(u32 type_bits, VkMemoryPropertyFlags props) const {
    for (u32 i = 0; i < mem_props_.memoryTypeCount; ++i) {
        const bool type_ok = (type_bits & (1u << i)) != 0;
        const bool props_ok = (mem_props_.memoryTypes[i].propertyFlags & props) == props;
        if (type_ok && props_ok) {
            return i;
        }
    }
    PANIC("no suitable Vulkan memory type");
    return 0;
}

Allocation Allocator::allocate(const VkMemoryRequirements& req, VkMemoryPropertyFlags props) {
    const u32 type = find_memory_type(req.memoryTypeBits, props);

    for (u32 i = 0; i < block_count_; ++i) {
        Block& b = blocks_[i];
        if (b.type_index != type) {
            continue;
        }
        const VkDeviceSize aligned = (b.used + req.alignment - 1) & ~(req.alignment - 1);
        if (aligned + req.size <= b.size) {
            b.used = aligned + req.size;
            Allocation a;
            a.memory = b.memory;
            a.offset = aligned;
            a.mapped = b.mapped != nullptr ? static_cast<u8*>(b.mapped) + aligned : nullptr;
            return a;
        }
    }

    ASSERT_MSG(block_count_ < MAX_BLOCKS, "allocator out of blocks");
    VkDeviceSize size = BLOCK_SIZE;
    if (req.size > size) {
        size = req.size;
    }

    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = size;
    ai.memoryTypeIndex = type;

    Block block{};
    block.size = size;
    block.type_index = type;
    ASSERT_MSG(vkAllocateMemory(device_, &ai, nullptr, &block.memory) == VK_SUCCESS,
               "vkAllocateMemory");

    // Map from the memory type's own flags, not the request. On unified memory a
    // device-local type is also host-visible, and its blocks may later be reused
    // for host-visible allocations, so those blocks must be mapped.
    const bool type_host_visible =
        (mem_props_.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    if (type_host_visible) {
        ASSERT_MSG(vkMapMemory(device_, block.memory, 0, VK_WHOLE_SIZE, 0, &block.mapped) ==
                       VK_SUCCESS,
                   "vkMapMemory");
    }
    block.used = req.size;
    blocks_[block_count_] = block;
    ++block_count_;

    Allocation a;
    a.memory = block.memory;
    a.offset = 0;
    a.mapped = block.mapped;
    return a;
}

VkBuffer create_buffer(VkDevice device, Allocator& alloc, u64 size, VkBufferUsageFlags usage,
                       VkMemoryPropertyFlags props, Allocation* out_alloc) {
    VkBufferCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer buffer = VK_NULL_HANDLE;
    ASSERT_MSG(vkCreateBuffer(device, &ci, nullptr, &buffer) == VK_SUCCESS, "vkCreateBuffer");

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, buffer, &req);
    const Allocation a = alloc.allocate(req, props);
    ASSERT_MSG(vkBindBufferMemory(device, buffer, a.memory, a.offset) == VK_SUCCESS,
               "vkBindBufferMemory");

    *out_alloc = a;
    return buffer;
}

}  // namespace gpu
