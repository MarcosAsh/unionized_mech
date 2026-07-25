#include "gpu/gpu.h"

#include "core/log.h"

#include <volk.h>

namespace gpu {

using DeviceResult = core::Result<Device, Error>;

namespace {

constexpr u32 MAX_QUEUE_FAMILIES = 16;

bool has_required_features(VkPhysicalDevice pd) {
    VkPhysicalDeviceVulkan13Features f13{};
    f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceVulkan12Features f12{};
    f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    f12.pNext = &f13;
    VkPhysicalDeviceFeatures2 f2{};
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(pd, &f2);

    return f13.dynamicRendering && f13.synchronization2 && f12.timelineSemaphore &&
           f12.bufferDeviceAddress && f12.descriptorIndexing && f12.runtimeDescriptorArray &&
           f12.descriptorBindingPartiallyBound &&
           f12.descriptorBindingSampledImageUpdateAfterBind &&
           f12.descriptorBindingStorageImageUpdateAfterBind &&
           f12.descriptorBindingStorageBufferUpdateAfterBind &&
           f12.shaderSampledImageArrayNonUniformIndexing;
}

// Prefer real GPUs. CPU (llvmpipe) scores zero and is rejected.
u32 device_score(VkPhysicalDeviceType type) {
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            return 3;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            return 2;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            return 1;
        default:
            return 0;
    }
}

void read_families(VkPhysicalDevice pd, VkQueueFamilyProperties* props, u32* count) {
    vkGetPhysicalDeviceQueueFamilyProperties(pd, count, nullptr);
    if (*count > MAX_QUEUE_FAMILIES) {
        *count = MAX_QUEUE_FAMILIES;
    }
    vkGetPhysicalDeviceQueueFamilyProperties(pd, count, props);
}

bool find_graphics_family(VkPhysicalDevice pd, u32* out) {
    VkQueueFamilyProperties props[MAX_QUEUE_FAMILIES];
    u32 count = 0;
    read_families(pd, props, &count);
    for (u32 i = 0; i < count; ++i) {
        if ((props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
            *out = i;
            return true;
        }
    }
    return false;
}

bool find_transfer_family(VkPhysicalDevice pd, u32 graphics, u32* out) {
    VkQueueFamilyProperties props[MAX_QUEUE_FAMILIES];
    u32 count = 0;
    read_families(pd, props, &count);
    for (u32 i = 0; i < count; ++i) {
        const bool xfer = (props[i].queueFlags & VK_QUEUE_TRANSFER_BIT) != 0;
        const bool gfx = (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
        const bool comp = (props[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0;
        if (xfer && !gfx && !comp && i != graphics) {
            *out = i;
            return true;
        }
    }
    return false;
}

void log_device(VkPhysicalDevice pd) {
    VkPhysicalDeviceDriverProperties driver{};
    driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    VkPhysicalDeviceProperties2 props{};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props.pNext = &driver;
    vkGetPhysicalDeviceProperties2(pd, &props);

    const u32 v = props.properties.apiVersion;
    core::log_infof("gpu: %s | %s %s | Vulkan %u.%u.%u", props.properties.deviceName,
                    driver.driverName, driver.driverInfo, VK_API_VERSION_MAJOR(v),
                    VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
}

VkDeviceQueueCreateInfo queue_info(u32 family, const f32* priority) {
    VkDeviceQueueCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    ci.queueFamilyIndex = family;
    ci.queueCount = 1;
    ci.pQueuePriorities = priority;
    return ci;
}

}  // namespace

DeviceResult Device::create(const Instance& instance) {
    u32 count = 0;
    vkEnumeratePhysicalDevices(instance.handle(), &count, nullptr);
    if (count == 0) {
        return DeviceResult::err(Error::NoSuitableDevice);
    }
    if (count > 16) {
        count = 16;
    }
    VkPhysicalDevice devices[16];
    vkEnumeratePhysicalDevices(instance.handle(), &count, devices);

    VkPhysicalDevice best = VK_NULL_HANDLE;
    u32 best_score = 0;
    u32 best_graphics = 0;
    for (u32 i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devices[i], &p);
        if (p.apiVersion < VK_API_VERSION_1_3) {
            continue;
        }
        const u32 s = device_score(p.deviceType);
        if (s == 0) {
            continue;
        }
        if (!has_required_features(devices[i])) {
            continue;
        }
        u32 graphics = 0;
        if (!find_graphics_family(devices[i], &graphics)) {
            continue;
        }
        if (s > best_score) {
            best = devices[i];
            best_score = s;
            best_graphics = graphics;
        }
    }
    if (best == VK_NULL_HANDLE) {
        return DeviceResult::err(Error::NoSuitableDevice);
    }
    log_device(best);

    const f32 priority = 1.0f;
    VkDeviceQueueCreateInfo queues[2];
    u32 queue_count = 0;
    queues[queue_count++] = queue_info(best_graphics, &priority);

    u32 transfer_family = 0;
    const bool has_transfer = find_transfer_family(best, best_graphics, &transfer_family);
    if (has_transfer) {
        queues[queue_count++] = queue_info(transfer_family, &priority);
    }

    VkPhysicalDeviceVulkan13Features e13{};
    e13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    e13.dynamicRendering = VK_TRUE;
    e13.synchronization2 = VK_TRUE;

    VkPhysicalDeviceVulkan12Features e12{};
    e12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    e12.pNext = &e13;
    e12.timelineSemaphore = VK_TRUE;
    e12.bufferDeviceAddress = VK_TRUE;
    e12.descriptorIndexing = VK_TRUE;
    e12.runtimeDescriptorArray = VK_TRUE;
    e12.descriptorBindingPartiallyBound = VK_TRUE;
    e12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    e12.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
    e12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
    e12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;

    VkPhysicalDeviceFeatures2 e2{};
    e2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    e2.pNext = &e12;

    const char* device_extensions[1] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &e2;
    dci.queueCreateInfoCount = queue_count;
    dci.pQueueCreateInfos = queues;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = device_extensions;

    VkDevice device = VK_NULL_HANDLE;
    if (vkCreateDevice(best, &dci, nullptr, &device) != VK_SUCCESS) {
        return DeviceResult::err(Error::DeviceCreate);
    }
    volkLoadDevice(device);

    Device out;
    out.physical_ = best;
    out.device_ = device;
    out.graphics_family_ = best_graphics;
    vkGetDeviceQueue(device, best_graphics, 0, &out.graphics_queue_);
    if (has_transfer) {
        out.transfer_family_ = transfer_family;
        vkGetDeviceQueue(device, transfer_family, 0, &out.transfer_queue_);
    }

    core::log_infof("gpu: graphics queue family %u%s", best_graphics,
                    has_transfer ? ", dedicated transfer queue present" : "");
    return DeviceResult::ok(static_cast<Device&&>(out));
}

Device::~Device() {
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
    }
}

Device::Device(Device&& other) noexcept
    : physical_(other.physical_),
      device_(other.device_),
      graphics_family_(other.graphics_family_),
      graphics_queue_(other.graphics_queue_),
      transfer_family_(other.transfer_family_),
      transfer_queue_(other.transfer_queue_) {
    other.device_ = VK_NULL_HANDLE;
}

Device& Device::operator=(Device&& other) noexcept {
    if (this != &other) {
        if (device_ != VK_NULL_HANDLE) {
            vkDestroyDevice(device_, nullptr);
        }
        physical_ = other.physical_;
        device_ = other.device_;
        graphics_family_ = other.graphics_family_;
        graphics_queue_ = other.graphics_queue_;
        transfer_family_ = other.transfer_family_;
        transfer_queue_ = other.transfer_queue_;
        other.device_ = VK_NULL_HANDLE;
    }
    return *this;
}

}  // namespace gpu
