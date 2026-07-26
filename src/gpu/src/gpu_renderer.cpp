#include "gpu/gpu.h"
#include "gpu_barrier.h"
#include "gpu_swapchain.h"

#include "core/assert.h"

#include <volk.h>

namespace gpu {

using RendererResult = core::Result<Renderer, Error>;

RendererResult Renderer::create(const Device& device, VkSurfaceKHR surface, u32 width, u32 height) {
    VkBool32 present_ok = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(device.physical(), device.graphics_family(), surface,
                                         &present_ok);
    if (present_ok != VK_TRUE) {
        return RendererResult::err(Error::NoSuitableDevice);
    }

    Renderer r;
    r.device_ = &device;
    r.surface_ = surface;
    r.allocator_.init(device.physical(), device.handle());
    r.init_bindless();

    VkSemaphoreTypeCreateInfo timeline_type{};
    timeline_type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    timeline_type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo timeline_ci{};
    timeline_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    timeline_ci.pNext = &timeline_type;
    ASSERT_MSG(vkCreateSemaphore(device.handle(), &timeline_ci, nullptr, &r.timeline_) == VK_SUCCESS,
               "timeline semaphore");

    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        r.image_available_[i] = make_binary_semaphore(device.handle());

        VkCommandPoolCreateInfo pool_ci{};
        pool_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_ci.queueFamilyIndex = device.graphics_family();
        ASSERT_MSG(vkCreateCommandPool(device.handle(), &pool_ci, nullptr, &r.pools_[i]) ==
                       VK_SUCCESS,
                   "command pool");

        VkCommandBufferAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool = r.pools_[i];
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        ASSERT_MSG(vkAllocateCommandBuffers(device.handle(), &alloc, &r.cmds_[i]) == VK_SUCCESS,
                   "command buffers");
    }

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(device.physical(), &props);
    r.timestamp_period_ = props.limits.timestampPeriod;

    VkQueryPoolCreateInfo query_ci{};
    query_ci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    query_ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    query_ci.queryCount = FRAMES_IN_FLIGHT * 2;
    ASSERT_MSG(vkCreateQueryPool(device.handle(), &query_ci, nullptr, &r.timestamp_pool_) ==
                   VK_SUCCESS,
               "vkCreateQueryPool");

    r.recreate_swapchain(width, height);
    return RendererResult::ok(static_cast<Renderer&&>(r));
}

bool Renderer::recreate_swapchain(u32 width, u32 height) {
    const VkDevice dev = device_->handle();

    if (swapchain_ != VK_NULL_HANDLE) {
        // Resize is a rare event, not the steady frame path. Wait for the graphics
        // queue (rendering and presents both) to drain so the semaphores and views
        // being retired are no longer in use. This is a queue wait, not a
        // device-wide wait, and never runs per frame.
        vkQueueWaitIdle(device_->graphics_queue());
        destroy_image_objects();
    }

    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device_->physical(), surface_, &caps);
    const VkExtent2D extent = choose_extent(caps, width, height);
    if (extent.width == 0 || extent.height == 0) {
        return false;
    }

    const VkSurfaceFormatKHR format = choose_surface_format(device_->physical(), surface_);
    const VkPresentModeKHR present_mode = choose_present_mode(device_->physical(), surface_);

    u32 desired = caps.minImageCount + 1;
    if (caps.maxImageCount != 0 && desired > caps.maxImageCount) {
        desired = caps.maxImageCount;
    }
    if (desired > MAX_IMAGES) {
        desired = MAX_IMAGES;
    }

    VkSwapchainKHR old = swapchain_;
    VkSwapchainCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = surface_;
    ci.minImageCount = desired;
    ci.imageFormat = format.format;
    ci.imageColorSpace = format.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = present_mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old;
    ASSERT_MSG(vkCreateSwapchainKHR(dev, &ci, nullptr, &swapchain_) == VK_SUCCESS, "swapchain");
    if (old != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(dev, old, nullptr);
    }

    format_ = format.format;
    extent_ = extent;

    u32 count = 0;
    vkGetSwapchainImagesKHR(dev, swapchain_, &count, nullptr);
    if (count > MAX_IMAGES) {
        count = MAX_IMAGES;
    }
    vkGetSwapchainImagesKHR(dev, swapchain_, &count, images_);
    image_count_ = count;

    for (u32 i = 0; i < image_count_; ++i) {
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = images_[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = format_;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        ASSERT_MSG(vkCreateImageView(dev, &vci, nullptr, &views_[i]) == VK_SUCCESS, "swap view");
        render_finished_[i] = make_binary_semaphore(dev);
    }

    create_depth();
    return true;
}

void Renderer::destroy_image_objects() {
    const VkDevice dev = device_->handle();
    if (depth_view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(dev, depth_view_, nullptr);
        depth_view_ = VK_NULL_HANDLE;
    }
    if (depth_image_ != VK_NULL_HANDLE) {
        vkDestroyImage(dev, depth_image_, nullptr);
        depth_image_ = VK_NULL_HANDLE;
    }
    for (u32 i = 0; i < image_count_; ++i) {
        if (views_[i] != VK_NULL_HANDLE) {
            vkDestroyImageView(dev, views_[i], nullptr);
            views_[i] = VK_NULL_HANDLE;
        }
        if (render_finished_[i] != VK_NULL_HANDLE) {
            vkDestroySemaphore(dev, render_finished_[i], nullptr);
            render_finished_[i] = VK_NULL_HANDLE;
        }
    }
    image_count_ = 0;
}

Renderer::~Renderer() {
    if (device_ == nullptr) {
        return;
    }
    const VkDevice dev = device_->handle();
    vkDeviceWaitIdle(dev);  // shutdown, not the frame loop

    for (u32 i = 0; i < owned_count_; ++i) {
        vkDestroyBuffer(dev, owned_buffers_[i], nullptr);
    }
    for (u32 i = 0; i < owned_texture_count_; ++i) {
        vkDestroyImageView(dev, owned_views_[i], nullptr);
        vkDestroyImage(dev, owned_images_[i], nullptr);
    }
    if (default_sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(dev, default_sampler_, nullptr);
    }
    if (bindless_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(dev, bindless_pool_, nullptr);
    }
    if (bindless_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(dev, bindless_layout_, nullptr);
    }
    destroy_image_objects();
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(dev, swapchain_, nullptr);
    }
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        if (image_available_[i] != VK_NULL_HANDLE) {
            vkDestroySemaphore(dev, image_available_[i], nullptr);
        }
        if (pools_[i] != VK_NULL_HANDLE) {
            vkDestroyCommandPool(dev, pools_[i], nullptr);
        }
    }
    if (timeline_ != VK_NULL_HANDLE) {
        vkDestroySemaphore(dev, timeline_, nullptr);
    }
    if (timestamp_pool_ != VK_NULL_HANDLE) {
        vkDestroyQueryPool(dev, timestamp_pool_, nullptr);
    }
    allocator_.shutdown();
}

Renderer::Renderer(Renderer&& other) noexcept { *this = static_cast<Renderer&&>(other); }

Renderer& Renderer::operator=(Renderer&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    // Move construction only ever targets a fresh Renderer, so there is nothing
    // live to tear down here. Copy every member, then neutralise the source so
    // its destructor (guarded on device_) does nothing.
    device_ = other.device_;
    surface_ = other.surface_;
    allocator_ = other.allocator_;
    swapchain_ = other.swapchain_;
    format_ = other.format_;
    extent_ = other.extent_;
    image_count_ = other.image_count_;
    depth_image_ = other.depth_image_;
    depth_view_ = other.depth_view_;
    timeline_ = other.timeline_;
    frame_counter_ = other.frame_counter_;
    bindless_pool_ = other.bindless_pool_;
    bindless_layout_ = other.bindless_layout_;
    bindless_set_ = other.bindless_set_;
    default_sampler_ = other.default_sampler_;
    next_storage_index_ = other.next_storage_index_;
    next_sampled_index_ = other.next_sampled_index_;
    owned_texture_count_ = other.owned_texture_count_;
    for (u32 i = 0; i < MAX_OWNED_TEXTURES; ++i) {
        owned_images_[i] = other.owned_images_[i];
        owned_views_[i] = other.owned_views_[i];
    }
    timestamp_pool_ = other.timestamp_pool_;
    timestamp_period_ = other.timestamp_period_;
    last_gpu_ms_ = other.last_gpu_ms_;
    owned_count_ = other.owned_count_;
    cur_image_ = other.cur_image_;
    cur_slot_ = other.cur_slot_;
    cur_submit_ = other.cur_submit_;
    cur_w_ = other.cur_w_;
    cur_h_ = other.cur_h_;
    for (u32 i = 0; i < MAX_IMAGES; ++i) {
        images_[i] = other.images_[i];
        views_[i] = other.views_[i];
        render_finished_[i] = other.render_finished_[i];
    }
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        image_available_[i] = other.image_available_[i];
        pools_[i] = other.pools_[i];
        cmds_[i] = other.cmds_[i];
    }
    for (u32 i = 0; i < MAX_OWNED_BUFFERS; ++i) {
        owned_buffers_[i] = other.owned_buffers_[i];
    }
    other.device_ = nullptr;
    return *this;
}

}  // namespace gpu
