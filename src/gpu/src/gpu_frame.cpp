#include "gpu/gpu.h"
#include "gpu_barrier.h"
#include "gpu_swapchain.h"

#include "core/assert.h"

#include <volk.h>

namespace gpu {

// The per-frame path: acquire, record, submit, present. Creation, swapchain
// lifecycle, and teardown live in gpu_renderer.cpp.

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

}  // namespace gpu
