#pragma once

// Internal to the sim module. The pieces simulate() composes each tick.

#include "sim/sim.h"

namespace sim {

/// Mech chassis dimensions and combat numbers, shared by movement, combat,
/// and the union logic. The render camera mirrors MECH_EYE.
constexpr f32 MECH_HALF_WIDTH = 1.6f;
constexpr f32 MECH_HEIGHT = 4.4f;
constexpr f32 MECH_EYE = 3.9f;
constexpr u16 MECH_REBUILD_TICKS = 1800;  // thirty seconds
constexpr u16 RESPAWN_TICKS = 180;        // three seconds

/// Eye height above the feet, where rays start and bots look from. Must match
/// the render camera exactly, or shots land away from the crosshair.
[[nodiscard]] inline f32 eye_height(const Character& c) {
    if (c.merged != 0) {
        return MECH_EYE;
    }
    return c.ducked != 0 ? 0.9f : 1.7f;
}

/// One weapon's tuning, held as data. Adding a gun is a row in the table in
/// sim_combat.cpp rather than another branch through the firing code, and the
/// chassis cannon is a row like any other instead of a special case.
struct Weapon {
    f32 near_dist;            ///< Metres of full damage.
    f32 far_dist;             ///< Metres before the last step down.
    f32 range;                ///< How far the ray reaches at all.
    i16 damage_near;          ///< Damage inside near_dist.
    i16 damage_far;           ///< Damage past near_dist.
    i16 damage_very_far;      ///< Damage past far_dist.
    u16 reload_ticks;         ///< Reload with a round still chambered.
    u16 reload_empty_ticks;   ///< Reload from dry, which takes longer.
    u16 mag;                  ///< Rounds per magazine. 0 means it never runs out.
    u16 reserve;              ///< Spare rounds carried at spawn.
    u16 cooldown_ticks;       ///< Ticks between shots.
};

constexpr u8 WEAPON_CARBINE = 0;
constexpr u8 WEAPON_CHASSIS = 1;

/// The weapon `index` names, clamped to the table.
[[nodiscard]] const Weapon& weapon_def(u8 index);

/// The weapon a character is actually holding: merging swaps the carbine for
/// the chassis cannon without disturbing the stowed weapon index.
[[nodiscard]] const Weapon& held_weapon(const Character& c);

/// Fill a character's magazine and reserve from its weapon. Used at match start
/// and on every respawn, so nobody comes back dry.
void rearm(Character& c);

/// Advance one character by one tick of movement from `cmd`. Pure: reads
/// `prev_c`, writes `c` (already copied from prev).
void step_character(Character& c, const Character& prev_c, const InputCmd& cmd);

/// Generate the tick's input for bot `index` from the previous world state.
/// Deterministic: all randomness derives from the world seed and tick.
[[nodiscard]] InputCmd bot_think(const World& prev, u32 index);

/// Resolve this tick's shots, reloads, damage, deaths, and respawns.
void resolve_combat(World& next, const InputCmd cmds[MAX_PLAYERS]);

/// Merge and eject transitions, mech movement under its pilot, and the
/// destroyed-chassis rebuild countdown. Runs after the characters step.
void step_union(World& next, const InputCmd cmds[MAX_PLAYERS]);

/// Pop the merged pilot of `index` out of the top hatch, freeing the mech.
void eject_pilot(World& next, u32 index);

}  // namespace sim
