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
    /// True when the ray tracing extension set is enabled on this device.
    [[nodiscard]] bool supports_ray_tracing() const { return ray_tracing_; }
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
    bool ray_tracing_ = false;
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
    void init(VkPhysicalDevice pd, VkDevice device, bool device_address);
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
    bool device_address_ = false;
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

/// A bottom-level acceleration structure over one mesh, plus what a rebuild
/// needs. Static geometry builds once; dynamic (skinned) geometry rebuilds per
/// frame from a new vertex slice.
struct Blas {
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
    VkDeviceAddress scratch_address = 0;
    VkDeviceAddress index_address = 0;
    u32 vertex_count = 0;
    u32 index_count = 0;
    u32 stride = 0;
};

/// One acquired frame: the command buffer to record into and its attachments.
/// `valid` is false when the frame was skipped, for example during a resize.
struct Frame {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkImageView color_view = VK_NULL_HANDLE;
    VkImageView depth_view = VK_NULL_HANDLE;
    VkExtent2D extent = {0, 0};
    u32 slot = 0;  ///< Frame-in-flight slot, for per-frame buffer slices.
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
    /// Upload an RGBA8 image with a full mip chain. `srgb` decodes the source
    /// as colour; pass false for data maps like normals and roughness.
    [[nodiscard]] Texture create_texture(const void* rgba, u32 width, u32 height,
                                         bool srgb = true);

    /// Create an empty device-local buffer the GPU writes at runtime, registered
    /// in the bindless storage array. Init-time only.
    [[nodiscard]] Buffer create_gpu_buffer(u64 size, VkBufferUsageFlags usage);

    /// Create a persistently mapped host-visible storage buffer, registered in
    /// the bindless storage array. `out_mapped` receives the CPU pointer.
    /// Init-time only.
    [[nodiscard]] Buffer create_mapped_buffer(u64 size, void** out_mapped);

    /// Replace the first `size` bytes of an existing device buffer via a staged
    /// copy, waiting for the queue to drain first. An editor-event operation
    /// (map reload), never part of the frame loop.
    void update_device_buffer(const Buffer& buffer, const void* data, u64 size);

    /// True when the device has the ray tracing extensions enabled.
    [[nodiscard]] bool rt_available() const;

    /// Build a triangle BLAS over `vertices` (position at offset 0 of `stride`)
    /// and `indices`, built and ready on return. Init-time only. The scratch is
    /// kept so dynamic geometry can rebuild via rebuild_blas.
    [[nodiscard]] Blas create_blas(VkBuffer vertices, u32 vertex_count, u32 stride,
                                   VkBuffer indices, u32 index_count);

    /// Re-record a BLAS build over new vertex data (a skinned slice), on the
    /// frame command buffer. The caller provides barriers around it.
    void rebuild_blas(VkCommandBuffer cmd, const Blas& blas, VkBuffer vertices,
                      u64 vertex_offset_bytes);

    /// Create the per-frame top-level structures for at most `max_instances`
    /// and register them at bindless binding 5. Init-time only.
    void create_tlas(u32 max_instances);

    /// Rebuild this frame slot's TLAS from `instances` on the frame command
    /// buffer, with the barriers making it visible to fragment shaders.
    void update_tlas(VkCommandBuffer cmd, core::Span<const VkAccelerationStructureInstanceKHR> instances,
                     u32 slot);

    /// The frame-in-flight slot of the frame begun by begin_frame, in
    /// [0, frames_in_flight). Per-frame CPU-written ranges key off this.
    [[nodiscard]] u32 frame_slot() const { return cur_slot_; }

    /// The sun shadow map depth target and its bindless sampled slot.
    [[nodiscard]] VkImage shadow_image() const { return shadow_image_; }
    [[nodiscard]] VkImageView shadow_view() const { return shadow_view_; }
    [[nodiscard]] u32 shadow_bindless() const { return shadow_bindless_; }
    /// Shadow map resolution in texels per side.
    static constexpr u32 shadow_size() { return 4096; }
    /// Number of frames in flight, for sizing per-frame ranges.
    [[nodiscard]] static constexpr u32 frames_in_flight() { return FRAMES_IN_FLIGHT; }

    [[nodiscard]] VkDevice device_handle() const;
    [[nodiscard]] VkFormat color_format() const { return format_; }

    /// Ask for the next presented frame to be read back to the CPU. A debug and
    /// test path, not a frame-loop one: it blocks on the queue to do it. This
    /// exists so a change to how the game looks can be checked by looking at it.
    void request_capture() { capture_wanted_ = true; }

    /// The frame read back by the last request, as tightly packed RGBA8. Null
    /// until a requested capture has landed; valid until the next request.
    [[nodiscard]] const u8* capture_pixels() const { return capture_rgba_; }
    [[nodiscard]] u32 capture_width() const { return capture_w_; }
    [[nodiscard]] u32 capture_height() const { return capture_h_; }
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
    static constexpr u32 MAX_OWNED_BUFFERS = 128;
    static constexpr u32 MAX_OWNED_TEXTURES = 64;
    static constexpr u32 BINDLESS_STORAGE_COUNT = 1024;
    static constexpr u32 BINDLESS_SAMPLED_COUNT = 1024;
    static constexpr u32 BINDLESS_STORAGE_IMAGE_COUNT = 256;
    static constexpr VkFormat DEPTH_FORMAT = VK_FORMAT_D32_SFLOAT;

    bool recreate_swapchain(u32 width, u32 height);
    void destroy_image_objects();
    void init_bindless();
    void create_depth();
    void create_shadow_map();
    [[nodiscard]] u32 register_storage_buffer(VkBuffer buffer);
    [[nodiscard]] u32 register_sampled_image(VkImageView view);

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

    VkImage shadow_image_ = VK_NULL_HANDLE;
    VkImageView shadow_view_ = VK_NULL_HANDLE;
    u32 shadow_bindless_ = 0;

    VkSemaphore image_available_[FRAMES_IN_FLIGHT] = {};
    VkCommandPool pools_[FRAMES_IN_FLIGHT] = {};
    VkCommandBuffer cmds_[FRAMES_IN_FLIGHT] = {};
    VkSemaphore timeline_ = VK_NULL_HANDLE;
    u64 frame_counter_ = 0;

    VkDescriptorPool bindless_pool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout bindless_layout_ = VK_NULL_HANDLE;
    VkDescriptorSet bindless_set_ = VK_NULL_HANDLE;
    VkSampler default_sampler_ = VK_NULL_HANDLE;
    VkSampler shadow_sampler_ = VK_NULL_HANDLE;
    u32 next_storage_index_ = 0;
    u32 next_sampled_index_ = 0;

    VkImage owned_images_[MAX_OWNED_TEXTURES] = {};
    VkImageView owned_views_[MAX_OWNED_TEXTURES] = {};
    u32 owned_texture_count_ = 0;

    static constexpr u32 MAX_OWNED_AS = 16;
    VkAccelerationStructureKHR owned_as_[MAX_OWNED_AS] = {};
    u32 owned_as_count_ = 0;
    VkAccelerationStructureKHR tlas_[FRAMES_IN_FLIGHT] = {};
    VkDeviceAddress tlas_scratch_address_[FRAMES_IN_FLIGHT] = {};
    VkBuffer tlas_instance_buffer_ = VK_NULL_HANDLE;
    VkDeviceAddress tlas_instance_address_ = 0;
    void* tlas_instances_mapped_ = nullptr;
    u32 tlas_max_instances_ = 0;

    VkQueryPool timestamp_pool_ = VK_NULL_HANDLE;
    f32 timestamp_period_ = 1.0f;
    f32 last_gpu_ms_ = 0.0f;

    VkBuffer owned_buffers_[MAX_OWNED_BUFFERS] = {};
    u32 owned_count_ = 0;

    u32 cur_image_ = 0;

    /// Frame capture. The staging buffer and the CPU copy are allocated on the
    /// first request and reused, so repeated captures cost one allocation.
    bool capture_wanted_ = false;
    VkBuffer capture_buffer_ = VK_NULL_HANDLE;
    void* capture_mapped_ = nullptr;
    u64 capture_capacity_ = 0;
    u8* capture_rgba_ = nullptr;
    u32 capture_w_ = 0;
    u32 capture_h_ = 0;
    void finish_capture();
    u32 cur_slot_ = 0;
    u64 cur_submit_ = 0;
    u32 cur_w_ = 0;
    u32 cur_h_ = 0;
};

}  // namespace gpu
