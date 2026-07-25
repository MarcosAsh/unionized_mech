// Stands up a Vulkan instance and logical device on the real GPU, with
// validation active, then exits. No window, so it runs headless in CI and here.
// If validation emits anything, the fatal messenger aborts and this exits nonzero.

#include "core/log.h"
#include "gpu/gpu.h"

int main() {
    // The device enables VK_KHR_swapchain, which requires the VK_KHR_surface
    // instance extension. The real app gets this from the window; here it is
    // named directly since the probe has no window.
    const char* const instance_extensions[1] = {VK_KHR_SURFACE_EXTENSION_NAME};
    core::Result<gpu::Instance, gpu::Error> inst =
        gpu::Instance::create(core::Span<const char* const>(instance_extensions, 1), true);
    if (inst.is_err()) {
        core::log_errorf("instance: %s", gpu::to_string(inst.error()));
        return 1;
    }
    gpu::Instance instance = static_cast<gpu::Instance&&>(inst.value());

    core::Result<gpu::Device, gpu::Error> dev = gpu::Device::create(instance);
    if (dev.is_err()) {
        core::log_errorf("device: %s", gpu::to_string(dev.error()));
        return 1;
    }
    gpu::Device device = static_cast<gpu::Device&&>(dev.value());
    (void)device;

    core::log_info("gpu_probe: instance and device OK, no validation messages");
    return 0;
}
