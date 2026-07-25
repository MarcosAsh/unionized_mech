#include "core/arena.h"
#include "core/assert.h"
#include "core/log.h"
#include "core/timer.h"
#include "core/types.h"
#include "gpu/gpu.h"
#include "platform/platform.h"
#include "render/render.h"
#include "sim/sim.h"

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

// M0 frame loop. Fixed 60Hz simulation double-buffered into two snapshots, with
// the render interpolating between them by the leftover-time alpha.
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

    render::Scene scene = render::Scene::create(renderer, scratch);
    scratch.reset();
    core::log_infof("window %ux%u. WASD moves, mouse looks, Escape quits.", win.width(),
                    win.height());

    constexpr u32 MAX_TICKS_PER_FRAME = 8;

    sim::World prev_world{};
    sim::World curr_world{};
    sim::FixedTimestep timestep;

    const u64 permanent_baseline = permanent.used();
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
        const f64 elapsed = static_cast<f64>(now_ns - prev_ns) * 1e-9;
        prev_ns = now_ns;

        const u32 ticks = timestep.advance(elapsed, MAX_TICKS_PER_FRAME);
        for (u32 i = 0; i < ticks; ++i) {
            prev_world = curr_world;
            const sim::InputCmd cmd = win.capture_input(curr_world.tick);
            sim::World next{};
            sim::simulate(curr_world, cmd, next);
            curr_world = next;
            ++total_ticks;
        }

        const gpu::Frame gpu_frame = renderer.begin_frame(win.width(), win.height());
        if (gpu_frame.valid) {
            scene.draw(gpu_frame, prev_world, curr_world, timestep.alpha());
            renderer.end_frame();
        }

        frame.reset();
        ++frames;

        // Acceptance check 4: nothing allocates from the permanent arena in the loop.
        ASSERT(permanent.used() == permanent_baseline);

        // In a capped run, resize twice to exercise the swapchain recreate path
        // under validation (acceptance check 2).
        if (frame_cap != 0) {
            if (frames == frame_cap / 3) {
                core::log_info("resize to 800x600");
                win.set_size(800, 600);
            } else if (frames == 2 * frame_cap / 3) {
                core::log_info("resize to 1600x900");
                win.set_size(1600, 900);
            }
        }

        if (now_ns - report_ns >= 1000000000ull) {
            const f64 dt = static_cast<f64>(now_ns - report_ns) * 1e-9;
            const f64 cpu_ms = dt * 1000.0 / static_cast<f64>(frames - frames_at_report);
            const f64 hspeed =
                static_cast<f64>(curr_world.vel_x) * static_cast<f64>(curr_world.vel_x) +
                static_cast<f64>(curr_world.vel_z) * static_cast<f64>(curr_world.vel_z);
            const char* state_names[4] = {"ground", "slide", "air", "wallrun"};
            core::log_infof(
                "t=%.0fs  fps=%.0f  cpu=%.2fms  gpu=%.2fms  ticks/s=%.0f  speed=%.1f  %s",
                static_cast<f64>(now_ns - start_ns) * 1e-9,
                static_cast<f64>(frames - frames_at_report) / dt, cpu_ms,
                static_cast<f64>(renderer.last_gpu_ms()),
                static_cast<f64>(total_ticks - ticks_at_report) / dt, __builtin_sqrt(hspeed),
                state_names[static_cast<u8>(curr_world.state) & 3]);
            report_ns = now_ns;
            ticks_at_report = total_ticks;
            frames_at_report = frames;
            scene.maybe_reload();
        }

        if (frame_cap != 0 && frames >= frame_cap) {
            break;
        }
    }

    core::log_infof("done. %llu frames, %llu ticks, hash=%016llx",
                    static_cast<unsigned long long>(frames),
                    static_cast<unsigned long long>(total_ticks),
                    static_cast<unsigned long long>(sim::hash(curr_world)));
    return 0;
}
