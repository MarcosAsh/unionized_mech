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
    /// Create the instance with the window's required extensions.
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

/// Owns a Vulkan surface and destroys it on the given instance.
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
    /// Select a Vulkan 1.3 device with the M0 feature set and create the logical
    /// device.
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

/// A slice of device memory handed out by the Allocator.
struct Allocation {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    void* mapped = nullptr;  // non-null only for host-visible memory
};

/// A bump sub-allocator over large VkDeviceMemory blocks, one run per memory
/// type. Frees everything at shutdown. The interface can grow a free list later
/// without callers changing.
class Allocator {
public:
    void init(VkPhysicalDevice pd, VkDevice device);
    void shutdown();
    [[nodiscard]] Allocation allocate(const VkMemoryRequirements& req, VkMemoryPropertyFlags props);

private:
    static constexpr u32 MAX_BLOCKS = 32;
    static constexpr VkDeviceSize BLOCK_SIZE = 32ull * 1024 * 1024;

    struct Block {
        VkDeviceMemory memory;
        VkDeviceSize size;
        VkDeviceSize used;
        u32 type_index;
        void* mapped;
    };

    [[nodiscard]] u32 find_memory_type(u32 type_bits, VkMemoryPropertyFlags props) const;

    VkPhysicalDevice pd_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties mem_props_{};
    Block blocks_[MAX_BLOCKS]{};
    u32 block_count_ = 0;
};

/// Create a buffer and bind freshly sub-allocated memory to it.
[[nodiscard]] VkBuffer create_buffer(VkDevice device, Allocator& alloc, u64 size,
                                     VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                                     Allocation* out_alloc);

/// A GPU buffer and, when registered, its slot in the bindless storage array.
struct Buffer {
    VkBuffer handle = VK_NULL_HANDLE;
    u64 size = 0;
    u32 bindless_index = 0xFFFFFFFFu;
};

/// A sampled GPU texture and its slot in the bindless sampled image array.
struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    u32 mip_count = 0;
    u32 bindless_index = 0xFFFFFFFFu;
};

/// One acquired frame: the command buffer to record into and its attachments.
/// `valid` is false when the frame was skipped, for example during a resize.
struct Frame {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkImageView color_view = VK_NULL_HANDLE;
    VkImageView depth_view = VK_NULL_HANDLE;
    VkExtent2D extent = {0, 0};
    bool valid = false;
};

/// Owns the swapchain, a depth buffer, the bindless descriptor set, the memory
/// allocator, and triple-buffered frame contexts. Presents frames. Pacing uses
/// a single timeline semaphore; the swapchain handshake uses binary semaphores.
/// Never waits the whole device idle inside the frame loop.
/// # Invariants
/// Borrows the Device and surface; both must outlive the Renderer.
class Renderer {
public:
    /// Create the swapchain, depth buffer, bindless set, and per-frame resources.
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

    /// Upload `size` bytes into a new device-local buffer. When `storage_bindless`
    /// is set the buffer is registered in the bindless storage array and its slot
    /// returned in Buffer::bindless_index. Init-time only.
    [[nodiscard]] Buffer create_device_buffer(const void* data, u64 size, VkBufferUsageFlags usage,
                                              bool storage_bindless);

    /// Upload a tightly packed RGBA8 image as an sRGB texture with a full mip
    /// chain generated by blits, and register it in the bindless sampled image
    /// array. Init-time only.
    [[nodiscard]] Texture create_texture(const void* rgba, u32 width, u32 height);

    [[nodiscard]] VkDevice device_handle() const;
    [[nodiscard]] VkFormat color_format() const { return format_; }
    [[nodiscard]] VkFormat depth_format() const { return DEPTH_FORMAT; }
    [[nodiscard]] VkDescriptorSetLayout bindless_layout() const { return bindless_layout_; }
    [[nodiscard]] VkDescriptorSet bindless_set() const { return bindless_set_; }

    /// Acquire and begin the next frame, handling resize with the given drawable
    /// size. The returned command buffer has its color and depth images already
    /// transitioned for rendering. Check Frame::valid before using it.
    [[nodiscard]] Frame begin_frame(u32 width, u32 height);

    /// Transition for present, submit, and present the frame from begin_frame.
    void end_frame();

    /// Convenience: begin a frame, clear it to (r, g, b), and present.
    void render_clear(f32 r, f32 g, f32 b, u32 width, u32 height);

    /// GPU time in milliseconds for the most recently completed frame, from
    /// timestamp queries. Zero until enough frames have retired.
    [[nodiscard]] f32 last_gpu_ms() const { return last_gpu_ms_; }

private:
    Renderer() = default;

    static constexpr u32 FRAMES_IN_FLIGHT = 3;
    static constexpr u32 MAX_IMAGES = 8;
    static constexpr u32 MAX_OWNED_BUFFERS = 16;
    static constexpr u32 MAX_OWNED_TEXTURES = 64;
    static constexpr u32 BINDLESS_STORAGE_COUNT = 1024;
    static constexpr u32 BINDLESS_SAMPLED_COUNT = 1024;
    static constexpr u32 BINDLESS_STORAGE_IMAGE_COUNT = 256;
    static constexpr VkFormat DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;

    bool recreate_swapchain(u32 width, u32 height);
    void destroy_image_objects();
    void init_bindless();
    void create_depth();

    const Device* device_ = nullptr;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    Allocator allocator_{};

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_ = {0, 0};
    u32 image_count_ = 0;
    VkImage images_[MAX_IMAGES] = {};
    VkImageView views_[MAX_IMAGES] = {};
    VkSemaphore render_finished_[MAX_IMAGES] = {};

    VkImage depth_image_ = VK_NULL_HANDLE;
    VkImageView depth_view_ = VK_NULL_HANDLE;

    VkSemaphore image_available_[FRAMES_IN_FLIGHT] = {};
    VkCommandPool pools_[FRAMES_IN_FLIGHT] = {};
    VkCommandBuffer cmds_[FRAMES_IN_FLIGHT] = {};
    VkSemaphore timeline_ = VK_NULL_HANDLE;
    u64 frame_counter_ = 0;

    VkDescriptorPool bindless_pool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout bindless_layout_ = VK_NULL_HANDLE;
    VkDescriptorSet bindless_set_ = VK_NULL_HANDLE;
    VkSampler default_sampler_ = VK_NULL_HANDLE;
    u32 next_storage_index_ = 0;
    u32 next_sampled_index_ = 0;

    VkImage owned_images_[MAX_OWNED_TEXTURES] = {};
    VkImageView owned_views_[MAX_OWNED_TEXTURES] = {};
    u32 owned_texture_count_ = 0;

    VkQueryPool timestamp_pool_ = VK_NULL_HANDLE;
    f32 timestamp_period_ = 1.0f;
    f32 last_gpu_ms_ = 0.0f;

    VkBuffer owned_buffers_[MAX_OWNED_BUFFERS] = {};
    u32 owned_count_ = 0;

    u32 cur_image_ = 0;
    u32 cur_slot_ = 0;
    u64 cur_submit_ = 0;
    u32 cur_w_ = 0;
    u32 cur_h_ = 0;
};

}  // namespace gpu
