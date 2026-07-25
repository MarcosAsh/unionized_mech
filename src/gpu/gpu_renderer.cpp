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

VkImageMemoryBarrier2 color_barrier(VkImage image, VkImageLayout old_layout,
                                    VkImageLayout new_layout, VkPipelineStageFlags2 src_stage,
                                    VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                                    VkAccessFlags2 dst_access) {
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
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
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

    VkSemaphoreTypeCreateInfo timeline_type{};
    timeline_type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    timeline_type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timeline_type.initialValue = 0;
    VkSemaphoreCreateInfo timeline_ci{};
    timeline_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    timeline_ci.pNext = &timeline_type;
    ASSERT_MSG(vkCreateSemaphore(device.handle(), &timeline_ci, nullptr, &r.timeline_) == VK_SUCCESS,
               "vkCreateSemaphore timeline");

    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        r.image_available_[i] = make_binary_semaphore(device.handle());

        VkCommandPoolCreateInfo pool_ci{};
        pool_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_ci.queueFamilyIndex = device.graphics_family();
        ASSERT_MSG(vkCreateCommandPool(device.handle(), &pool_ci, nullptr, &r.pools_[i]) ==
                       VK_SUCCESS,
                   "vkCreateCommandPool");

        VkCommandBufferAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool = r.pools_[i];
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        ASSERT_MSG(vkAllocateCommandBuffers(device.handle(), &alloc, &r.cmds_[i]) == VK_SUCCESS,
                   "vkAllocateCommandBuffers");
    }

    if (!r.recreate_swapchain(width, height)) {
        // Zero-sized at startup is unusual but not fatal; the first resize fixes it.
    }
    return RendererResult::ok(static_cast<Renderer&&>(r));
}

bool Renderer::recreate_swapchain(u32 width, u32 height) {
    const VkDevice dev = device_->handle();

    if (swapchain_ != VK_NULL_HANDLE) {
        // Wait for all rendering we submitted to finish before retiring images.
        const u64 target = frame_counter_;
        VkSemaphoreWaitInfo wait{};
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wait.semaphoreCount = 1;
        wait.pSemaphores = &timeline_;
        wait.pValues = &target;
        vkWaitSemaphores(dev, &wait, UINT64_MAX);
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

    VkSwapchainKHR created = VK_NULL_HANDLE;
    ASSERT_MSG(vkCreateSwapchainKHR(dev, &ci, nullptr, &created) == VK_SUCCESS,
               "vkCreateSwapchainKHR");
    swapchain_ = created;
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
        ASSERT_MSG(vkCreateImageView(dev, &vci, nullptr, &views_[i]) == VK_SUCCESS,
                   "vkCreateImageView");
        render_finished_[i] = make_binary_semaphore(dev);
    }
    return true;
}

void Renderer::destroy_image_objects() {
    const VkDevice dev = device_->handle();
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

void Renderer::render_clear(f32 r, f32 g, f32 b, u32 width, u32 height) {
    if (width == 0 || height == 0) {
        return;
    }
    if (swapchain_ == VK_NULL_HANDLE || width != extent_.width || height != extent_.height) {
        if (!recreate_swapchain(width, height)) {
            return;
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

    u32 image_index = 0;
    const VkResult acquired = vkAcquireNextImageKHR(dev, swapchain_, UINT64_MAX,
                                                    image_available_[slot], VK_NULL_HANDLE,
                                                    &image_index);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain(width, height);
        return;
    }
    ASSERT_MSG(acquired == VK_SUCCESS || acquired == VK_SUBOPTIMAL_KHR, "vkAcquireNextImageKHR");

    ASSERT_MSG(vkResetCommandPool(dev, pools_[slot], 0) == VK_SUCCESS, "vkResetCommandPool");
    const VkCommandBuffer cmd = cmds_[slot];
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ASSERT_MSG(vkBeginCommandBuffer(cmd, &begin) == VK_SUCCESS, "vkBeginCommandBuffer");

    submit_barrier(cmd, color_barrier(images_[image_index], VK_IMAGE_LAYOUT_UNDEFINED,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                      VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT));

    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = views_[image_index];
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{r, g, b, 1.0f}};

    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea.extent = extent_;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &rendering);
    vkCmdEndRendering(cmd);

    submit_barrier(cmd, color_barrier(images_[image_index],
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                      VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                      VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0));

    ASSERT_MSG(vkEndCommandBuffer(cmd) == VK_SUCCESS, "vkEndCommandBuffer");

    VkCommandBufferSubmitInfo cmd_info{};
    cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmd_info.commandBuffer = cmd;

    VkSemaphoreSubmitInfo wait_info{};
    wait_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    wait_info.semaphore = image_available_[slot];
    wait_info.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSemaphoreSubmitInfo signal_info[2]{};
    signal_info[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signal_info[0].semaphore = render_finished_[image_index];
    signal_info[0].stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    signal_info[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signal_info[1].semaphore = timeline_;
    signal_info[1].value = submit_value;
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
    present.pWaitSemaphores = &render_finished_[image_index];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &image_index;
    const VkResult presented = vkQueuePresentKHR(device_->graphics_queue(), &present);

    frame_counter_ = submit_value;

    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
        recreate_swapchain(width, height);
        return;
    }
    ASSERT_MSG(presented == VK_SUCCESS, "vkQueuePresentKHR");
}

Renderer::~Renderer() {
    if (device_ == nullptr) {
        return;
    }
    const VkDevice dev = device_->handle();
    vkDeviceWaitIdle(dev);  // shutdown, not the frame loop

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
}

Renderer::Renderer(Renderer&& other) noexcept {
    *this = static_cast<Renderer&&>(other);
}

Renderer& Renderer::operator=(Renderer&& other) noexcept {
    if (this != &other) {
        device_ = other.device_;
        surface_ = other.surface_;
        swapchain_ = other.swapchain_;
        format_ = other.format_;
        extent_ = other.extent_;
        image_count_ = other.image_count_;
        timeline_ = other.timeline_;
        frame_counter_ = other.frame_counter_;
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
        other.device_ = nullptr;
        other.swapchain_ = VK_NULL_HANDLE;
        other.timeline_ = VK_NULL_HANDLE;
        other.image_count_ = 0;
        for (u32 i = 0; i < MAX_IMAGES; ++i) {
            other.views_[i] = VK_NULL_HANDLE;
            other.render_finished_[i] = VK_NULL_HANDLE;
        }
        for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
            other.image_available_[i] = VK_NULL_HANDLE;
            other.pools_[i] = VK_NULL_HANDLE;
        }
    }
    return *this;
}

}  // namespace gpu
