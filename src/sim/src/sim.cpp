#include "sim/sim.h"

#include "sim_level.h"
#include "sim_math.h"

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

constexpr f32 SLIDE_FRICTION = 1.5f;   // low friction so a slide keeps momentum
constexpr f32 SLIDE_MIN_SPEED = 5.0f;  // crouch above this speed becomes a slide
constexpr f32 SLIDE_BOOST = 4.0f;         // speed added when a slide begins
constexpr f32 SLIDE_ACCEL = 4.0f;         // continued speed gain during a slide
constexpr f32 SLIDE_MAX_SPEED = 16.0f;    // cap a slide builds toward
constexpr f32 SLIDE_STEER_SPEED = 4.0f;   // weak steering wish speed while sliding
constexpr f32 SLIDE_STEER_ACCEL = 4.0f;   // weak steering accel while sliding

// Double jump.
constexpr u8 MAX_AIR_JUMPS = 1;  // one extra jump in the air

// Forgiveness. These raise the floor without touching the ceiling.
constexpr u8 COYOTE_TICKS = 6;       // jump still works ~100ms after a ledge
constexpr u8 JUMP_BUFFER_TICKS = 8;  // press ~133ms early still fires on landing

// Mantle. Moving into a ledge at chest height vaults it, keeping momentum,
// instead of stopping dead. The most common flow-killer, removed.
constexpr f32 MANTLE_REACH = 1.3f;   // highest ledge that can be vaulted
constexpr f32 MANTLE_MARGIN = 0.5f;  // extra launch speed past the ledge lip

// Ledge climb. Holding jump against a wall whose top is within reach pulls the
// player up it, slower than a jump but sure. The mantle takes over at the lip.
constexpr f32 CLIMB_REACH = 3.0f;  // wall top at most this far above the feet
constexpr f32 CLIMB_SPEED = 3.5f;  // upward speed while climbing

constexpr f32 LAND_IMPACT_DECAY = 0.85f;  // per-tick decay of the landing dip

// Wallrun. Speed builds the longer you run, which is the Titanfall signature.
// The detection reach lives in sim_level.h with the hull.
constexpr f32 WALL_PULL = 2.0f;           // gentle pull toward the wall to stay stuck
constexpr f32 WALLRUN_MIN_SPEED = 4.0f;   // along-wall speed needed to start
constexpr f32 WALLRUN_ACCEL = 6.0f;       // along-wall acceleration while running
constexpr f32 WALLRUN_MAX_SPEED = 16.0f;  // speed a wallrun builds toward
constexpr f32 WALLRUN_GRAVITY = 6.0f;     // reduced gravity while wallrunning
constexpr f32 WALLRUN_MAX_FALL = 3.0f;    // cap downward speed on the wall
constexpr u16 WALLRUN_MAX_TICKS = 120;    // how long a single wallrun can last
constexpr f32 WALLJUMP_UP = 6.0f;         // upward launch off the wall
constexpr f32 WALLJUMP_PUSH = 6.0f;       // push away from the wall

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
    const bool ducked = button_down(cmd.buttons, Button::Crouch);
    const f32 hull_h = ducked ? DUCK_HEIGHT : HULL_HEIGHT;
    const bool jump_down = button_down(cmd.buttons, Button::Jump);
    const bool jump_pressed = jump_down && prev.jump_was_down == 0;

    // Forgiveness bookkeeping. A fresh press arms the jump buffer, and coyote
    // ticks count time since the ground was last underfoot.
    u8 jump_buffer = jump_pressed ? JUMP_BUFFER_TICKS
                                  : (prev.jump_buffer > 0 ? static_cast<u8>(prev.jump_buffer - 1)
                                                          : static_cast<u8>(0));
    const u8 coyote =
        grounded ? static_cast<u8>(0)
                 : (prev.coyote_ticks < 250 ? static_cast<u8>(prev.coyote_ticks + 1)
                                            : static_cast<u8>(250));

    // A ground jump this tick skips friction, so a hop timed on landing keeps its
    // speed. That plus air acceleration is what makes bunnyhopping work. A
    // buffered press from just before landing counts.
    const bool ground_jump = grounded && (jump_down || jump_buffer > 0);
    const bool sliding = grounded && ducked &&
                         sim_sqrt(next.vel_x * next.vel_x + next.vel_z * next.vel_z) > SLIDE_MIN_SPEED;

    // Ground friction (Quake/Source style). Sliding uses a low friction.
    if (grounded && !ground_jump) {
        const f32 speed = sim_sqrt(next.vel_x * next.vel_x + next.vel_z * next.vel_z);
        if (speed > 0.0f) {
            const f32 friction = sliding ? SLIDE_FRICTION : FRICTION;
            const f32 control = speed < STOP_SPEED ? STOP_SPEED : speed;
            f32 newspeed = speed - control * friction * SIM_DT;
            if (newspeed < 0.0f) {
                newspeed = 0.0f;
            }
            const f32 scale = newspeed / speed;
            next.vel_x *= scale;
            next.vel_z *= scale;
        }
    }

    // Accelerate toward the wish direction. A low air wish speed with real accel
    // gives airstrafing. While sliding, steering is weak so the slide holds its
    // line rather than letting you run out of it.
    if (wish_len > 0.0f) {
        f32 wish_speed;
        f32 accel;
        if (sliding) {
            wish_speed = SLIDE_STEER_SPEED;
            accel = SLIDE_STEER_ACCEL;
        } else if (grounded) {
            wish_speed = MAX_GROUND_SPEED;
            accel = GROUND_ACCEL;
        } else {
            wish_speed = AIR_MAX_SPEED;
            accel = AIR_ACCEL;
        }
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

    // Slide dynamics. A slide starts with a burst and then keeps building speed
    // over time, rather than bleeding off, which is the TF2 slide feel.
    if (sliding) {
        const f32 hs = sim_sqrt(next.vel_x * next.vel_x + next.vel_z * next.vel_z);
        if (hs > 0.0f) {
            const bool was_sliding = prev.state == MoveState::Slide;
            f32 target = was_sliding ? hs + SLIDE_ACCEL * SIM_DT : hs + SLIDE_BOOST;
            if (target > SLIDE_MAX_SPEED) {
                target = SLIDE_MAX_SPEED;
            }
            const f32 scale = target / hs;
            next.vel_x *= scale;
            next.vel_z *= scale;
        }
    }

    // Wallrun: while airborne and moving along a nearby wall, stick to it, build
    // speed the longer you run, and allow a launch off it.
    f32 wall_nx = 0.0f;
    f32 wall_nz = 0.0f;
    f32 wall_top = 0.0f;
    const bool wall_found = !grounded && find_wall(next.cam_x, next.cam_y, next.cam_z, hull_h,
                                                   level_boxes(), &wall_nx, &wall_nz, &wall_top);
    bool wallrunning = false;
    if (wall_found && prev.wallrun_ticks < WALLRUN_MAX_TICKS) {
        const f32 into = next.vel_x * wall_nx + next.vel_z * wall_nz;
        const f32 along_x = next.vel_x - into * wall_nx;
        const f32 along_z = next.vel_z - into * wall_nz;
        const f32 along = sim_sqrt(along_x * along_x + along_z * along_z);
        if (along > WALLRUN_MIN_SPEED && into < 1.0f) {
            wallrunning = true;
            // Run along the wall and accelerate up to the cap, plus a gentle pull
            // toward the wall so you can attach from a short gap and stay stuck.
            const f32 dir_x = along_x / along;
            const f32 dir_z = along_z / along;
            f32 new_along = along + WALLRUN_ACCEL * SIM_DT;
            if (new_along > WALLRUN_MAX_SPEED) {
                new_along = WALLRUN_MAX_SPEED;
            }
            next.vel_x = dir_x * new_along - wall_nx * WALL_PULL;
            next.vel_z = dir_z * new_along - wall_nz * WALL_PULL;
        }
    }

    next.vel_y -= (wallrunning ? WALLRUN_GRAVITY : GRAVITY) * SIM_DT;
    if (wallrunning && next.vel_y < -WALLRUN_MAX_FALL) {
        next.vel_y = -WALLRUN_MAX_FALL;
    }

    // Ledge climb: airborne against a wall whose top is within reach, holding
    // jump and pushing toward it, rises steadily instead of falling off. The
    // mantle vaults the lip once it comes within its reach.
    bool climbing = false;
    if (wall_found && !wallrunning && jump_down) {
        const f32 ledge_h = wall_top - next.cam_y;
        const f32 toward = -(wish_x * wall_nx + wish_z * wall_nz);
        if (ledge_h > 0.0f && ledge_h <= CLIMB_REACH && toward > 0.3f) {
            climbing = true;
            next.vel_y = CLIMB_SPEED;
            // Hug the wall and drop horizontal drift, trading speed for safety.
            next.vel_x = -wall_nx * WALL_PULL;
            next.vel_z = -wall_nz * WALL_PULL;
        }
    }

    // Jumping. Ground jump auto-hops while held or fires from the buffer
    // (friction was skipped above). A fresh press just after walking off a ledge
    // gets the coyote jump. Wall jump and double jump fire on a fresh press.
    // Touching the ground or a wall refills the air jump.
    const bool coyote_ok = !grounded && !wallrunning && coyote <= COYOTE_TICKS && prev.vel_y <= 0.0f;
    if (grounded) {
        next.air_jumps = MAX_AIR_JUMPS;
        if (jump_down || jump_buffer > 0) {
            next.vel_y = JUMP_VELOCITY;
            jump_buffer = 0;
        }
    } else if (wallrunning) {
        next.air_jumps = MAX_AIR_JUMPS;
        if (jump_pressed) {
            next.vel_y = WALLJUMP_UP;
            next.vel_x += wall_nx * WALLJUMP_PUSH;
            next.vel_z += wall_nz * WALLJUMP_PUSH;
            jump_buffer = 0;
        }
    } else if (climbing) {
        // Wall contact refills the double jump; the held button climbs.
        next.air_jumps = MAX_AIR_JUMPS;
    } else if (jump_pressed && coyote_ok) {
        // Coyote jump. The ledge was left a moment ago, treat it as a ground
        // jump and keep the double jump in reserve.
        next.vel_y = JUMP_VELOCITY;
        jump_buffer = 0;
    } else if (jump_pressed && next.air_jumps > 0) {
        // Double jump. Redirect momentum toward the current input so you can
        // change direction in the air, then relaunch.
        if (wish_len > 0.0f) {
            const f32 hspeed = sim_sqrt(next.vel_x * next.vel_x + next.vel_z * next.vel_z);
            next.vel_x = wish_x * hspeed;
            next.vel_z = wish_z * hspeed;
        }
        next.vel_y = JUMP_VELOCITY;
        next.air_jumps -= 1;
        jump_buffer = 0;
    }
    next.jump_was_down = jump_down ? 1 : 0;
    next.jump_buffer = jump_buffer;
    next.coyote_ticks = coyote;

    // Move and slide, one axis at a time, against the static boxes. Resolving
    // per axis lets the player slide along walls and stand on box tops.
    const core::Span<const Aabb> boxes = level_boxes();

    next.cam_x += next.vel_x * SIM_DT;
    for (u64 i = 0; i < boxes.size(); ++i) {
        const Aabb& b = boxes[i];
        if (hull_overlaps(next.cam_x, next.cam_y, next.cam_z, hull_h, b)) {
            const f32 ledge = b.max_y - next.cam_y;
            next.cam_x = next.vel_x > 0.0f ? b.min_x - HULL_HALF_WIDTH : b.max_x + HULL_HALF_WIDTH;
            if (ledge > 0.0f && ledge <= MANTLE_REACH) {
                // Mantle: launch over the lip and keep horizontal speed, so the
                // ledge is vaulted instead of ending the run.
                const f32 needed = sim_sqrt(2.0f * GRAVITY * ledge) + MANTLE_MARGIN;
                if (next.vel_y < needed) {
                    next.vel_y = needed;
                }
            } else {
                next.vel_x = 0.0f;
            }
        }
    }

    next.cam_z += next.vel_z * SIM_DT;
    for (u64 i = 0; i < boxes.size(); ++i) {
        const Aabb& b = boxes[i];
        if (hull_overlaps(next.cam_x, next.cam_y, next.cam_z, hull_h, b)) {
            const f32 ledge = b.max_y - next.cam_y;
            next.cam_z = next.vel_z > 0.0f ? b.min_z - HULL_HALF_WIDTH : b.max_z + HULL_HALF_WIDTH;
            if (ledge > 0.0f && ledge <= MANTLE_REACH) {
                const f32 needed = sim_sqrt(2.0f * GRAVITY * ledge) + MANTLE_MARGIN;
                if (next.vel_y < needed) {
                    next.vel_y = needed;
                }
            } else {
                next.vel_z = 0.0f;
            }
        }
    }

    // The landing dip eases back every tick and refills on impact, sized by the
    // fall speed, so hard landings read harder. Render-side juice only.
    next.land_impact = prev.land_impact * LAND_IMPACT_DECAY;

    bool grounded_now = false;
    next.cam_y += next.vel_y * SIM_DT;
    for (u64 i = 0; i < boxes.size(); ++i) {
        const Aabb& b = boxes[i];
        if (hull_overlaps(next.cam_x, next.cam_y, next.cam_z, hull_h, b)) {
            if (next.vel_y <= 0.0f) {
                next.cam_y = b.max_y;  // land on top
                grounded_now = true;
                if (prev.on_ground == 0 && -next.vel_y > next.land_impact) {
                    next.land_impact = -next.vel_y;
                }
            } else {
                next.cam_y = b.min_y - hull_h;  // bumped head
            }
            next.vel_y = 0.0f;
        }
    }

    if (next.cam_y <= 0.0f) {
        next.cam_y = 0.0f;
        if (next.vel_y < 0.0f) {
            if (prev.on_ground == 0 && -next.vel_y > next.land_impact) {
                next.land_impact = -next.vel_y;
            }
            next.vel_y = 0.0f;
        }
        grounded_now = true;
    }
    next.on_ground = grounded_now ? 1 : 0;
    next.ducked = ducked ? 1 : 0;

    const bool wallrun_active = wallrunning && !grounded_now;
    next.wallrun_ticks = wallrun_active ? static_cast<u16>(prev.wallrun_ticks + 1) : 0;
    next.wall_nx = wallrun_active ? wall_nx : 0.0f;
    next.wall_nz = wallrun_active ? wall_nz : 0.0f;

    const f32 final_speed = sim_sqrt(next.vel_x * next.vel_x + next.vel_z * next.vel_z);
    if (grounded_now) {
        next.state = (ducked && final_speed > SLIDE_MIN_SPEED) ? MoveState::Slide : MoveState::Ground;
    } else if (wallrun_active) {
        next.state = MoveState::Wallrun;
    } else if (climbing) {
        next.state = MoveState::Climb;
    } else {
        next.state = MoveState::Air;
    }

    // Expose a nearby wall while airborne so the camera can lean toward it
    // before the run starts, which reads as anticipation.
    if (next.state == MoveState::Air) {
        f32 nx = 0.0f;
        f32 nz = 0.0f;
        f32 top = 0.0f;
        if (find_wall(next.cam_x, next.cam_y, next.cam_z, hull_h, boxes, &nx, &nz, &top)) {
            next.wall_nx = nx;
            next.wall_nz = nz;
        }
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
    h = fnv1a(h, &w.wall_nx, sizeof(w.wall_nx));
    h = fnv1a(h, &w.wall_nz, sizeof(w.wall_nz));
    h = fnv1a(h, &w.land_impact, sizeof(w.land_impact));
    h = fnv1a(h, &w.wallrun_ticks, sizeof(w.wallrun_ticks));
    h = fnv1a(h, &w.state, sizeof(w.state));
    h = fnv1a(h, &w.on_ground, sizeof(w.on_ground));
    h = fnv1a(h, &w.ducked, sizeof(w.ducked));
    h = fnv1a(h, &w.air_jumps, sizeof(w.air_jumps));
    h = fnv1a(h, &w.jump_was_down, sizeof(w.jump_was_down));
    h = fnv1a(h, &w.coyote_ticks, sizeof(w.coyote_ticks));
    h = fnv1a(h, &w.jump_buffer, sizeof(w.jump_buffer));
    return h;
}

}  // namespace sim
