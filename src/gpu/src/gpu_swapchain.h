#pragma once

// Internal to the gpu module. Not included outside it.

#include "core/types.h"

#include <vulkan/vulkan.h>

namespace gpu {

/// Pick an sRGB surface format when available, else the first reported one.
[[nodiscard]] VkSurfaceFormatKHR choose_surface_format(VkPhysicalDevice pd, VkSurfaceKHR surface);

/// Pick a present mode. FIFO is always supported and is used for M0.
[[nodiscard]] VkPresentModeKHR choose_present_mode(VkPhysicalDevice pd, VkSurfaceKHR surface);

/// Clamp the requested size to the surface limits, honouring a fixed current
/// extent when the platform reports one.
[[nodiscard]] VkExtent2D choose_extent(const VkSurfaceCapabilitiesKHR& caps, u32 width, u32 height);

}  // namespace gpu
