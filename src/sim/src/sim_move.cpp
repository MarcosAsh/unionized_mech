#include "sim_collide.h"
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
constexpr f32 JUMP_VELOCITY = 6.76f;    // launch speed; the apex lands at 1.52m
constexpr f32 WALK_SPEED = 4.41f;       // ground speed while aiming down the sights
constexpr f32 CROUCH_SPEED = 2.03f;     // ground speed while crouched and slow
constexpr f32 GROUND_ACCEL = 10.0f;    // how fast ground speed is gained
constexpr f32 AIR_ACCEL = 12.0f;       // air control strength
constexpr f32 AIR_MAX_SPEED = 1.5f;    // capped air wish speed enables airstrafe
constexpr f32 FRICTION = 6.0f;         // ground friction
constexpr f32 STOP_SPEED = 1.5f;       // floor on friction so slow stops are crisp

constexpr f32 SLIDE_MIN_SPEED = 5.0f;     // crouch above this speed becomes a slide
constexpr f32 SLIDE_DECEL = 1.27f;        // flat slide bleed, replaces friction
constexpr f32 SLIDE_BOOST = 2.0f;         // entry burst, see the note at its use
constexpr u8 SLIDE_BOOST_COOLDOWN = 120;  // two seconds before entry pays again
constexpr f32 SLIDE_STEER_SPEED = 4.0f;   // weak steering wish speed while sliding
constexpr f32 SLIDE_STEER_ACCEL = 4.0f;   // weak steering accel while sliding

constexpr u8 MAX_AIR_JUMPS = 1;  // one extra jump in the air

// Lurch. For a short grace period after any jump, newly pressing a direction
// redirects the momentum you already have toward it. It is the single mechanic
// that makes air control expressive: pressing steers, releasing does not, and
// the whole family of "no-lurch" techniques exists to move without paying for
// it. Turning far from where you are already going costs speed; a small
// correction is free.
constexpr u8 LURCH_FULL_TICKS = 12;      // 0.2s at undiminished strength
constexpr u8 LURCH_END_TICKS = 24;       // strength reaches zero by 0.4s
constexpr f32 LURCH_FREE_COS = 0.9063f;  // cos(25 degrees): redirect inside this is free
constexpr f32 LURCH_TURN = 0.45f;        // radians one full-strength lurch turns you
constexpr f32 LURCH_MAX_LOSS = 0.5f;     // speed given up turning all the way around

/// Lurch strength `ticks` into the window. Full for the first stretch, then
/// eased out to nothing. The real falloff is documented as not being a straight
/// line but its shape is not published, so this is our curve, not a measurement.
[[nodiscard]] f32 lurch_strength(u8 ticks) {
    if (ticks == 0 || ticks > LURCH_END_TICKS) {
        return 0.0f;
    }
    if (ticks <= LURCH_FULL_TICKS) {
        return 1.0f;
    }
    const f32 span = static_cast<f32>(LURCH_END_TICKS - LURCH_FULL_TICKS);
    const f32 gone = static_cast<f32>(ticks - LURCH_FULL_TICKS) / span;
    const f32 left = 1.0f - gone;
    return left * left;
}

// Forgiveness. These raise the floor without touching the ceiling.
constexpr u8 COYOTE_TICKS = 6;       // jump still works ~100ms after a ledge
constexpr u8 JUMP_BUFFER_TICKS = 8;  // press ~133ms early still fires on landing

// Ledge climb. Holding jump against a wall whose top is within reach pulls the
// character up it, slower than a jump but sure.
constexpr f32 CLIMB_REACH = 3.0f;  // wall top at most this far above the feet
constexpr f32 CLIMB_SPEED = 3.5f;  // upward speed while climbing

// Wallrun. Speed snaps up hard and the wall lets go of you gradually, which is
// the signature this game is chasing.
constexpr f32 WALL_PULL = 2.0f;             // gentle pull toward the wall to stay stuck
constexpr f32 WALLRUN_MIN_SPEED = 4.0f;     // along-wall speed needed to start
constexpr f32 WALLRUN_ACCEL = 38.1f;        // along-wall acceleration, near instant
constexpr f32 WALLRUN_MAX_SPEED = 10.7f;    // along-wall speed ceiling
constexpr f32 WALLRUN_MAX_FALL = 3.0f;      // cap downward speed on the wall
constexpr u16 WALLRUN_MAX_TICKS = 105;      // 1.75s before the wall gives up on you
constexpr u16 WALLRUN_GRAVITY_RAMP = 42;    // 0.7s to ease gravity back to full
constexpr f32 WALLJUMP_UP = 5.84f;          // upward launch off the wall
constexpr f32 WALLJUMP_PUSH = 5.21f;        // push away from the wall
constexpr f32 WALLJUMP_INPUT = 2.03f;       // stick direction folded into the launch

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
    const bool aiming = button_down(cmd.buttons, Button::Aim);
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
            // Crouching walks; aiming down the sights costs you the sprint and
            // leaves you at a walk, so committing to a shot commits your feet.
            wish_speed = ducked ? CROUCH_SPEED : (aiming ? WALK_SPEED : RUN_SPEED);
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

    // Slide entry burst, paid once and then rate-limited. Without the cooldown
    // a slide can be re-entered every few ticks, and tapping crouch becomes a
    // free speed pump that outruns everything else in the game.
    c.slide_cooldown = prev_c.slide_cooldown > 0 ? static_cast<u8>(prev_c.slide_cooldown - 1)
                                                 : static_cast<u8>(0);
    if (sliding && prev_c.state != MoveState::Slide && c.slide_cooldown == 0) {
        const f32 hs = sim_sqrt(c.vx * c.vx + c.vz * c.vz);
        if (hs > 0.0f) {
            f32 target = hs + SLIDE_BOOST;
            if (target > TOP_SPEED) {
                target = TOP_SPEED;
            }
            const f32 scale = target / hs;
            c.vx *= scale;
            c.vz *= scale;
        }
        c.slide_cooldown = SLIDE_BOOST_COOLDOWN;
    }

    // Wallrun.
    f32 wall_nx = 0.0f;
    f32 wall_nz = 0.0f;
    f32 wall_top = 0.0f;
    const bool wall_found = !grounded && find_wall(c.x, c.y, c.z, hull_h, level_boxes(), &wall_nx,
                                                   &wall_nz, &wall_top);

    // The run timer belongs to the *wall*, not to the contact. It carries
    // through the ticks spent off a face, and only a different face or the floor
    // hands it back. That is what makes the limit mean anything: it used to
    // reset the moment contact was lost, so dropping off for a single tick
    // refunded both the timer and the gravity ramp below, and one face would
    // carry a character for as long as it cared to hold on.
    i8 latch_nx = prev_c.wall_latch_nx;
    i8 latch_nz = prev_c.wall_latch_nz;
    u16 wall_ticks = prev_c.wallrun_ticks;

    // Crouching lets go. A wall is something you hold onto, so releasing it
    // wants a button of its own rather than only ever happening to you.
    bool wallrunning = false;
    if (wall_found && !ducked) {
        // find_wall only ever reports axis-aligned normals, so a face is named
        // by two bytes, and comparing them separates "the wall I have been
        // running" from "a fresh one" without carrying a box index around. It
        // cannot tell two faces that point the same way apart, which is a
        // deliberate under-approximation: it refuses a little more than it has
        // to, and never grants a run it should not.
        const i8 face_nx = static_cast<i8>(wall_nx);
        const i8 face_nz = static_cast<i8>(wall_nz);
        if (face_nx != latch_nx || face_nz != latch_nz) {
            // A wall this character was not already on is free to take, and
            // starts a fresh run. Landing clears the latch too, so faces are
            // always fresh again on the way out of a spawn.
            latch_nx = face_nx;
            latch_nz = face_nz;
            wall_ticks = 0;
        }
        if (wall_ticks < WALLRUN_MAX_TICKS) {
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
    }

    // Gravity eases in over the first stretch of a wallrun rather than sitting
    // at a flat reduced value. Entering sticks you to the wall; the longer you
    // hang on, the more the wall gives you back to gravity. It reads the wall's
    // own timer, so re-catching a face you have already been running does not
    // hand the easy first stretch back.
    f32 gravity = GRAVITY;
    if (wallrunning) {
        gravity = wall_ticks >= WALLRUN_GRAVITY_RAMP
                      ? GRAVITY
                      : GRAVITY * (static_cast<f32>(wall_ticks) /
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
    // Any jump arms the lurch window below. Walking off a ledge does not.
    bool jumped = false;
    const bool coyote_ok = !grounded && !wallrunning && coyote <= COYOTE_TICKS && prev_c.vy <= 0.0f;
    if (grounded) {
        c.air_jumps = MAX_AIR_JUMPS;
        if (jump_down || jump_buffer > 0) {
            c.vy = JUMP_VELOCITY;
            jump_buffer = 0;
            jumped = true;
        }
    } else if (wallrunning) {
        c.air_jumps = MAX_AIR_JUMPS;
        if (jump_pressed) {
            // Off the wall, plus whatever the stick is asking for, so a wall
            // jump can be aimed instead of only ever throwing you straight out.
            c.vy = WALLJUMP_UP;
            c.vx += wall_nx * WALLJUMP_PUSH + wish_x * WALLJUMP_INPUT;
            c.vz += wall_nz * WALLJUMP_PUSH + wish_z * WALLJUMP_INPUT;
            jump_buffer = 0;
            jumped = true;
        }
    } else if (climbing) {
        c.air_jumps = MAX_AIR_JUMPS;
    } else if (jump_pressed && coyote_ok) {
        c.vy = JUMP_VELOCITY;
        jump_buffer = 0;
        jumped = true;
    } else if (jump_pressed && c.air_jumps > 0) {
        if (wish_len > 0.0f) {
            const f32 hspeed = sim_sqrt(c.vx * c.vx + c.vz * c.vz);
            c.vx = wish_x * hspeed;
            c.vz = wish_z * hspeed;
        }
        c.vy = JUMP_VELOCITY;
        c.air_jumps -= 1;
        jump_buffer = 0;
        jumped = true;
    }
    c.jump_was_down = jump_down ? 1 : 0;
    c.jump_buffer = jump_buffer;
    c.coyote_ticks = coyote;

    // Lurch: a direction newly pressed inside the post-jump window drags the
    // momentum you already have toward it. A redirect, never an impulse, so it
    // cannot manufacture speed; turning past the free cone spends some. Pressing
    // costs, holding and releasing do not, which is the whole reason the
    // no-lurch techniques exist.
    u8 lurch = jumped ? static_cast<u8>(1)
                      : (prev_c.lurch_ticks > 0 && prev_c.lurch_ticks < LURCH_END_TICKS
                             ? static_cast<u8>(prev_c.lurch_ticks + 1)
                             : static_cast<u8>(0));
    const bool pressed_dir = (cmd.move_x != 0 && cmd.move_x != prev_c.last_move_x) ||
                             (cmd.move_y != 0 && cmd.move_y != prev_c.last_move_y);
    const f32 lurch_now = lurch_strength(lurch);
    if (lurch_now > 0.0f && pressed_dir && wish_len > 0.0f) {
        const f32 speed = sim_sqrt(c.vx * c.vx + c.vz * c.vz);
        if (speed > 0.01f) {
            const f32 dir_x = c.vx / speed;
            const f32 dir_z = c.vz / speed;
            const f32 along = dir_x * wish_x + dir_z * wish_z;
            const f32 turn = LURCH_TURN * lurch_now;
            // Rotate the heading toward the press rather than blending the two
            // vectors: blending is degenerate at exactly 180 degrees, where the
            // midpoint of two opposite directions is the origin and the whole
            // redirect silently vanishes. That is the case that matters most.
            f32 nx = wish_x;
            f32 nz = wish_z;
            if (along < sim_cos(turn)) {
                // Further away than one lurch reaches, so turn by that much and
                // no more. The cross product picks which way around.
                const f32 cross = dir_x * wish_z - dir_z * wish_x;
                const f32 ct = sim_cos(turn);
                const f32 st = cross >= 0.0f ? sim_sin(turn) : -sim_sin(turn);
                nx = dir_x * ct - dir_z * st;
                nz = dir_x * st + dir_z * ct;
            }
            // The cost rises with how far off the heading the press was, judged
            // on the dot product rather than the angle so no inverse trig enters
            // the sim. It is monotonic in the angle, which is all the rule needs.
            f32 keep = 1.0f;
            if (along < LURCH_FREE_COS) {
                keep -= LURCH_MAX_LOSS * lurch_now * (LURCH_FREE_COS - along) /
                        (LURCH_FREE_COS + 1.0f);
            }
            c.vx = nx * speed * keep;
            c.vz = nz * speed * keep;
        }
    }

    const bool grounded_now = move_and_slide(c, prev_c, hull_h, grounded);
    c.on_ground = grounded_now ? 1 : 0;
    c.ducked = ducked ? 1 : 0;
    // Landing closes the window; nothing else re-arms it but another jump.
    c.lurch_ticks = grounded_now ? static_cast<u8>(0) : lurch;
    c.last_move_x = cmd.move_x;
    c.last_move_y = cmd.move_y;

    // Touching the floor forgets the wall entirely: every face is fresh again on
    // the way back up. Short of that the latch and its timer persist, including
    // through the ticks spent off the wall after a wall jump.
    const bool wallrun_active = wallrunning && !grounded_now;
    c.wallrun_ticks = grounded_now
                          ? static_cast<u16>(0)
                          : (wallrun_active ? static_cast<u16>(wall_ticks + 1) : wall_ticks);
    c.wall_latch_nx = grounded_now ? static_cast<i8>(0) : latch_nx;
    c.wall_latch_nz = grounded_now ? static_cast<i8>(0) : latch_nz;
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
        if (find_wall(c.x, c.y, c.z, hull_h, level_boxes(), &nx, &nz, &top)) {
            c.wall_nx = nx;
            c.wall_nz = nz;
        }
    }
}

}  // namespace sim
