#include "core/arena.h"
#include "core/log.h"
#include "core/timer.h"
#include "core/types.h"
#include "gpu/gpu.h"
#include "platform/platform.h"
#include "sim/sim.h"

#include <cmath>

namespace {

constexpr bool ENABLE_VALIDATION =
#ifdef NDEBUG
    false;
#else
    true;
#endif

// Optional first argument: a frame cap for a short automated run. Zero means run
// interactively until the user quits.
u64 parse_frame_cap(int argc, char** argv) {
    if (argc < 2) {
        return 0;
    }
    u64 value = 0;
    for (const char* p = argv[1]; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') {
            return 0;
        }
        value = value * 10u + static_cast<u64>(*p - '0');
    }
    return value;
}

}  // namespace

// M0 frame loop. Fixed 60Hz simulation, and a swapchain clear whose colour is
// driven by the simulation's spin angle so the window visibly animates.
int main(int argc, char** argv) {
    const u64 frame_cap = parse_frame_cap(argc, argv);
    const bool interactive = frame_cap == 0;

    core::Arena permanent = core::Arena::with_capacity(64ull << 20);
    core::Arena frame = core::Arena::with_capacity(16ull << 20);
    core::Arena scratch = core::Arena::with_capacity(16ull << 20);

    core::Result<platform::Window, const char*> win_result =
        platform::Window::open("unionized_mech", 1280, 720, interactive);
    if (win_result.is_err()) {
        core::log_errorf("failed to open window: %s", win_result.error());
        return 1;
    }
    platform::Window win = static_cast<platform::Window&&>(win_result.value());

    core::Result<gpu::Instance, gpu::Error> inst_result =
        gpu::Instance::create(win.vulkan_instance_extensions(), ENABLE_VALIDATION);
    if (inst_result.is_err()) {
        core::log_errorf("vulkan instance: %s", gpu::to_string(inst_result.error()));
        return 1;
    }
    gpu::Instance instance = static_cast<gpu::Instance&&>(inst_result.value());

    core::Result<VkSurfaceKHR, const char*> surf_result = win.create_surface(instance.handle());
    if (surf_result.is_err()) {
        core::log_errorf("vulkan surface: %s", surf_result.error());
        return 1;
    }
    gpu::Surface surface = gpu::Surface::adopt(instance, surf_result.value());

    core::Result<gpu::Device, gpu::Error> dev_result = gpu::Device::create(instance);
    if (dev_result.is_err()) {
        core::log_errorf("vulkan device: %s", gpu::to_string(dev_result.error()));
        return 1;
    }
    gpu::Device device = static_cast<gpu::Device&&>(dev_result.value());

    core::Result<gpu::Renderer, gpu::Error> rend_result =
        gpu::Renderer::create(device, surface.handle(), win.width(), win.height());
    if (rend_result.is_err()) {
        core::log_errorf("vulkan renderer: %s", gpu::to_string(rend_result.error()));
        return 1;
    }
    gpu::Renderer renderer = static_cast<gpu::Renderer&&>(rend_result.value());
    core::log_infof("window %ux%u. WASD moves, mouse looks, Escape quits.", win.width(),
                    win.height());

    constexpr i32 MAX_TICKS_PER_FRAME = 8;

    sim::World world{};
    f64 accumulator = 0.0;
    const u64 start_ns = core::Timer::now_ns();
    u64 prev_ns = start_ns;
    u64 report_ns = start_ns;
    u64 total_ticks = 0;
    u64 ticks_at_report = 0;
    u64 frames = 0;
    u64 frames_at_report = 0;

    while (!win.quit_requested()) {
        win.pump();

        const u64 now_ns = core::Timer::now_ns();
        accumulator += static_cast<f64>(now_ns - prev_ns) * 1e-9;
        prev_ns = now_ns;

        i32 steps = 0;
        while (accumulator >= static_cast<f64>(sim::SIM_DT) && steps < MAX_TICKS_PER_FRAME) {
            const sim::InputCmd cmd = win.capture_input(world.tick);
            sim::World next{};
            sim::simulate(world, cmd, next);
            world = next;
            accumulator -= static_cast<f64>(sim::SIM_DT);
            ++steps;
            ++total_ticks;
        }

        const f32 a = world.spin_angle;
        renderer.render_clear(0.5f + 0.5f * sinf(a), 0.5f + 0.5f * sinf(a + 2.094f),
                              0.5f + 0.5f * sinf(a + 4.188f), win.width(), win.height());

        frame.reset();
        ++frames;

        if (now_ns - report_ns >= 1000000000ull) {
            const f64 dt = static_cast<f64>(now_ns - report_ns) * 1e-9;
            core::log_infof(
                "t=%.0fs  ticks/s=%.0f  fps=%.0f  yaw=%.2f pitch=%.2f pos=(%.1f,%.1f,%.1f)",
                static_cast<f64>(now_ns - start_ns) * 1e-9,
                static_cast<f64>(total_ticks - ticks_at_report) / dt,
                static_cast<f64>(frames - frames_at_report) / dt,
                static_cast<f64>(world.cam_yaw), static_cast<f64>(world.cam_pitch),
                static_cast<f64>(world.cam_x), static_cast<f64>(world.cam_y),
                static_cast<f64>(world.cam_z));
            report_ns = now_ns;
            ticks_at_report = total_ticks;
            frames_at_report = frames;
        }

        if (frame_cap != 0 && frames >= frame_cap) {
            break;
        }
    }

    core::log_infof("done. %llu frames, %llu ticks, hash=%016llx",
                    static_cast<unsigned long long>(frames),
                    static_cast<unsigned long long>(total_ticks),
                    static_cast<unsigned long long>(sim::hash(world)));

    (void)permanent;
    (void)scratch;
    return 0;
}
