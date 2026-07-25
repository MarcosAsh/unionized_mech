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

/// Owns a Vulkan surface and destroys it on the given instance. The window
/// creates the surface; this wrapper gives it a lifetime tied to RAII so it is
/// torn down before the instance.
class Surface {
public:
    /// Take ownership of `surface`, which must have been created on `instance`.
    [[nodiscard]] static Surface adopt(const Instance& instance, VkSurfaceKHR surface);

    ~Surface();
    Surface(Surface&& other) noexcept;
    Surface& operator=(Surface&& other) noexcept;
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;

    [[nodiscard]] VkSurfaceKHR handle() const { return surface_; }

private:
    Surface() = default;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
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

/// Owns the swapchain and triple-buffered frame contexts, and presents frames.
/// Pacing uses a single timeline semaphore; the swapchain handshake uses binary
/// semaphores as the present engine requires. Never waits the whole device idle
/// inside the frame loop.
/// # Invariants
/// Borrows the Device and surface; both must outlive the Renderer.
class Renderer {
public:
    /// Create the swapchain and per-frame resources for `surface` at the given
    /// drawable size.
    /// # Errors
    /// NoSuitableDevice if the graphics queue cannot present to the surface.
    [[nodiscard]] static core::Result<Renderer, Error> create(const Device& device,
                                                              VkSurfaceKHR surface, u32 width,
                                                              u32 height);

    ~Renderer();
    Renderer(Renderer&& other) noexcept;
    Renderer& operator=(Renderer&& other) noexcept;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    /// Clear the swapchain image to (r, g, b) and present it. `width` and
    /// `height` are the current drawable size, used to detect and handle resize.
    /// Does nothing while the window is zero-sized.
    void render_clear(f32 r, f32 g, f32 b, u32 width, u32 height);

private:
    Renderer() = default;

    static constexpr u32 FRAMES_IN_FLIGHT = 3;
    static constexpr u32 MAX_IMAGES = 8;

    bool recreate_swapchain(u32 width, u32 height);
    void destroy_image_objects();

    const Device* device_ = nullptr;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_ = {0, 0};
    u32 image_count_ = 0;
    VkImage images_[MAX_IMAGES] = {};
    VkImageView views_[MAX_IMAGES] = {};
    VkSemaphore render_finished_[MAX_IMAGES] = {};

    VkSemaphore image_available_[FRAMES_IN_FLIGHT] = {};
    VkCommandPool pools_[FRAMES_IN_FLIGHT] = {};
    VkCommandBuffer cmds_[FRAMES_IN_FLIGHT] = {};
    VkSemaphore timeline_ = VK_NULL_HANDLE;
    u64 frame_counter_ = 0;
};

}  // namespace gpu
