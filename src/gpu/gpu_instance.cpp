#include "gpu/gpu.h"

#include "core/assert.h"
#include "core/log.h"

#include <volk.h>

#include <cstdlib>

namespace gpu {

using InstanceResult = core::Result<Instance, Error>;

const char* to_string(Error e) {
    switch (e) {
        case Error::VolkInit:
            return "volk failed to initialize (Vulkan loader missing)";
        case Error::InstanceCreate:
            return "vkCreateInstance failed";
        case Error::NoSuitableDevice:
            return "no Vulkan 1.3 device with the required features";
        case Error::DeviceCreate:
            return "vkCreateDevice failed";
    }
    return "unknown";
}

namespace {

// Any warning or error is fatal. This is how "zero validation messages" is
// enforced: a message aborts the process, so a run or a CI step fails loudly.
VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data,
                                              void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        core::log_errorf("[vulkan] %s", data->pMessage);
        std::abort();
    }
    core::log_infof("[vulkan] %s", data->pMessage);
    return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT messenger_info() {
    VkDebugUtilsMessengerCreateInfoEXT ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    ci.pfnUserCallback = debug_callback;
    return ci;
}

}  // namespace

InstanceResult Instance::create(core::Span<const char* const> window_extensions,
                                bool enable_validation) {
    if (volkInitialize() != VK_SUCCESS) {
        return InstanceResult::err(Error::VolkInit);
    }

    const char* extensions[16];
    u32 ext_count = 0;
    for (u64 i = 0; i < window_extensions.size(); ++i) {
        ASSERT(ext_count < 16);
        extensions[ext_count++] = window_extensions[i];
    }
    if (enable_validation) {
        ASSERT(ext_count < 16);
        extensions[ext_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    }

    const char* layers[1] = {"VK_LAYER_KHRONOS_validation"};

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "unionized_mech";
    app.apiVersion = VK_API_VERSION_1_3;

    VkDebugUtilsMessengerCreateInfoEXT dbg = messenger_info();

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = ext_count;
    ci.ppEnabledExtensionNames = extensions;
    if (enable_validation) {
        ci.enabledLayerCount = 1;
        ci.ppEnabledLayerNames = layers;
        ci.pNext = &dbg;  // catches messages emitted during create and destroy
    }

    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ci, nullptr, &instance) != VK_SUCCESS) {
        return InstanceResult::err(Error::InstanceCreate);
    }
    volkLoadInstance(instance);

    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    if (enable_validation) {
        ASSERT_MSG(vkCreateDebugUtilsMessengerEXT(instance, &dbg, nullptr, &messenger) == VK_SUCCESS,
                   "vkCreateDebugUtilsMessengerEXT");
    }

    Instance out;
    out.instance_ = instance;
    out.messenger_ = messenger;
    return InstanceResult::ok(static_cast<Instance&&>(out));
}

Instance::~Instance() {
    if (instance_ != VK_NULL_HANDLE) {
        if (messenger_ != VK_NULL_HANDLE) {
            vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
        }
        vkDestroyInstance(instance_, nullptr);
    }
}

Instance::Instance(Instance&& other) noexcept
    : instance_(other.instance_), messenger_(other.messenger_) {
    other.instance_ = VK_NULL_HANDLE;
    other.messenger_ = VK_NULL_HANDLE;
}

Instance& Instance::operator=(Instance&& other) noexcept {
    if (this != &other) {
        if (instance_ != VK_NULL_HANDLE) {
            if (messenger_ != VK_NULL_HANDLE) {
                vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
            }
            vkDestroyInstance(instance_, nullptr);
        }
        instance_ = other.instance_;
        messenger_ = other.messenger_;
        other.instance_ = VK_NULL_HANDLE;
        other.messenger_ = VK_NULL_HANDLE;
    }
    return *this;
}

Surface Surface::adopt(const Instance& instance, VkSurfaceKHR surface) {
    Surface out;
    out.instance_ = instance.handle();
    out.surface_ = surface;
    return out;
}

Surface::~Surface() {
    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
    }
}

Surface::Surface(Surface&& other) noexcept : instance_(other.instance_), surface_(other.surface_) {
    other.surface_ = VK_NULL_HANDLE;
}

Surface& Surface::operator=(Surface&& other) noexcept {
    if (this != &other) {
        if (surface_ != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
        }
        instance_ = other.instance_;
        surface_ = other.surface_;
        other.surface_ = VK_NULL_HANDLE;
    }
    return *this;
}

}  // namespace gpu
