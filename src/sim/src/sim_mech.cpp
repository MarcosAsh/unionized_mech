#include "sim_internal.h"
#include "sim_level.h"
#include "sim_math.h"

// The union system: a robot walks up to the dormant chassis at the arena
// centre, presses use, and its processes migrate in. The merged pair moves as
// one heavy body; eject pops the pilot out of the top hatch. While merged the
// pilot character mirrors the mech's position, so every existing spatial
// query (bot targeting, hitscan, line of sight) sees the union naturally.

namespace sim {

namespace {

constexpr f32 MECH_RUN = 7.0f;
constexpr f32 MECH_ACCEL = 28.0f;
constexpr f32 MECH_FRICTION = 5.0f;
constexpr f32 MECH_JUMP = 7.0f;
constexpr f32 MECH_GRAVITY = 22.0f;
constexpr f32 MERGE_RANGE = 5.0f;
constexpr f32 LOOK_SCALE = 0.0025f;   // matches the character controller
constexpr f32 PITCH_LIMIT = 1.55f;

bool mech_blocked(f32 x, f32 y, f32 z) {
    const core::Span<const Aabb> boxes = level_boxes();
    for (u64 i = 0; i < boxes.size(); ++i) {
        const Aabb& b = boxes[i];
        if (x - MECH_HALF_WIDTH < b.max_x && x + MECH_HALF_WIDTH > b.min_x && y < b.max_y &&
            y + MECH_HEIGHT > b.min_y && z - MECH_HALF_WIDTH < b.max_z &&
            z + MECH_HALF_WIDTH > b.min_z) {
            return true;
        }
    }
    return false;
}

// Heavy ground movement: accelerate toward the wish direction, one slow jump,
// no slide, no wallrun. The chassis faces wherever the pilot aims.
void step_mech(Mech& m, f32 yaw, const InputCmd& cmd) {
    m.yaw = yaw;
    const f32 fx = sim_sin(yaw);
    const f32 fz = -sim_cos(yaw);
    const f32 mx = static_cast<f32>(cmd.move_x);
    const f32 my = static_cast<f32>(cmd.move_y);
    f32 wx = fx * my + -fz * mx;
    f32 wz = fz * my + fx * mx;
    const f32 wlen = sim_sqrt(wx * wx + wz * wz);
    if (wlen > 0.01f) {
        wx /= wlen;
        wz /= wlen;
        const f32 step = MECH_ACCEL * SIM_DT;
        f32 dvx = wx * MECH_RUN - m.vx;
        f32 dvz = wz * MECH_RUN - m.vz;
        const f32 dlen = sim_sqrt(dvx * dvx + dvz * dvz);
        if (dlen > step) {
            dvx = dvx / dlen * step;
            dvz = dvz / dlen * step;
        }
        m.vx += dvx;
        m.vz += dvz;
    } else if (m.on_ground != 0) {
        f32 scale = 1.0f - MECH_FRICTION * SIM_DT;
        if (scale < 0.0f) {
            scale = 0.0f;
        }
        m.vx *= scale;
        m.vz *= scale;
    }
    if (button_down(cmd.buttons, Button::Jump) && m.on_ground != 0) {
        m.vy = MECH_JUMP;
    }
    m.vy -= MECH_GRAVITY * SIM_DT;

    const f32 nx = m.x + m.vx * SIM_DT;
    if (mech_blocked(nx, m.y, m.z)) {
        m.vx = 0.0f;
    } else {
        m.x = nx;
    }
    const f32 nz = m.z + m.vz * SIM_DT;
    if (mech_blocked(m.x, m.y, nz)) {
        m.vz = 0.0f;
    } else {
        m.z = nz;
    }
    const f32 ny = m.y + m.vy * SIM_DT;
    if (ny <= 0.0f) {
        m.y = 0.0f;
        m.vy = 0.0f;
        m.on_ground = 1;
    } else if (mech_blocked(m.x, ny, m.z)) {
        if (m.vy < 0.0f) {
            m.on_ground = 1;  // came down onto a box; rest where blocked
        }
        m.vy = 0.0f;
    } else {
        m.y = ny;
        m.on_ground = 0;
    }
}

}  // namespace

void step_union(World& next, const InputCmd cmds[MAX_PLAYERS]) {
    Mech& mech = next.mech;

    // A destroyed chassis rebuilds at the arena centre after the countdown.
    if (mech.alive == 0) {
        if (mech.respawn_ticks > 1) {
            mech.respawn_ticks -= 1;
        } else {
            mech = Mech{};
        }
    }

    // A pilot slot pointing at a character that is no longer merged (killed,
    // respawned) frees the chassis.
    if (mech.pilot != NO_PILOT && next.chars[mech.pilot].merged == 0) {
        mech.pilot = NO_PILOT;
    }

    for (u32 i = 0; i < MAX_PLAYERS; ++i) {
        Character& c = next.chars[i];
        const bool use_down = button_down(cmds[i].buttons, Button::Use);
        const bool use_edge = use_down && c.use_was_down == 0;
        c.use_was_down = use_down ? 1 : 0;
        if (c.alive == 0) {
            continue;
        }

        if (c.merged != 0) {
            if (mech.alive == 0 || mech.pilot != i) {
                c.merged = 0;  // chassis gone from under the pilot
                continue;
            }
            // Aim stays on the pilot; the chassis moves and turns under it.
            c.yaw = wrap_angle(c.yaw + static_cast<f32>(cmds[i].look_dx) * LOOK_SCALE);
            f32 pitch = c.pitch + static_cast<f32>(cmds[i].look_dy) * LOOK_SCALE;
            if (pitch > PITCH_LIMIT) {
                pitch = PITCH_LIMIT;
            }
            if (pitch < -PITCH_LIMIT) {
                pitch = -PITCH_LIMIT;
            }
            c.pitch = pitch;
            step_mech(mech, c.yaw, cmds[i]);
            c.x = mech.x;
            c.y = mech.y;
            c.z = mech.z;
            c.vx = mech.vx;
            c.vy = mech.vy;
            c.vz = mech.vz;
            c.on_ground = mech.on_ground;
            // The combat timers step_character would normally age.
            if (c.fire_cooldown > 0) {
                c.fire_cooldown -= 1;
            }
            if (c.shot_age < 255) {
                c.shot_age += 1;
            }
            if (c.hurt_age < 255) {
                c.hurt_age += 1;
            }
            if (use_edge) {
                eject_pilot(next, i);
            }
            continue;
        }

        if (use_edge && mech.alive != 0 && mech.pilot == NO_PILOT) {
            const f32 dx = c.x - mech.x;
            const f32 dy = c.y - mech.y;
            const f32 dz = c.z - mech.z;
            if (dx * dx + dy * dy + dz * dz < MERGE_RANGE * MERGE_RANGE) {
                c.merged = 1;
                mech.pilot = static_cast<u8>(i);
                c.x = mech.x;
                c.y = mech.y;
                c.z = mech.z;
                c.vx = 0.0f;
                c.vy = 0.0f;
                c.vz = 0.0f;
            }
        }
    }
}

void eject_pilot(World& next, u32 index) {
    Character& c = next.chars[index];
    c.merged = 0;
    next.mech.pilot = NO_PILOT;
    c.y = next.mech.y + MECH_HEIGHT + 0.2f;  // out through the top hatch
    c.vx = 0.0f;
    c.vy = 3.0f;
    c.vz = 0.0f;
}

}  // namespace sim
