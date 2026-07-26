#include "gpu/gpu.h"

#include "core/assert.h"

#include <volk.h>

#include <cstring>

namespace gpu {

namespace {

VkDeviceAddress buffer_address(VkDevice dev, VkBuffer buffer) {
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = buffer;
    return vkGetBufferDeviceAddress(dev, &info);
}

VkAccelerationStructureGeometryKHR triangle_geometry(VkDeviceAddress verts, u32 stride,
                                                     u32 max_vertex, VkDeviceAddress indices) {
    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geometry.geometry.triangles.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    geometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    geometry.geometry.triangles.vertexData.deviceAddress = verts;
    geometry.geometry.triangles.vertexStride = stride;
    geometry.geometry.triangles.maxVertex = max_vertex;
    geometry.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
    geometry.geometry.triangles.indexData.deviceAddress = indices;
    return geometry;
}

}  // namespace

bool Renderer::rt_available() const { return device_->supports_ray_tracing(); }

Blas Renderer::create_blas(VkBuffer vertices, u32 vertex_count, u32 stride, VkBuffer indices,
                           u32 index_count) {
    ASSERT_MSG(rt_available(), "create_blas without ray tracing");
    const VkDevice dev = device_->handle();

    Blas out;
    out.vertex_count = vertex_count;
    out.index_count = index_count;
    out.stride = stride;
    out.index_address = buffer_address(dev, indices);

    VkAccelerationStructureGeometryKHR geometry = triangle_geometry(
        buffer_address(dev, vertices), stride, vertex_count - 1, out.index_address);

    VkAccelerationStructureBuildGeometryInfoKHR build{};
    build.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                  VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build.geometryCount = 1;
    build.pGeometries = &geometry;

    const u32 primitive_count = index_count / 3;
    VkAccelerationStructureBuildSizesInfoKHR sizes{};
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkGetAccelerationStructureBuildSizesKHR(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                            &build, &primitive_count, &sizes);

    Allocation alloc{};
    VkBuffer storage = create_buffer(dev, allocator_, sizes.accelerationStructureSize,
                                     VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &alloc);
    ASSERT_MSG(owned_count_ < MAX_OWNED_BUFFERS, "too many owned buffers");
    owned_buffers_[owned_count_++] = storage;

    VkBuffer scratch = create_buffer(dev, allocator_, sizes.buildScratchSize,
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                         VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &alloc);
    ASSERT_MSG(owned_count_ < MAX_OWNED_BUFFERS, "too many owned buffers");
    owned_buffers_[owned_count_++] = scratch;
    out.scratch_address = buffer_address(dev, scratch);

    VkAccelerationStructureCreateInfoKHR as_ci{};
    as_ci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    as_ci.buffer = storage;
    as_ci.size = sizes.accelerationStructureSize;
    as_ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    ASSERT_MSG(vkCreateAccelerationStructureKHR(dev, &as_ci, nullptr, &out.handle) == VK_SUCCESS,
               "vkCreateAccelerationStructureKHR");
    ASSERT_MSG(owned_as_count_ < MAX_OWNED_AS, "too many acceleration structures");
    owned_as_[owned_as_count_++] = out.handle;

    VkAccelerationStructureDeviceAddressInfoKHR addr_info{};
    addr_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    addr_info.accelerationStructure = out.handle;
    out.address = vkGetAccelerationStructureDeviceAddressKHR(dev, &addr_info);

    // Build now on a one-shot command buffer. Init-time, not the frame loop.
    build.dstAccelerationStructure = out.handle;
    build.scratchData.deviceAddress = out.scratch_address;
    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = primitive_count;
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;

    VkCommandPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_ci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_ci.queueFamilyIndex = device_->graphics_family();
    VkCommandPool pool = VK_NULL_HANDLE;
    ASSERT_MSG(vkCreateCommandPool(dev, &pool_ci, nullptr, &pool) == VK_SUCCESS, "blas pool");
    VkCommandBufferAllocateInfo cba{};
    cba.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cba.commandPool = pool;
    cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cba.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    ASSERT_MSG(vkAllocateCommandBuffers(dev, &cba, &cmd) == VK_SUCCESS, "blas cmd");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &build, &ranges);
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
    return out;
}

void Renderer::rebuild_blas(VkCommandBuffer cmd, const Blas& blas, VkBuffer vertices,
                            u64 vertex_offset_bytes) {
    const VkDevice dev = device_->handle();
    VkAccelerationStructureGeometryKHR geometry =
        triangle_geometry(buffer_address(dev, vertices) + vertex_offset_bytes, blas.stride,
                          blas.vertex_count - 1, blas.index_address);

    VkAccelerationStructureBuildGeometryInfoKHR build{};
    build.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                  VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build.dstAccelerationStructure = blas.handle;
    build.geometryCount = 1;
    build.pGeometries = &geometry;
    build.scratchData.deviceAddress = blas.scratch_address;

    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = blas.index_count / 3;
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &build, &ranges);
}

void Renderer::create_tlas(u32 max_instances) {
    ASSERT_MSG(rt_available(), "create_tlas without ray tracing");
    const VkDevice dev = device_->handle();
    tlas_max_instances_ = max_instances;

    Allocation instance_alloc{};
    tlas_instance_buffer_ = create_buffer(
        dev, allocator_,
        static_cast<u64>(FRAMES_IN_FLIGHT) * max_instances *
            sizeof(VkAccelerationStructureInstanceKHR),
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        &instance_alloc);
    ASSERT(instance_alloc.mapped != nullptr);
    ASSERT_MSG(owned_count_ < MAX_OWNED_BUFFERS, "too many owned buffers");
    owned_buffers_[owned_count_++] = tlas_instance_buffer_;
    tlas_instances_mapped_ = instance_alloc.mapped;
    tlas_instance_address_ = buffer_address(dev, tlas_instance_buffer_);

    for (u32 slot = 0; slot < FRAMES_IN_FLIGHT; ++slot) {
        VkAccelerationStructureGeometryKHR geometry{};
        geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        geometry.geometry.instances.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;

        VkAccelerationStructureBuildGeometryInfoKHR build{};
        build.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
        build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        build.geometryCount = 1;
        build.pGeometries = &geometry;

        VkAccelerationStructureBuildSizesInfoKHR sizes{};
        sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        vkGetAccelerationStructureBuildSizesKHR(dev,
                                                VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                &build, &max_instances, &sizes);

        Allocation alloc{};
        VkBuffer storage = create_buffer(dev, allocator_, sizes.accelerationStructureSize,
                                         VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &alloc);
        ASSERT_MSG(owned_count_ < MAX_OWNED_BUFFERS, "too many owned buffers");
        owned_buffers_[owned_count_++] = storage;
        VkBuffer scratch = create_buffer(dev, allocator_, sizes.buildScratchSize,
                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &alloc);
        ASSERT_MSG(owned_count_ < MAX_OWNED_BUFFERS, "too many owned buffers");
        owned_buffers_[owned_count_++] = scratch;
        tlas_scratch_address_[slot] = buffer_address(dev, scratch);

        VkAccelerationStructureCreateInfoKHR as_ci{};
        as_ci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        as_ci.buffer = storage;
        as_ci.size = sizes.accelerationStructureSize;
        as_ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        ASSERT_MSG(vkCreateAccelerationStructureKHR(dev, &as_ci, nullptr, &tlas_[slot]) ==
                       VK_SUCCESS,
                   "tlas create");
        ASSERT_MSG(owned_as_count_ < MAX_OWNED_AS, "too many acceleration structures");
        owned_as_[owned_as_count_++] = tlas_[slot];

        // Bindless binding 5 holds one TLAS per frame slot, written once here:
        // rebuilds update the same object, so the descriptor stays valid.
        VkWriteDescriptorSetAccelerationStructureKHR as_write{};
        as_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        as_write.accelerationStructureCount = 1;
        as_write.pAccelerationStructures = &tlas_[slot];
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.pNext = &as_write;
        write.dstSet = bindless_set_;
        write.dstBinding = 5;
        write.dstArrayElement = slot;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);
    }
}

void Renderer::update_tlas(VkCommandBuffer cmd,
                           core::Span<const VkAccelerationStructureInstanceKHR> instances,
                           u32 slot) {
    ASSERT(instances.size() <= tlas_max_instances_);
    const u64 offset = static_cast<u64>(slot) * tlas_max_instances_ *
                       sizeof(VkAccelerationStructureInstanceKHR);
    std::memcpy(static_cast<u8*>(tlas_instances_mapped_) + offset, instances.data(),
                instances.size() * sizeof(VkAccelerationStructureInstanceKHR));

    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geometry.geometry.instances.data.deviceAddress = tlas_instance_address_ + offset;

    VkAccelerationStructureBuildGeometryInfoKHR build{};
    build.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build.dstAccelerationStructure = tlas_[slot];
    build.geometryCount = 1;
    build.pGeometries = &geometry;
    build.scratchData.deviceAddress = tlas_scratch_address_[slot];

    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = static_cast<u32>(instances.size());
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &build, &ranges);
}

}  // namespace gpu
