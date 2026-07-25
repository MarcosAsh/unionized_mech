#include "sim/sim.h"

#include "sim/sim_math.h"

namespace sim {

namespace {

// Gameplay tuning for the M0 demo world. Every value is a compile-time constant,
// so the arithmetic below is bit-identical on every target.
constexpr f32 TAU = 6.28318530717958647692f;
constexpr f32 SPIN_RATE = TAU / (4.0f * static_cast<f32>(SIM_HZ));  // one turn / 4s
constexpr f32 MOVE_SPEED = 6.0f;                                    // units per second
constexpr f32 LOOK_SCALE = 0.0025f;                                 // radians per mouse count
constexpr f32 PITCH_LIMIT = 1.55334f;                               // just under pi/2

u64 fnv1a(u64 h, const void* data, u64 n) {
    const u8* p = static_cast<const u8*>(data);
    for (u64 i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

}  // namespace

void simulate(const World& prev, const InputCmd& cmd, World& next) {
    next = prev;
    next.tick = TickId{prev.tick.raw + 1};

    next.spin_angle = prev.spin_angle + SPIN_RATE;

    next.cam_yaw = wrap_angle(prev.cam_yaw + static_cast<f32>(cmd.look_dx) * LOOK_SCALE);

    f32 pitch = prev.cam_pitch + static_cast<f32>(cmd.look_dy) * LOOK_SCALE;
    if (pitch > PITCH_LIMIT) {
        pitch = PITCH_LIMIT;
    }
    if (pitch < -PITCH_LIMIT) {
        pitch = -PITCH_LIMIT;
    }
    next.cam_pitch = pitch;

    // Yaw-relative movement using deterministic trig. At yaw 0 the camera faces
    // -Z, so forward is (sin yaw, 0, -cos yaw) and right is (cos yaw, 0, sin yaw).
    const f32 s = sim_sin(next.cam_yaw);
    const f32 c = sim_cos(next.cam_yaw);
    const f32 move_x = static_cast<f32>(cmd.move_x);
    const f32 move_y = static_cast<f32>(cmd.move_y);
    const f32 wish_x = c * move_x + s * move_y;
    const f32 wish_z = s * move_x - c * move_y;
    const f32 step = MOVE_SPEED * SIM_DT;
    next.cam_x = prev.cam_x + wish_x * step;
    next.cam_z = prev.cam_z + wish_z * step;
}

u32 FixedTimestep::advance(f64 elapsed, u32 max_ticks) {
    accumulator_ += elapsed;
    const f64 dt = static_cast<f64>(SIM_DT);
    u32 ticks = 0;
    while (accumulator_ >= dt && ticks < max_ticks) {
        accumulator_ -= dt;
        ++ticks;
    }
    return ticks;
}

f32 FixedTimestep::alpha() const {
    return static_cast<f32>(accumulator_ / static_cast<f64>(SIM_DT));
}

u64 hash(const World& w) {
    u64 h = 0xcbf29ce484222325ull;
    h = fnv1a(h, &w.tick.raw, sizeof(w.tick.raw));
    h = fnv1a(h, &w.cam_x, sizeof(w.cam_x));
    h = fnv1a(h, &w.cam_y, sizeof(w.cam_y));
    h = fnv1a(h, &w.cam_z, sizeof(w.cam_z));
    h = fnv1a(h, &w.cam_yaw, sizeof(w.cam_yaw));
    h = fnv1a(h, &w.cam_pitch, sizeof(w.cam_pitch));
    h = fnv1a(h, &w.spin_angle, sizeof(w.spin_angle));
    return h;
}

}  // namespace sim
