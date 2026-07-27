#include "sim_internal.h"
#include "sim_level.h"
#include "sim_math.h"

namespace sim {

namespace {

// Character controller tuning, in metres and seconds. These are the feel knobs.
// Every value is a compile-time constant, so the arithmetic below is
// bit-identical on every target.
constexpr f32 LOOK_SCALE = 0.0025f;    // radians per mouse count
constexpr f32 PITCH_LIMIT = 1.55334f;  // just under pi/2
constexpr f32 GRAVITY = 15.0f;          // sv_gravity 750 x gravityscale 0.80
constexpr f32 JUMP_VELOCITY = 6.76f;    // jumpheight 60u: sqrt(2 * G * 1.524m)
constexpr f32 MAX_GROUND_SPEED = 6.6f;  // sprintspeed 260 u/s
constexpr f32 GROUND_ACCEL = 10.0f;    // how fast ground speed is gained
constexpr f32 AIR_ACCEL = 12.0f;       // air control strength
constexpr f32 AIR_MAX_SPEED = 1.5f;    // capped air wish speed enables airstrafe
constexpr f32 FRICTION = 6.0f;         // ground friction
constexpr f32 STOP_SPEED = 1.5f;       // floor on friction so slow stops are crisp

constexpr f32 SLIDE_MIN_SPEED = 5.0f;     // crouch above this speed becomes a slide
constexpr f32 SLIDE_DECEL = 1.27f;        // slidedecel 50 u/s^2, replaces friction
constexpr f32 SLIDE_BOOST = 2.0f;         // entry burst, see the note at its use
constexpr f32 SLIDE_MAX_SPEED = 12.0f;    // ceiling the entry burst cannot pass
constexpr f32 SLIDE_STEER_SPEED = 4.0f;   // weak steering wish speed while sliding
constexpr f32 SLIDE_STEER_ACCEL = 4.0f;   // weak steering accel while sliding

constexpr u8 MAX_AIR_JUMPS = 1;  // one extra jump in the air

// Forgiveness. These raise the floor without touching the ceiling.
constexpr u8 COYOTE_TICKS = 6;       // jump still works ~100ms after a ledge
constexpr u8 JUMP_BUFFER_TICKS = 8;  // press ~133ms early still fires on landing

// Mantle. Moving into a ledge at chest height vaults it, keeping momentum.
constexpr f32 MANTLE_REACH = 1.3f;   // highest ledge that can be vaulted
constexpr f32 MANTLE_MARGIN = 0.5f;  // extra launch speed past the ledge lip

// Ledge climb. Holding jump against a wall whose top is within reach pulls the
// character up it, slower than a jump but sure.
constexpr f32 CLIMB_REACH = 3.0f;  // wall top at most this far above the feet
constexpr f32 CLIMB_SPEED = 3.5f;  // upward speed while climbing

constexpr f32 LAND_IMPACT_DECAY = 0.85f;  // per-tick decay of the landing dip

// Wallrun. Speed snaps up hard and the wall lets go of you gradually, which is
// the Titanfall signature. Numbers converted from the pilot definitions.
constexpr f32 WALL_PULL = 2.0f;             // gentle pull toward the wall to stay stuck
constexpr f32 WALLRUN_MIN_SPEED = 4.0f;     // along-wall speed needed to start
constexpr f32 WALLRUN_ACCEL = 38.1f;        // wallrunAccelerateHorizontal 1500 u/s^2
constexpr f32 WALLRUN_MAX_SPEED = 10.7f;    // wallrunMaxSpeedHorizontal 420 u/s
constexpr f32 WALLRUN_MAX_FALL = 3.0f;      // cap downward speed on the wall
constexpr u16 WALLRUN_MAX_TICKS = 105;      // wallrun_timeLimit 1.75s
constexpr u16 WALLRUN_GRAVITY_RAMP = 42;    // wallrun_gravityRampUpTime 0.7s
constexpr f32 WALLJUMP_UP = 5.84f;          // wallrunJumpUpSpeed 230 u/s
constexpr f32 WALLJUMP_PUSH = 5.21f;        // wallrunJumpOutwardSpeed 205 u/s

}  // namespace

void step_character(Character& c, const Character& prev_c, const InputCmd& cmd) {
    c.yaw = wrap_angle(prev_c.yaw + static_cast<f32>(cmd.look_dx) * LOOK_SCALE);

    f32 pitch = prev_c.pitch + static_cast<f32>(cmd.look_dy) * LOOK_SCALE;
    if (pitch > PITCH_LIMIT) {
        pitch = PITCH_LIMIT;
    }
    if (pitch < -PITCH_LIMIT) {
        pitch = -PITCH_LIMIT;
    }
    c.pitch = pitch;

    if (c.fire_cooldown > 0) {
        c.fire_cooldown -= 1;
    }
    if (c.shot_age < 255) {
        c.shot_age += 1;
    }
    if (c.hurt_age < 255) {
        c.hurt_age += 1;
    }

    // Wish direction on the ground plane, relative to yaw. At yaw 0 the
    // character faces -Z.
    const f32 s = sim_sin(c.yaw);
    const f32 co = sim_cos(c.yaw);
    const f32 mx = static_cast<f32>(cmd.move_x);
    const f32 my = static_cast<f32>(cmd.move_y);
    f32 wish_x = co * mx + s * my;
    f32 wish_z = s * mx - co * my;
    const f32 wish_len = sim_sqrt(wish_x * wish_x + wish_z * wish_z);
    if (wish_len > 0.0f) {
        wish_x /= wish_len;
        wish_z /= wish_len;
    }

    const bool grounded = prev_c.on_ground != 0;
    const bool ducked = button_down(cmd.buttons, Button::Crouch);
    const f32 hull_h = ducked ? DUCK_HEIGHT : HULL_HEIGHT;
    const bool jump_down = button_down(cmd.buttons, Button::Jump);
    const bool jump_pressed = jump_down && prev_c.jump_was_down == 0;

    // Forgiveness bookkeeping.
    u8 jump_buffer = jump_pressed ? JUMP_BUFFER_TICKS
                                  : (prev_c.jump_buffer > 0 ? static_cast<u8>(prev_c.jump_buffer - 1)
                                                            : static_cast<u8>(0));
    const u8 coyote =
        grounded ? static_cast<u8>(0)
                 : (prev_c.coyote_ticks < 250 ? static_cast<u8>(prev_c.coyote_ticks + 1)
                                              : static_cast<u8>(250));

    // A ground jump this tick skips friction, so a hop timed on landing keeps
    // its speed: bunnyhopping.
    const bool ground_jump = grounded && (jump_down || jump_buffer > 0);
    const bool sliding =
        grounded && ducked && sim_sqrt(c.vx * c.vx + c.vz * c.vz) > SLIDE_MIN_SPEED;

    if (grounded && !ground_jump) {
        const f32 speed = sim_sqrt(c.vx * c.vx + c.vz * c.vz);
        if (speed > 0.0f) {
            // A slide swaps proportional friction for a flat deceleration, so
            // it sheds a fixed amount per second instead of a fraction of what
            // is left: the faster you enter, the further it carries you.
            f32 newspeed;
            if (sliding) {
                newspeed = speed - SLIDE_DECEL * SIM_DT;
            } else {
                const f32 control = speed < STOP_SPEED ? STOP_SPEED : speed;
                newspeed = speed - control * FRICTION * SIM_DT;
            }
            if (newspeed < 0.0f) {
                newspeed = 0.0f;
            }
            const f32 scale = newspeed / speed;
            c.vx *= scale;
            c.vz *= scale;
        }
    }

    // Accelerate toward the wish direction (projection-limited, Quake style).
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
        const f32 current = c.vx * wish_x + c.vz * wish_z;
        const f32 add_speed = wish_speed - current;
        if (add_speed > 0.0f) {
            f32 accel_speed = accel * SIM_DT * wish_speed;
            if (accel_speed > add_speed) {
                accel_speed = add_speed;
            }
            c.vx += accel_speed * wish_x;
            c.vz += accel_speed * wish_z;
        }
    }

    // Slide entry burst, paid once. Titanfall has no such boost: a slide there
    // only spends momentum you arrived with, and the speed comes back from
    // slopes. This arena is flat, so entry pays out instead and the decel above
    // takes it back over the length of the slide. Revisit once the level has
    // gradients worth sliding down.
    if (sliding && prev_c.state != MoveState::Slide) {
        const f32 hs = sim_sqrt(c.vx * c.vx + c.vz * c.vz);
        if (hs > 0.0f) {
            f32 target = hs + SLIDE_BOOST;
            if (target > SLIDE_MAX_SPEED) {
                target = SLIDE_MAX_SPEED;
            }
            const f32 scale = target / hs;
            c.vx *= scale;
            c.vz *= scale;
        }
    }

    // Wallrun.
    f32 wall_nx = 0.0f;
    f32 wall_nz = 0.0f;
    f32 wall_top = 0.0f;
    const bool wall_found = !grounded && find_wall(c.x, c.y, c.z, hull_h, level_boxes(), &wall_nx,
                                                   &wall_nz, &wall_top);
    bool wallrunning = false;
    if (wall_found && prev_c.wallrun_ticks < WALLRUN_MAX_TICKS) {
        const f32 into = c.vx * wall_nx + c.vz * wall_nz;
        const f32 along_x = c.vx - into * wall_nx;
        const f32 along_z = c.vz - into * wall_nz;
        const f32 along = sim_sqrt(along_x * along_x + along_z * along_z);
        if (along > WALLRUN_MIN_SPEED && into < 1.0f) {
            wallrunning = true;
            const f32 dir_x = along_x / along;
            const f32 dir_z = along_z / along;
            f32 new_along = along + WALLRUN_ACCEL * SIM_DT;
            if (new_along > WALLRUN_MAX_SPEED) {
                new_along = WALLRUN_MAX_SPEED;
            }
            c.vx = dir_x * new_along - wall_nx * WALL_PULL;
            c.vz = dir_z * new_along - wall_nz * WALL_PULL;
        }
    }

    // Gravity eases in over the first stretch of a wallrun rather than sitting
    // at a flat reduced value. Entering sticks you to the wall; the longer you
    // hang on, the more the wall gives you back to gravity.
    f32 gravity = GRAVITY;
    if (wallrunning) {
        const u16 held = prev_c.wallrun_ticks;
        gravity = held >= WALLRUN_GRAVITY_RAMP
                      ? GRAVITY
                      : GRAVITY * (static_cast<f32>(held) /
                                   static_cast<f32>(WALLRUN_GRAVITY_RAMP));
    }
    c.vy -= gravity * SIM_DT;
    if (wallrunning && c.vy < -WALLRUN_MAX_FALL) {
        c.vy = -WALLRUN_MAX_FALL;
    }

    // Ledge climb: airborne against a wall whose top is within reach, holding
    // jump and pushing toward it.
    bool climbing = false;
    if (wall_found && !wallrunning && jump_down) {
        const f32 ledge_h = wall_top - c.y;
        const f32 toward = -(wish_x * wall_nx + wish_z * wall_nz);
        if (ledge_h > 0.0f && ledge_h <= CLIMB_REACH && toward > 0.3f) {
            climbing = true;
            c.vy = CLIMB_SPEED;
            c.vx = -wall_nx * WALL_PULL;
            c.vz = -wall_nz * WALL_PULL;
        }
    }

    // Jumping: auto-hop on the ground, coyote, wall jump, double jump.
    const bool coyote_ok = !grounded && !wallrunning && coyote <= COYOTE_TICKS && prev_c.vy <= 0.0f;
    if (grounded) {
        c.air_jumps = MAX_AIR_JUMPS;
        if (jump_down || jump_buffer > 0) {
            c.vy = JUMP_VELOCITY;
            jump_buffer = 0;
        }
    } else if (wallrunning) {
        c.air_jumps = MAX_AIR_JUMPS;
        if (jump_pressed) {
            c.vy = WALLJUMP_UP;
            c.vx += wall_nx * WALLJUMP_PUSH;
            c.vz += wall_nz * WALLJUMP_PUSH;
            jump_buffer = 0;
        }
    } else if (climbing) {
        c.air_jumps = MAX_AIR_JUMPS;
    } else if (jump_pressed && coyote_ok) {
        c.vy = JUMP_VELOCITY;
        jump_buffer = 0;
    } else if (jump_pressed && c.air_jumps > 0) {
        if (wish_len > 0.0f) {
            const f32 hspeed = sim_sqrt(c.vx * c.vx + c.vz * c.vz);
            c.vx = wish_x * hspeed;
            c.vz = wish_z * hspeed;
        }
        c.vy = JUMP_VELOCITY;
        c.air_jumps -= 1;
        jump_buffer = 0;
    }
    c.jump_was_down = jump_down ? 1 : 0;
    c.jump_buffer = jump_buffer;
    c.coyote_ticks = coyote;

    // Move and slide, one axis at a time, against the static boxes.
    const core::Span<const Aabb> boxes = level_boxes();

    c.x += c.vx * SIM_DT;
    for (u64 i = 0; i < boxes.size(); ++i) {
        const Aabb& b = boxes[i];
        if (hull_overlaps(c.x, c.y, c.z, hull_h, b)) {
            const f32 ledge = b.max_y - c.y;
            c.x = c.vx > 0.0f ? b.min_x - HULL_HALF_WIDTH : b.max_x + HULL_HALF_WIDTH;
            if (ledge > 0.0f && ledge <= MANTLE_REACH) {
                const f32 needed = sim_sqrt(2.0f * GRAVITY * ledge) + MANTLE_MARGIN;
                if (c.vy < needed) {
                    c.vy = needed;
                }
            } else {
                c.vx = 0.0f;
            }
        }
    }

    c.z += c.vz * SIM_DT;
    for (u64 i = 0; i < boxes.size(); ++i) {
        const Aabb& b = boxes[i];
        if (hull_overlaps(c.x, c.y, c.z, hull_h, b)) {
            const f32 ledge = b.max_y - c.y;
            c.z = c.vz > 0.0f ? b.min_z - HULL_HALF_WIDTH : b.max_z + HULL_HALF_WIDTH;
            if (ledge > 0.0f && ledge <= MANTLE_REACH) {
                const f32 needed = sim_sqrt(2.0f * GRAVITY * ledge) + MANTLE_MARGIN;
                if (c.vy < needed) {
                    c.vy = needed;
                }
            } else {
                c.vz = 0.0f;
            }
        }
    }

    // The landing dip eases back every tick and refills on impact.
    c.land_impact = prev_c.land_impact * LAND_IMPACT_DECAY;

    bool grounded_now = false;
    c.y += c.vy * SIM_DT;
    for (u64 i = 0; i < boxes.size(); ++i) {
        const Aabb& b = boxes[i];
        if (hull_overlaps(c.x, c.y, c.z, hull_h, b)) {
            if (c.vy <= 0.0f) {
                c.y = b.max_y;  // land on top
                grounded_now = true;
                if (prev_c.on_ground == 0 && -c.vy > c.land_impact) {
                    c.land_impact = -c.vy;
                }
            } else {
                c.y = b.min_y - hull_h;  // bumped head
            }
            c.vy = 0.0f;
        }
    }

    if (c.y <= 0.0f) {
        c.y = 0.0f;
        if (c.vy < 0.0f) {
            if (prev_c.on_ground == 0 && -c.vy > c.land_impact) {
                c.land_impact = -c.vy;
            }
            c.vy = 0.0f;
        }
        grounded_now = true;
    }
    c.on_ground = grounded_now ? 1 : 0;
    c.ducked = ducked ? 1 : 0;

    const bool wallrun_active = wallrunning && !grounded_now;
    c.wallrun_ticks = wallrun_active ? static_cast<u16>(prev_c.wallrun_ticks + 1) : 0;
    c.wall_nx = wallrun_active ? wall_nx : 0.0f;
    c.wall_nz = wallrun_active ? wall_nz : 0.0f;

    const f32 final_speed = sim_sqrt(c.vx * c.vx + c.vz * c.vz);
    if (grounded_now) {
        c.state = (ducked && final_speed > SLIDE_MIN_SPEED) ? MoveState::Slide : MoveState::Ground;
    } else if (wallrun_active) {
        c.state = MoveState::Wallrun;
    } else if (climbing) {
        c.state = MoveState::Climb;
    } else {
        c.state = MoveState::Air;
    }

    // Expose a nearby wall while airborne for the camera's anticipation lean.
    if (c.state == MoveState::Air) {
        f32 nx = 0.0f;
        f32 nz = 0.0f;
        f32 top = 0.0f;
        if (find_wall(c.x, c.y, c.z, hull_h, boxes, &nx, &nz, &top)) {
            c.wall_nx = nx;
            c.wall_nz = nz;
        }
    }
}

}  // namespace sim
