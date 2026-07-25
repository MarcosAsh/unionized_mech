#include "platform/platform.h"

#include <SDL3/SDL.h>

namespace platform {

using WindowResult = core::Result<Window, const char*>;

namespace {

SDL_Window* as_window(void* p) { return static_cast<SDL_Window*>(p); }

i16 clamp_i16(f32 v) {
    if (v > 32767.0f) {
        v = 32767.0f;
    }
    if (v < -32768.0f) {
        v = -32768.0f;
    }
    return static_cast<i16>(v);
}

i8 axis(bool positive, bool negative) {
    return static_cast<i8>((positive ? 1 : 0) - (negative ? 1 : 0));
}

}  // namespace

WindowResult Window::open(const char* title, u32 width, u32 height) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        return WindowResult::err("SDL_Init failed");
    }

    SDL_Window* w = SDL_CreateWindow(title, static_cast<int>(width), static_cast<int>(height),
                                     SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (w == nullptr) {
        SDL_Quit();
        return WindowResult::err("SDL_CreateWindow failed");
    }
    SDL_SetWindowRelativeMouseMode(w, true);

    int pw = 0;
    int ph = 0;
    SDL_GetWindowSizeInPixels(w, &pw, &ph);

    Window win;
    win.window_ = w;
    win.width_ = static_cast<u32>(pw);
    win.height_ = static_cast<u32>(ph);
    return WindowResult::ok(static_cast<Window&&>(win));
}

Window::~Window() {
    if (window_ != nullptr) {
        SDL_DestroyWindow(as_window(window_));
        SDL_Quit();
    }
}

Window::Window(Window&& other) noexcept
    : window_(other.window_),
      quit_(other.quit_),
      width_(other.width_),
      height_(other.height_),
      mouse_dx_(other.mouse_dx_),
      mouse_dy_(other.mouse_dy_) {
    other.window_ = nullptr;
}

Window& Window::operator=(Window&& other) noexcept {
    if (this != &other) {
        if (window_ != nullptr) {
            SDL_DestroyWindow(as_window(window_));
            SDL_Quit();
        }
        window_ = other.window_;
        quit_ = other.quit_;
        width_ = other.width_;
        height_ = other.height_;
        mouse_dx_ = other.mouse_dx_;
        mouse_dy_ = other.mouse_dy_;
        other.window_ = nullptr;
    }
    return *this;
}

void Window::pump() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_EVENT_QUIT:
                quit_ = true;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.scancode == SDL_SCANCODE_ESCAPE) {
                    quit_ = true;
                }
                break;
            case SDL_EVENT_MOUSE_MOTION:
                mouse_dx_ += e.motion.xrel;
                mouse_dy_ += e.motion.yrel;
                break;
            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
                int pw = 0;
                int ph = 0;
                SDL_GetWindowSizeInPixels(as_window(window_), &pw, &ph);
                width_ = static_cast<u32>(pw);
                height_ = static_cast<u32>(ph);
                break;
            }
            default:
                break;
        }
    }
}

sim::InputCmd Window::capture_input(sim::TickId tick) {
    sim::InputCmd cmd;
    cmd.tick = tick;

    const bool* keys = SDL_GetKeyboardState(nullptr);
    cmd.move_y = axis(keys[SDL_SCANCODE_W], keys[SDL_SCANCODE_S]);
    cmd.move_x = axis(keys[SDL_SCANCODE_D], keys[SDL_SCANCODE_A]);

    cmd.look_dx = clamp_i16(mouse_dx_);
    cmd.look_dy = clamp_i16(mouse_dy_);
    mouse_dx_ = 0.0f;
    mouse_dy_ = 0.0f;

    u16 buttons = 0;
    if (keys[SDL_SCANCODE_SPACE]) {
        sim::set_button(buttons, sim::Button::Jump, true);
    }
    if (keys[SDL_SCANCODE_LCTRL]) {
        sim::set_button(buttons, sim::Button::Crouch, true);
    }
    const SDL_MouseButtonFlags mb = SDL_GetMouseState(nullptr, nullptr);
    if ((mb & SDL_BUTTON_LMASK) != 0) {
        sim::set_button(buttons, sim::Button::Fire, true);
    }
    if ((mb & SDL_BUTTON_RMASK) != 0) {
        sim::set_button(buttons, sim::Button::Aim, true);
    }
    cmd.buttons = buttons;

    return cmd;
}

}  // namespace platform
