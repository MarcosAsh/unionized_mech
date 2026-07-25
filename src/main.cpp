#include "core/arena.h"
#include "core/log.h"
#include "core/timer.h"
#include "core/types.h"
#include "platform/platform.h"
#include "sim/sim.h"

using WindowResult = core::Result<platform::Window, const char*>;

// M0 frame loop. Fixed 60Hz accumulator with a keep-the-debt catch-up cap.
// Rendering bolts on at commit 7, so the window stays blank for now and the
// proof of life is in the logs and in um_headless.
int main() {
    core::Arena permanent = core::Arena::with_capacity(64ull << 20);
    core::Arena frame = core::Arena::with_capacity(16ull << 20);
    core::Arena scratch = core::Arena::with_capacity(16ull << 20);

    WindowResult win_result = platform::Window::open("unionized_mech", 1280, 720);
    if (win_result.is_err()) {
        core::log_errorf("failed to open window: %s", win_result.error());
        return 1;
    }
    platform::Window win = static_cast<platform::Window&&>(win_result.value());
    core::log_infof("window open %ux%u. WASD moves, mouse looks, Escape quits.", win.width(),
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

        frame.reset();
        ++frames;

        // Once-a-second heartbeat so a run shows live proof of life.
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
    }

    const f64 seconds = static_cast<f64>(core::Timer::now_ns() - start_ns) * 1e-9;
    core::log_infof(
        "ran %llu frames, %llu ticks in %.2fs. cam yaw=%.3f pitch=%.3f pos=(%.2f,%.2f,%.2f) hash=%016llx",
        static_cast<unsigned long long>(frames), static_cast<unsigned long long>(total_ticks),
        seconds, static_cast<f64>(world.cam_yaw), static_cast<f64>(world.cam_pitch),
        static_cast<f64>(world.cam_x), static_cast<f64>(world.cam_y),
        static_cast<f64>(world.cam_z), static_cast<unsigned long long>(sim::hash(world)));

    (void)permanent;
    (void)scratch;
    return 0;
}
