#pragma once

#include "core/result.h"
#include "core/span.h"
#include "core/types.h"
#include "sim/sim.h"

#include <vulkan/vulkan.h>

namespace platform {

/// A window and its input state. Owns its SDL subsystems for its lifetime.
/// # Invariants
/// One window per process in M0. The header exposes no SDL types.
class Window {
public:
    /// Open a Vulkan-capable, resizable window. When `capture_mouse` is true the
    /// cursor is grabbed for relative look, which suits interactive play but not
    /// a short automated run.
    /// # Errors
    /// Returns a static error string if SDL or window creation fails.
    [[nodiscard]] static core::Result<Window, const char*> open(const char* title, u32 width,
                                                                u32 height, bool capture_mouse);

    ~Window();
    Window(Window&& other) noexcept;
    Window& operator=(Window&& other) noexcept;
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    /// Drain the OS event queue, updating quit, drawable size, and the mouse
    /// motion accumulated since the last pump.
    void pump();

    /// True once the user has asked to close the window or pressed Escape.
    [[nodiscard]] bool quit_requested() const { return quit_; }

    /// Drawable width in pixels, kept current across resizes.
    [[nodiscard]] u32 width() const { return width_; }
    /// Drawable height in pixels, kept current across resizes.
    [[nodiscard]] u32 height() const { return height_; }
    /// True while the window has no drawable area and should not be rendered.
    [[nodiscard]] bool occluded() const { return width_ == 0 || height_ == 0; }

    /// Request a new window size. The drawable size updates on the next pump.
    void set_size(u32 width, u32 height);

    /// Build one tick of intent for `tick` from the live keyboard and mouse plus
    /// the motion accumulated since the last pump. Clears the motion accumulator.
    [[nodiscard]] sim::InputCmd capture_input(sim::TickId tick);

    /// The Vulkan instance extensions this window needs. Points into SDL-owned
    /// storage that lives for the duration of the process.
    [[nodiscard]] core::Span<const char* const> vulkan_instance_extensions() const;

    /// Create a Vulkan surface for this window on `instance`.
    /// # Errors
    /// Returns a static error string if surface creation fails.
    [[nodiscard]] core::Result<VkSurfaceKHR, const char*> create_surface(VkInstance instance) const;

private:
    Window() = default;

    void* window_ = nullptr;  // SDL_Window*
    bool quit_ = false;
    u32 width_ = 0;
    u32 height_ = 0;
    f32 mouse_dx_ = 0.0f;
    f32 mouse_dy_ = 0.0f;
};

}  // namespace platform
