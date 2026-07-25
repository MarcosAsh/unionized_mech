#pragma once

#include "core/result.h"
#include "core/span.h"
#include "core/types.h"

#include <vulkan/vulkan.h>

namespace gpu {

/// Why a gpu bring-up step failed.
enum class Error : u8 {
    VolkInit,
    InstanceCreate,
    NoSuitableDevice,
    DeviceCreate,
};

/// Human-readable name for an Error.
[[nodiscard]] const char* to_string(Error e);

/// Vulkan instance plus, in debug, a validation debug messenger that treats any
/// warning or error as fatal. Owns volk initialization.
class Instance {
public:
    /// Create the instance with the window's required extensions. When
    /// `enable_validation` is true the Khronos validation layer and the fatal
    /// messenger are attached.
    /// # Errors
    /// VolkInit if the loader is missing, InstanceCreate if creation fails.
    [[nodiscard]] static core::Result<Instance, Error> create(
        core::Span<const char* const> window_extensions, bool enable_validation);

    ~Instance();
    Instance(Instance&& other) noexcept;
    Instance& operator=(Instance&& other) noexcept;
    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;

    [[nodiscard]] VkInstance handle() const { return instance_; }

private:
    Instance() = default;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
};

/// The chosen physical device and a logical device with a graphics queue, plus
/// a dedicated transfer queue when the hardware exposes one.
class Device {
public:
    /// Select a Vulkan 1.3 device with the M0 feature set (dynamic rendering,
    /// synchronization2, timeline semaphores, buffer device address, descriptor
    /// indexing), rejecting CPU devices, and create the logical device.
    /// # Errors
    /// NoSuitableDevice if nothing qualifies, DeviceCreate if creation fails.
    [[nodiscard]] static core::Result<Device, Error> create(const Instance& instance);

    ~Device();
    Device(Device&& other) noexcept;
    Device& operator=(Device&& other) noexcept;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    [[nodiscard]] VkPhysicalDevice physical() const { return physical_; }
    [[nodiscard]] VkDevice handle() const { return device_; }
    [[nodiscard]] u32 graphics_family() const { return graphics_family_; }
    [[nodiscard]] VkQueue graphics_queue() const { return graphics_queue_; }
    [[nodiscard]] bool has_transfer_queue() const { return transfer_queue_ != VK_NULL_HANDLE; }
    [[nodiscard]] u32 transfer_family() const { return transfer_family_; }
    [[nodiscard]] VkQueue transfer_queue() const { return transfer_queue_; }

private:
    Device() = default;

    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    u32 graphics_family_ = 0;
    VkQueue graphics_queue_ = VK_NULL_HANDLE;
    u32 transfer_family_ = 0;
    VkQueue transfer_queue_ = VK_NULL_HANDLE;
};

}  // namespace gpu
