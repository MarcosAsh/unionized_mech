#include "sim_internal.h"
#include "sim_level.h"
#include "sim_math.h"

namespace sim {

namespace {

// Stateless deterministic bits from the world seed, tick, and a stream id.
// PCG-style mix: any change in input scrambles every output bit.
u32 mix(u32 a, u32 b, u32 c) {
    u32 h = a * 0x9E3779B9u + b * 0x85EBCA6Bu + c * 0xC2B2AE35u;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

// Roam destinations spread across the map's zones.
constexpr f32 ROAM_POINTS[][3] = {
    {0.0f, 0.0f, 0.0f},     {24.0f, 0.0f, 30.0f},  {-35.0f, 0.0f, 35.0f},
    {-20.0f, 0.0f, -38.0f}, {0.0f, 0.0f, -90.0f},  {14.0f, 8.0f, 14.0f},
    {-54.0f, 0.0f, 5.0f},   {40.0f, 0.0f, -20.0f},
};
constexpr u32 ROAM_COUNT = sizeof(ROAM_POINTS) / sizeof(ROAM_POINTS[0]);

constexpr f32 BOT_VIEW_RANGE = 70.0f;
constexpr f32 AIM_TOLERANCE = 0.06f;  // radians of error before firing

// Bot decision thresholds, not physics. A bot commits to a wall above this
// speed because a wallrun below it drops immediately and looks like a stumble.
constexpr f32 BOT_WALLRUN_SPEED = 5.0f;

// Grenades are thrown into this band only: closer and the thrower is inside
// their own blast, further and the arc falls short of anything useful.
constexpr f32 BOT_NADE_MIN_D2 = 100.0f;   // 10m
constexpr f32 BOT_NADE_MAX_D2 = 900.0f;   // 30m

// Steer look_dx so yaw eases toward `target_yaw`.
i16 steer(f32 current, f32 target, f32 rate) {
    f32 diff = wrap_angle(target - current);
    if (diff > rate) {
        diff = rate;
    }
    if (diff < -rate) {
        diff = -rate;
    }
    return static_cast<i16>(diff * 400.0f);  // inverse of LOOK_SCALE
}

}  // namespace

InputCmd bot_think(const World& prev, u32 index) {
    InputCmd cmd{};
    cmd.tick = prev.tick;
    const Character& me = prev.chars[index];
    if (me.alive == 0) {
        return cmd;
    }

    // The nearest visible enemy.
    i32 target = -1;
    f32 best_d2 = BOT_VIEW_RANGE * BOT_VIEW_RANGE;
    for (u32 j = 0; j < MAX_PLAYERS; ++j) {
        const Character& other = prev.chars[j];
        if (j == index || other.alive == 0 || other.team == me.team) {
            continue;
        }
        const f32 dx = other.x - me.x;
        const f32 dy = other.y - me.y;
        const f32 dz = other.z - me.z;
        const f32 d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < best_d2 && segment_clear(me.x, me.y + eye_height(me), me.z, other.x,
                                          other.y + eye_height(other), other.z)) {
            best_d2 = d2;
            target = static_cast<i32>(j);
        }
    }

    if (target >= 0) {
        const Character& enemy = prev.chars[target];
        const f32 dx = enemy.x - me.x;
        const f32 dy = (enemy.y + 1.0f) - (me.y + eye_height(me));  // aim at the chest
        const f32 dz = enemy.z - me.z;
        const f32 flat = sim_sqrt(dx * dx + dz * dz);
        // Yaw convention: forward at yaw 0 is -Z, so yaw = atan2(x, -z).
        // cross_y is sin(bearing error): positive when the target is to the
        // right, where yaw must grow. Steering eases rather than solves, which
        // stays deterministic and reads as human.
        f32 desired_yaw = me.yaw;
        if (flat > 0.01f) {
            const f32 fx = sim_sin(me.yaw);
            const f32 fz = -sim_cos(me.yaw);
            const f32 cross_y = fx * (dz / flat) - fz * (dx / flat);
            const f32 dot_f = fx * (dx / flat) + fz * (dz / flat);
            desired_yaw = me.yaw + (dot_f < 0.0f ? (cross_y < 0.0f ? -0.6f : 0.6f)
                                                 : cross_y * 0.8f);
        }
        // Imperfect aim: a wobble re-rolled every ~400ms drags the aim point
        // off the true bearing, so bots miss moving targets the way people do,
        // and the slow steer rate gives the target room to dodge.
        const u32 wob = mix(prev.seed, prev.tick.raw / 25, index ^ 0x33u);
        const f32 yaw_wobble =
            (static_cast<f32>(wob & 255) * (1.0f / 255.0f) - 0.5f) * 0.15f;
        const f32 pitch_wobble =
            (static_cast<f32>(wob >> 8 & 255) * (1.0f / 255.0f) - 0.5f) * 0.06f;
        desired_yaw += yaw_wobble;
        cmd.look_dx = steer(me.yaw, desired_yaw, 0.055f);
        const f32 desired_pitch =
            (flat > 0.01f ? dy / flat * 0.8f : 0.0f) + pitch_wobble;
        cmd.look_dy = steer(me.pitch, desired_pitch, 0.05f);

        // Fire when roughly on the (wobbled) aim, with pauses in the trigger
        // discipline; strafe unpredictably while fighting.
        const f32 aim_err = wrap_angle(desired_yaw - me.yaw);
        if (aim_err > -AIM_TOLERANCE && aim_err < AIM_TOLERANCE && (wob >> 16 & 3) != 0) {
            cmd.buttons |= static_cast<u16>(Button::Fire);
        }
        // An occasional grenade at mid range. Rare on purpose: two per life, and
        // a bot that leads every engagement with one stops reading as a rifleman.
        if (me.grenades > 0 && best_d2 > BOT_NADE_MIN_D2 && best_d2 < BOT_NADE_MAX_D2 &&
            (wob >> 20 & 127) == 0) {
            cmd.buttons |= static_cast<u16>(Button::Grenade);
        }
        const u32 r = mix(prev.seed, prev.tick.raw / 20, index);
        cmd.move_x = static_cast<i8>((r & 3) == 0 ? -1 : ((r & 3) == 1 ? 1 : 0));
        cmd.move_y = static_cast<i8>((r >> 2 & 3) != 0 ? 1 : 0);
        if ((r >> 4 & 15) == 0) {
            cmd.buttons |= static_cast<u16>(Button::Jump);
        }
        return cmd;
    }

    // Nobody in sight is the moment to top up. Reloading only when the magazine
    // runs dry means always reloading in the middle of a fight.
    if (me.ammo < weapon_mag(me) && me.reserve > 0) {
        cmd.buttons |= static_cast<u16>(Button::Reload);
    }

    // No enemy: walk the waypoint graph toward a periodically re-rolled goal
    // node. Maps without a graph fall back to beelining between roam points.
    f32 gx = 0.0f;
    f32 gz = 0.0f;
    bool climb_hint = false;
    if (nav_count() > 0) {
        const u32 goal_node = mix(prev.seed, prev.tick.raw / 600, index) % nav_count();
        f32 hy = 0.0f;
        if (!nav_next_hop(me.x, me.y, me.z, goal_node, &gx, &hy, &gz)) {
            return cmd;  // marooned off the graph; hold position
        }
        climb_hint = hy > me.y + 0.5f;  // the next node is up a ledge
    } else {
        const u32 pick = mix(prev.seed, prev.tick.raw / 600, index) % ROAM_COUNT;
        gx = ROAM_POINTS[pick][0];
        gz = ROAM_POINTS[pick][2];
    }
    const f32 dx = gx - me.x;
    const f32 dz = gz - me.z;
    const f32 flat = sim_sqrt(dx * dx + dz * dz);
    if (flat > 2.0f) {
        const f32 fx = sim_sin(me.yaw);
        const f32 fz = -sim_cos(me.yaw);
        const f32 cross_y = fx * (dz / flat) - fz * (dx / flat);
        const f32 dot_f = fx * (dx / flat) + fz * (dz / flat);
        const f32 desired_yaw =
            me.yaw + (dot_f < 0.0f ? (cross_y < 0.0f ? -0.5f : 0.5f) : cross_y * 0.6f);
        cmd.look_dx = steer(me.yaw, desired_yaw, 0.10f);
        cmd.move_y = 1;

        // Take the walls. A wallrun holds well above sprint speed, so a bot that
        // only ever runs the floor is crossing the level the slow way. Jumping
        // into a wall while already carrying speed is all it takes to start one,
        // and holding forward is what keeps it going.
        f32 wall_nx = 0.0f;
        f32 wall_nz = 0.0f;
        f32 wall_top = 0.0f;
        const bool wall_near =
            find_wall(me.x, me.y, me.z, HULL_HEIGHT, level_boxes(), &wall_nx, &wall_nz, &wall_top);
        // Being near a wall is not enough: a wallrun needs speed ALONG the face,
        // and a bot charging straight at one just bounces off. Split the
        // velocity against the wall normal and require the same thing the
        // movement code does, or the jump is wasted.
        const f32 speed2 = me.vx * me.vx + me.vz * me.vz;
        const f32 into = me.vx * wall_nx + me.vz * wall_nz;  // positive is away
        const f32 along2 = speed2 - into * into;
        const bool mount_wall = wall_near && me.on_ground != 0 &&
                                along2 > BOT_WALLRUN_SPEED * BOT_WALLRUN_SPEED && into < 1.0f;

        const u32 r = mix(prev.seed, prev.tick.raw / 30, index ^ 0x55u);
        if (mount_wall || (r & 31) == 0 || climb_hint ||
            (me.on_ground != 0 && me.vx * me.vx + me.vz * me.vz < 1.0f &&
             prev.tick.raw % 90 == (index * 9) % 90)) {
            cmd.buttons |= static_cast<u16>(Button::Jump);  // wallrun, hop, climb, or unstick
        }
    }
    return cmd;
}

}  // namespace sim
