#include "sim_internal.h"
#include "sim_level.h"
#include "sim_math.h"

namespace sim {

namespace {

// Grenade tuning, in metres and seconds. Like the movement knobs these are all
// compile-time constants, so the arithmetic below is bit-identical everywhere.
constexpr f32 THROW_SPEED = 16.0f;   // launch speed along the aim
constexpr f32 THROW_LIFT = 2.5f;     // upward kick, so a level throw still arcs
constexpr f32 THROW_FORWARD = 0.6f;  // spawn this far ahead, clear of the thrower
constexpr u16 FUSE_TICKS = 150;      // two and a half seconds
constexpr f32 BOUNCE = 0.35f;        // fraction of speed kept across a bounce
constexpr f32 REST_SPEED = 0.6f;     // below this a bounce just stops it
constexpr f32 BLAST_RADIUS = 5.0f;   // metres
constexpr i16 BLAST_DAMAGE = 110;    // at the centre, tapering to nothing at the edge

/// True when the point is strictly inside `b`. A grenade is treated as a point:
/// it is small next to the hull the level was built around, and a point keeps
/// the bounce resolution to one comparison per axis.
[[nodiscard]] bool point_in(f32 x, f32 y, f32 z, const Aabb& b) {
    return x > b.min_x && x < b.max_x && y > b.min_y && y < b.max_y && z > b.min_z &&
           z < b.max_z;
}

/// Everything within the blast takes damage on a linear taper from the centre.
/// Enemies only, matching hitscan: friendly fire would need a scoring rule this
/// game has not decided on yet.
void detonate(World& next, const Grenade& g) {
    const Character& owner = next.chars[g.owner];
    for (u32 i = 0; i < MAX_PLAYERS; ++i) {
        Character& target = next.chars[i];
        if (target.alive == 0 || target.team == owner.team) {
            continue;
        }
        // Measure to the middle of the target rather than its feet, so a
        // grenade landing at head height is not judged to have missed.
        const f32 half = (target.merged != 0 ? MECH_HEIGHT : HULL_HEIGHT) * 0.5f;
        const f32 dx = target.x - g.x;
        const f32 dy = (target.y + half) - g.y;
        const f32 dz = target.z - g.z;
        const f32 dist = sim_sqrt(dx * dx + dy * dy + dz * dz);
        if (dist >= BLAST_RADIUS) {
            continue;
        }
        const f32 falloff = (BLAST_RADIUS - dist) / BLAST_RADIUS;
        const i16 damage = static_cast<i16>(static_cast<f32>(BLAST_DAMAGE) * falloff);
        apply_damage(next, g.owner, i, damage);
    }
}

/// Advance one axis and bounce off whatever it ends up inside, the same
/// move-and-slide shape the character controller uses, one axis at a time.
void move_axis(Grenade& g, u32 axis, core::Span<const Aabb> boxes) {
    f32& pos = axis == 0 ? g.x : (axis == 1 ? g.y : g.z);
    f32& vel = axis == 0 ? g.vx : (axis == 1 ? g.vy : g.vz);
    pos += vel * SIM_DT;
    for (u64 i = 0; i < boxes.size(); ++i) {
        const Aabb& b = boxes[i];
        if (!point_in(g.x, g.y, g.z, b)) {
            continue;
        }
        const f32 lo = axis == 0 ? b.min_x : (axis == 1 ? b.min_y : b.min_z);
        const f32 hi = axis == 0 ? b.max_x : (axis == 1 ? b.max_y : b.max_z);
        pos = vel > 0.0f ? lo : hi;
        vel = -vel * BOUNCE;
        if (vel < REST_SPEED && vel > -REST_SPEED) {
            vel = 0.0f;
        }
    }
}

}  // namespace

void step_grenades(World& next, const InputCmd cmds[MAX_PLAYERS]) {
    // Throws first, so a grenade thrown this tick starts its fuse this tick.
    for (u32 i = 0; i < MAX_PLAYERS; ++i) {
        Character& c = next.chars[i];
        const bool down = button_down(cmds[i].buttons, Button::Grenade);
        const bool pressed = down && c.grenade_was_down == 0;
        c.grenade_was_down = down ? 1 : 0;
        if (!pressed || c.alive == 0 || c.merged != 0 || c.grenades == 0) {
            continue;
        }
        // First free slot, or nothing: a full sky drops the throw rather than
        // evicting someone else's grenade.
        u32 slot = MAX_GRENADES;
        for (u32 s = 0; s < MAX_GRENADES; ++s) {
            if (next.grenades[s].active == 0) {
                slot = s;
                break;
            }
        }
        if (slot == MAX_GRENADES) {
            continue;
        }
        const f32 cp = sim_cos(c.pitch);
        const f32 dx = sim_sin(c.yaw) * cp;
        const f32 dy = sim_sin(c.pitch);
        const f32 dz = -sim_cos(c.yaw) * cp;

        Grenade& g = next.grenades[slot];
        g = Grenade{};
        g.active = 1;
        g.owner = static_cast<u8>(i);
        g.fuse_ticks = FUSE_TICKS;
        g.x = c.x + dx * THROW_FORWARD;
        g.y = c.y + eye_height(c) + dy * THROW_FORWARD;
        g.z = c.z + dz * THROW_FORWARD;
        // The thrower's own momentum carries into the throw, so a grenade
        // lobbed while sprinting or wallrunning goes where it looks like it
        // should rather than dropping behind you.
        g.vx = c.vx + dx * THROW_SPEED;
        g.vy = c.vy + dy * THROW_SPEED + THROW_LIFT;
        g.vz = c.vz + dz * THROW_SPEED;
        c.grenades -= 1;
    }

    const core::Span<const Aabb> boxes = level_boxes();
    for (u32 s = 0; s < MAX_GRENADES; ++s) {
        Grenade& g = next.grenades[s];
        if (g.active == 0) {
            continue;
        }
        g.vy -= GRAVITY * SIM_DT;
        move_axis(g, 0, boxes);
        move_axis(g, 1, boxes);
        move_axis(g, 2, boxes);
        if (g.y <= 0.0f) {  // the implicit floor
            g.y = 0.0f;
            g.vy = -g.vy * BOUNCE;
            if (g.vy < REST_SPEED) {
                g.vy = 0.0f;
            }
        }

        if (g.fuse_ticks > 0) {
            g.fuse_ticks -= 1;
        }
        if (g.fuse_ticks == 0) {
            detonate(next, g);
            g = Grenade{};
        }
    }
}

}  // namespace sim
