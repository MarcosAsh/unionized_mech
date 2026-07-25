#include "sim/sim.h"

#include "sim/sim_math.h"

namespace sim {

namespace {

// Character controller tuning, in metres and seconds. These are the feel knobs.
// Every value is a compile-time constant, so the arithmetic below is
// bit-identical on every target.
constexpr f32 LOOK_SCALE = 0.0025f;    // radians per mouse count
constexpr f32 PITCH_LIMIT = 1.55334f;  // just under pi/2
constexpr f32 GRAVITY = 20.0f;         // downward acceleration
constexpr f32 JUMP_VELOCITY = 7.0f;    // upward launch speed on jump
constexpr f32 MAX_GROUND_SPEED = 8.0f; // target speed while running
constexpr f32 GROUND_ACCEL = 10.0f;    // how fast ground speed is gained
constexpr f32 AIR_ACCEL = 12.0f;       // air control strength
constexpr f32 AIR_MAX_SPEED = 1.5f;    // capped air wish speed enables airstrafe
constexpr f32 FRICTION = 6.0f;         // ground friction
constexpr f32 STOP_SPEED = 1.5f;       // floor on friction so slow stops are crisp

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

    next.cam_yaw = wrap_angle(prev.cam_yaw + static_cast<f32>(cmd.look_dx) * LOOK_SCALE);

    f32 pitch = prev.cam_pitch + static_cast<f32>(cmd.look_dy) * LOOK_SCALE;
    if (pitch > PITCH_LIMIT) {
        pitch = PITCH_LIMIT;
    }
    if (pitch < -PITCH_LIMIT) {
        pitch = -PITCH_LIMIT;
    }
    next.cam_pitch = pitch;

    // Wish direction on the ground plane, relative to yaw. At yaw 0 the camera
    // faces -Z, so forward is (sin yaw, 0, -cos yaw) and right is (cos yaw, 0, sin yaw).
    const f32 s = sim_sin(next.cam_yaw);
    const f32 c = sim_cos(next.cam_yaw);
    const f32 mx = static_cast<f32>(cmd.move_x);
    const f32 my = static_cast<f32>(cmd.move_y);
    f32 wish_x = c * mx + s * my;
    f32 wish_z = s * mx - c * my;
    const f32 wish_len = sim_sqrt(wish_x * wish_x + wish_z * wish_z);
    if (wish_len > 0.0f) {
        wish_x /= wish_len;
        wish_z /= wish_len;
    }

    const bool grounded = prev.on_ground != 0;

    // Ground friction (Quake/Source style).
    if (grounded) {
        const f32 speed = sim_sqrt(next.vel_x * next.vel_x + next.vel_z * next.vel_z);
        if (speed > 0.0f) {
            const f32 control = speed < STOP_SPEED ? STOP_SPEED : speed;
            f32 newspeed = speed - control * FRICTION * SIM_DT;
            if (newspeed < 0.0f) {
                newspeed = 0.0f;
            }
            const f32 scale = newspeed / speed;
            next.vel_x *= scale;
            next.vel_z *= scale;
        }
    }

    // Accelerate toward the wish direction. A low air wish speed with real accel
    // is what gives airstrafing and momentum retention.
    if (wish_len > 0.0f) {
        const f32 wish_speed = grounded ? MAX_GROUND_SPEED : AIR_MAX_SPEED;
        const f32 accel = grounded ? GROUND_ACCEL : AIR_ACCEL;
        const f32 current = next.vel_x * wish_x + next.vel_z * wish_z;
        const f32 add_speed = wish_speed - current;
        if (add_speed > 0.0f) {
            f32 accel_speed = accel * SIM_DT * wish_speed;
            if (accel_speed > add_speed) {
                accel_speed = add_speed;
            }
            next.vel_x += accel_speed * wish_x;
            next.vel_z += accel_speed * wish_z;
        }
    }

    next.vel_y -= GRAVITY * SIM_DT;
    if (grounded && sim::button_down(cmd.buttons, Button::Jump)) {
        next.vel_y = JUMP_VELOCITY;
    }

    next.cam_x += next.vel_x * SIM_DT;
    next.cam_y += next.vel_y * SIM_DT;
    next.cam_z += next.vel_z * SIM_DT;

    // Floor at y = 0. Cube collision arrives in the next M3 step.
    if (next.cam_y <= 0.0f) {
        next.cam_y = 0.0f;
        if (next.vel_y < 0.0f) {
            next.vel_y = 0.0f;
        }
        next.on_ground = 1;
    } else {
        next.on_ground = 0;
    }
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
    h = fnv1a(h, &w.vel_x, sizeof(w.vel_x));
    h = fnv1a(h, &w.vel_y, sizeof(w.vel_y));
    h = fnv1a(h, &w.vel_z, sizeof(w.vel_z));
    h = fnv1a(h, &w.on_ground, sizeof(w.on_ground));
    return h;
}

}  // namespace sim
