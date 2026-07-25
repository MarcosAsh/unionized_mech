#include "gpu/gpu.h"
#include "gpu/gpu_swapchain.h"

#include "core/assert.h"

#include <volk.h>

namespace gpu {

using RendererResult = core::Result<Renderer, Error>;

namespace {

VkSemaphore make_binary_semaphore(VkDevice device) {
    VkSemaphoreCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore s = VK_NULL_HANDLE;
    ASSERT_MSG(vkCreateSemaphore(device, &ci, nullptr, &s) == VK_SUCCESS, "vkCreateSemaphore");
    return s;
}

VkImageMemoryBarrier2 image_barrier(VkImage image, VkImageAspectFlags aspect,
                                    VkImageLayout old_layout, VkImageLayout new_layout,
                                    VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                                    VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = old_layout;
    b.newLayout = new_layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    return b;
}

void submit_barrier(VkCommandBuffer cmd, const VkImageMemoryBarrier2& barrier) {
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

}  // namespace

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

Frame Renderer::begin_frame(u32 width, u32 height) {
    Frame frame{};
    if (width == 0 || height == 0) {
        return frame;
    }
    if (swapchain_ == VK_NULL_HANDLE || width != extent_.width || height != extent_.height) {
        if (!recreate_swapchain(width, height)) {
            return frame;
        }
    }

    const VkDevice dev = device_->handle();
    const u64 submit_value = frame_counter_ + 1;
    if (submit_value > FRAMES_IN_FLIGHT) {
        const u64 wait_value = submit_value - FRAMES_IN_FLIGHT;
        VkSemaphoreWaitInfo wait{};
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wait.semaphoreCount = 1;
        wait.pSemaphores = &timeline_;
        wait.pValues = &wait_value;
        vkWaitSemaphores(dev, &wait, UINT64_MAX);
    }

    const u32 slot = static_cast<u32>(frame_counter_ % FRAMES_IN_FLIGHT);

    // Read this slot's timestamps from its previous use, now that the pacing wait
    // guarantees that frame finished. Two timestamps bracket the frame's GPU work.
    if (frame_counter_ >= FRAMES_IN_FLIGHT) {
        u64 ts[2] = {0, 0};
        if (vkGetQueryPoolResults(dev, timestamp_pool_, slot * 2, 2, sizeof(ts), ts, sizeof(u64),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            const f64 ns = static_cast<f64>(ts[1] - ts[0]) * static_cast<f64>(timestamp_period_);
            last_gpu_ms_ = static_cast<f32>(ns * 1e-6);
        }
    }

    u32 image_index = 0;
    const VkResult acquired = vkAcquireNextImageKHR(dev, swapchain_, UINT64_MAX,
                                                    image_available_[slot], VK_NULL_HANDLE,
                                                    &image_index);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain(width, height);
        return frame;
    }
    ASSERT_MSG(acquired == VK_SUCCESS || acquired == VK_SUBOPTIMAL_KHR, "vkAcquireNextImageKHR");

    const VkCommandBuffer cmd = cmds_[slot];
    ASSERT_MSG(vkResetCommandPool(dev, pools_[slot], 0) == VK_SUCCESS, "vkResetCommandPool");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ASSERT_MSG(vkBeginCommandBuffer(cmd, &begin) == VK_SUCCESS, "vkBeginCommandBuffer");

    vkCmdResetQueryPool(cmd, timestamp_pool_, slot * 2, 2);
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, timestamp_pool_, slot * 2);

    submit_barrier(cmd, image_barrier(images_[image_index], VK_IMAGE_ASPECT_COLOR_BIT,
                                      VK_IMAGE_LAYOUT_UNDEFINED,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                      VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT));
    submit_barrier(cmd, image_barrier(depth_image_, VK_IMAGE_ASPECT_DEPTH_BIT,
                                      VK_IMAGE_LAYOUT_UNDEFINED,
                                      VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                                      VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT, 0,
                                      VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                          VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT));

    cur_image_ = image_index;
    cur_slot_ = slot;
    cur_submit_ = submit_value;
    cur_w_ = width;
    cur_h_ = height;

    frame.cmd = cmd;
    frame.color_view = views_[image_index];
    frame.depth_view = depth_view_;
    frame.extent = extent_;
    frame.valid = true;
    return frame;
}

void Renderer::end_frame() {
    const VkCommandBuffer cmd = cmds_[cur_slot_];

    submit_barrier(cmd, image_barrier(images_[cur_image_], VK_IMAGE_ASPECT_COLOR_BIT,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                      VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                      VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0));

    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestamp_pool_,
                         cur_slot_ * 2 + 1);
    ASSERT_MSG(vkEndCommandBuffer(cmd) == VK_SUCCESS, "vkEndCommandBuffer");

    VkCommandBufferSubmitInfo cmd_info{};
    cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmd_info.commandBuffer = cmd;

    VkSemaphoreSubmitInfo wait_info{};
    wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    wait_info.semaphore = image_available_[cur_slot_];
    wait_info.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSemaphoreSubmitInfo signal_info[2]{};
    signal_info[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signal_info[0].semaphore = render_finished_[cur_image_];
    signal_info[0].stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    signal_info[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signal_info[1].semaphore = timeline_;
    signal_info[1].value = cur_submit_;
    signal_info[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &wait_info;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmd_info;
    submit.signalSemaphoreInfoCount = 2;
    submit.pSignalSemaphoreInfos = signal_info;
    ASSERT_MSG(vkQueueSubmit2(device_->graphics_queue(), 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS,
               "vkQueueSubmit2");

    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &render_finished_[cur_image_];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &cur_image_;
    const VkResult presented = vkQueuePresentKHR(device_->graphics_queue(), &present);

    frame_counter_ = cur_submit_;

    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
        recreate_swapchain(cur_w_, cur_h_);
        return;
    }
    ASSERT_MSG(presented == VK_SUCCESS, "vkQueuePresentKHR");
}

void Renderer::render_clear(f32 r, f32 g, f32 b, u32 width, u32 height) {
    const Frame frame = begin_frame(width, height);
    if (!frame.valid) {
        return;
    }
    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = frame.color_view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{r, g, b, 1.0f}};

    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea.extent = frame.extent;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vkCmdBeginRendering(frame.cmd, &rendering);
    vkCmdEndRendering(frame.cmd);
    end_frame();
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
    next_storage_index_ = other.next_storage_index_;
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
