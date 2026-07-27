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

/// Advance one character by one tick of movement from `cmd`. Pure: reads
/// `prev_c`, writes `c` (already copied from prev).
void step_character(Character& c, const Character& prev_c, const InputCmd& cmd);

/// Generate the tick's input for bot `index` from the previous world state.
/// Deterministic: all randomness derives from the world seed and tick.
[[nodiscard]] InputCmd bot_think(const World& prev, u32 index);

/// Resolve this tick's shots, damage, deaths, and respawns. `fired[i]` is
/// whether character i pressed fire this tick.
void resolve_combat(World& next, const bool fired[MAX_PLAYERS]);

/// Merge and eject transitions, mech movement under its pilot, and the
/// destroyed-chassis rebuild countdown. Runs after the characters step.
void step_union(World& next, const InputCmd cmds[MAX_PLAYERS]);

/// Pop the merged pilot of `index` out of the top hatch, freeing the mech.
void eject_pilot(World& next, u32 index);

}  // namespace sim
