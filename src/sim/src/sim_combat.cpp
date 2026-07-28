#include "sim_internal.h"
#include "sim_level.h"
#include "sim_math.h"

namespace sim {

namespace {

// The weapon table. Distances in metres, everything else in ticks at 60Hz.
// Aimed fire is a zero-width ray, which is what the hitscan below already
// models, so cadence, falloff, and ammunition are the whole of the tuning.
const Weapon WEAPONS[] = {
    // Carbine. The pilot's rifle: fast, four rounds to a kill up close.
    //  near   far     range   dmg near/far/vfar  reload  empty  mag  spare  cd
    {38.10f, 50.80f, 200.0f, 25, 17, 12, 132, 175, 24, 240, 4},
    // Chassis cannon. Slow and heavy, flat damage at any range, never dry.
    {200.0f, 200.0f, 200.0f, 60, 60, 60, 0, 0, 0, 0, 18},
};

constexpr u8 WEAPON_COUNT = static_cast<u8>(sizeof(WEAPONS) / sizeof(WEAPONS[0]));

/// What `w` lands at `dist` metres. Stepped rather than interpolated: a weapon
/// is defined as three bands, and reading a curve between them would invent
/// precision the design does not have.
[[nodiscard]] i16 shot_damage(const Weapon& w, f32 dist) {
    if (dist <= w.near_dist) {
        return w.damage_near;
    }
    return dist <= w.far_dist ? w.damage_far : w.damage_very_far;
}

// Ray versus box, the slab test. Returns the entry distance through `t` when
// the ray hits within [0, max_t].
bool ray_vs_aabb(f32 ox, f32 oy, f32 oz, f32 dx, f32 dy, f32 dz, const Aabb& b, f32 max_t,
                 f32* t) {
    f32 t_min = 0.0f;
    f32 t_max = max_t;
    const f32 origin[3] = {ox, oy, oz};
    const f32 dir[3] = {dx, dy, dz};
    const f32 lo[3] = {b.min_x, b.min_y, b.min_z};
    const f32 hi[3] = {b.max_x, b.max_y, b.max_z};
    for (u32 axis = 0; axis < 3; ++axis) {
        if (dir[axis] > -1e-8f && dir[axis] < 1e-8f) {
            if (origin[axis] < lo[axis] || origin[axis] > hi[axis]) {
                return false;
            }
            continue;
        }
        const f32 inv = 1.0f / dir[axis];
        f32 t0 = (lo[axis] - origin[axis]) * inv;
        f32 t1 = (hi[axis] - origin[axis]) * inv;
        if (t0 > t1) {
            const f32 swap = t0;
            t0 = t1;
            t1 = swap;
        }
        if (t0 > t_min) {
            t_min = t0;
        }
        if (t1 < t_max) {
            t_max = t1;
        }
        if (t_min > t_max) {
            return false;
        }
    }
    *t = t_min;
    return true;
}

// A character's hull as a box; a merged robot presents the mech's hull.
Aabb character_hull(const Character& c) {
    if (c.merged != 0) {
        return Aabb{c.x - MECH_HALF_WIDTH, c.y, c.z - MECH_HALF_WIDTH,
                    c.x + MECH_HALF_WIDTH, c.y + MECH_HEIGHT, c.z + MECH_HALF_WIDTH};
    }
    const f32 h = c.ducked != 0 ? DUCK_HEIGHT : HULL_HEIGHT;
    return Aabb{c.x - HULL_HALF_WIDTH, c.y, c.z - HULL_HALF_WIDTH,
                c.x + HULL_HALF_WIDTH, c.y + h, c.z + HULL_HALF_WIDTH};
}

}  // namespace

const Weapon& weapon_def(u8 index) {
    return WEAPONS[index < WEAPON_COUNT ? index : 0];
}

const Weapon& held_weapon(const Character& c) {
    return c.merged != 0 ? WEAPONS[WEAPON_CHASSIS] : weapon_def(c.weapon);
}

void apply_damage(World& next, u32 attacker, u32 target_index, i16 damage) {
    Character& target = next.chars[target_index];
    if (target.alive == 0 || damage <= 0) {
        return;
    }
    target.hurt_age = 0;
    if (target.merged != 0) {
        // Damage to a merged robot lands on the chassis, less what the armour
        // turns away. Rounded up so no hit ever does nothing at all. The pilot
        // evacuates intact when the chassis dies; wrecking it scores.
        const i32 through = (static_cast<i32>(damage) * MECH_ARMOUR_PERCENT + 99) / 100;
        next.mech.health = static_cast<i16>(next.mech.health - static_cast<i16>(through));
        if (next.mech.health <= 0) {
            next.mech.alive = 0;
            next.mech.respawn_ticks = MECH_REBUILD_TICKS;
            eject_pilot(next, target_index);
            next.chars[attacker].kills += 1;
        }
        return;
    }
    target.health = static_cast<i16>(target.health - damage);
    if (target.health <= 0) {
        target.alive = 0;
        target.respawn_ticks = RESPAWN_TICKS;
        next.chars[attacker].kills += 1;
    }
}

u16 weapon_mag(const Character& c) { return held_weapon(c).mag; }

void rearm(Character& c) {
    const Weapon& w = weapon_def(c.weapon);
    c.ammo = w.mag;
    c.reserve = w.reserve;
    c.reload_ticks = 0;
    c.grenades = GRENADES_PER_LIFE;
}

bool segment_clear(f32 ax, f32 ay, f32 az, f32 bx, f32 by, f32 bz) {
    const f32 dx = bx - ax;
    const f32 dy = by - ay;
    const f32 dz = bz - az;
    const f32 len = sim_sqrt(dx * dx + dy * dy + dz * dz);
    if (len <= 0.0f) {
        return true;
    }
    const core::Span<const Aabb> boxes = level_boxes();
    f32 t = 0.0f;
    for (u64 i = 0; i < boxes.size(); ++i) {
        if (ray_vs_aabb(ax, ay, az, dx / len, dy / len, dz / len, boxes[i], len, &t)) {
            return false;
        }
    }
    return true;
}

void resolve_combat(World& next, const InputCmd cmds[MAX_PLAYERS]) {
    // Shots resolve against the post-move state, in slot order, which keeps
    // the outcome deterministic and fair enough at 60Hz.
    for (u32 i = 0; i < MAX_PLAYERS; ++i) {
        Character& shooter = next.chars[i];
        const Weapon& weapon = held_weapon(shooter);
        const bool wants_fire = button_down(cmds[i].buttons, Button::Fire);
        const bool reload_down = button_down(cmds[i].buttons, Button::Reload);
        const bool reload_pressed = reload_down && shooter.reload_was_down == 0;
        shooter.reload_was_down = reload_down ? 1 : 0;

        // Ammunition, for weapons that carry any. A reload runs on its own
        // clock and blocks firing until it lands.
        if (weapon.mag > 0 && shooter.alive != 0) {
            if (shooter.reload_ticks > 0) {
                shooter.reload_ticks -= 1;
                if (shooter.reload_ticks == 0) {
                    const u16 room = static_cast<u16>(weapon.mag - shooter.ammo);
                    const u16 taken = room < shooter.reserve ? room : shooter.reserve;
                    shooter.ammo = static_cast<u16>(shooter.ammo + taken);
                    shooter.reserve = static_cast<u16>(shooter.reserve - taken);
                }
            } else if (shooter.reserve > 0 && shooter.ammo < weapon.mag &&
                       (reload_pressed || (wants_fire && shooter.ammo == 0))) {
                // Asked for, or forced by pulling the trigger on an empty
                // magazine. Bots never press reload, so the second case is what
                // keeps them shooting.
                shooter.reload_ticks =
                    shooter.ammo == 0 ? weapon.reload_empty_ticks : weapon.reload_ticks;
            }
        }

        if (!wants_fire || shooter.alive == 0 || shooter.fire_cooldown > 0 ||
            shooter.reload_ticks > 0 || (weapon.mag > 0 && shooter.ammo == 0)) {
            continue;
        }
        if (weapon.mag > 0) {
            shooter.ammo -= 1;
        }
        shooter.fire_cooldown = static_cast<u8>(weapon.cooldown_ticks);

        const f32 cp = sim_cos(shooter.pitch);
        const f32 dx = sim_sin(shooter.yaw) * cp;
        const f32 dy = sim_sin(shooter.pitch);
        const f32 dz = -sim_cos(shooter.yaw) * cp;
        const f32 ox = shooter.x;
        const f32 oy = shooter.y + eye_height(shooter);
        const f32 oz = shooter.z;

        // The wall the shot stops at.
        f32 best_t = weapon.range;
        const core::Span<const Aabb> boxes = level_boxes();
        f32 t = 0.0f;
        for (u64 b = 0; b < boxes.size(); ++b) {
            if (ray_vs_aabb(ox, oy, oz, dx, dy, dz, boxes[b], best_t, &t) && t < best_t) {
                best_t = t;
            }
        }

        // The nearest enemy in front of that wall.
        i32 hit = -1;
        for (u32 j = 0; j < MAX_PLAYERS; ++j) {
            const Character& target = next.chars[j];
            if (j == i || target.alive == 0 || target.team == shooter.team) {
                continue;
            }
            if (ray_vs_aabb(ox, oy, oz, dx, dy, dz, character_hull(target), best_t, &t) &&
                t < best_t) {
                best_t = t;
                hit = static_cast<i32>(j);
            }
        }
        shooter.last_shot_t = best_t;
        shooter.shot_x = ox;
        shooter.shot_y = oy;
        shooter.shot_z = oz;
        shooter.shot_yaw = shooter.yaw;
        shooter.shot_pitch = shooter.pitch;
        shooter.shot_age = 0;
        shooter.shot_hit = hit >= 0 ? 1 : 0;
        if (hit >= 0) {
            // Damage falls off with the distance the ray just travelled; a
            // weapon with equal bands, like the cannon, simply reads flat.
            apply_damage(next, i, static_cast<u32>(hit), shot_damage(weapon, best_t));
        }
    }

    // Respawns: dead characters count down, then return at a team spawn.
    for (u32 i = 0; i < MAX_PLAYERS; ++i) {
        Character& c = next.chars[i];
        if (c.alive != 0) {
            continue;
        }
        if (c.respawn_ticks > 0) {
            c.respawn_ticks -= 1;
            continue;
        }
        const Spawn spawn = team_spawn(c.team, i);
        const u8 team = c.team;
        const u16 kills = c.kills;
        c = Character{};
        c.team = team;
        c.kills = kills;
        c.x = spawn.x;
        c.y = spawn.y;
        c.z = spawn.z;
        c.yaw = spawn.yaw;
        rearm(c);  // nobody comes back from a respawn holding an empty gun
    }
}

}  // namespace sim
