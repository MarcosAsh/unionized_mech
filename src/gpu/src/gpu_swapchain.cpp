#include "gpu_swapchain.h"

#include <volk.h>

namespace gpu {

namespace {

constexpr u32 MAX_FORMATS = 64;
constexpr u32 MAX_MODES = 8;

}  // namespace

VkSurfaceFormatKHR choose_surface_format(VkPhysicalDevice pd, VkSurfaceKHR surface) {
    u32 count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &count, nullptr);
    if (count > MAX_FORMATS) {
        count = MAX_FORMATS;
    }
    VkSurfaceFormatKHR formats[MAX_FORMATS];
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &count, formats);

    for (u32 i = 0; i < count; ++i) {
        const bool srgb = formats[i].format == VK_FORMAT_B8G8R8A8_SRGB ||
                          formats[i].format == VK_FORMAT_R8G8B8A8_SRGB;
        if (srgb && formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return formats[i];
        }
    }
    return formats[0];
}

VkPresentModeKHR choose_present_mode(VkPhysicalDevice pd, VkSurfaceKHR surface) {
    u32 count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surface, &count, nullptr);
    if (count > MAX_MODES) {
        count = MAX_MODES;
    }
    VkPresentModeKHR modes[MAX_MODES];
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surface, &count, modes);

    // FIFO is guaranteed by the spec. M0 keeps it simple and tear-free.
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D choose_extent(const VkSurfaceCapabilitiesKHR& caps, u32 width, u32 height) {
    if (caps.currentExtent.width != 0xFFFFFFFFu) {
        return caps.currentExtent;
    }
    VkExtent2D e = {width, height};
    if (e.width < caps.minImageExtent.width) {
        e.width = caps.minImageExtent.width;
    }
    if (e.width > caps.maxImageExtent.width) {
        e.width = caps.maxImageExtent.width;
    }
    if (e.height < caps.minImageExtent.height) {
        e.height = caps.minImageExtent.height;
    }
    if (e.height > caps.maxImageExtent.height) {
        e.height = caps.maxImageExtent.height;
    }
    return e;
}

}  // namespace gpu
