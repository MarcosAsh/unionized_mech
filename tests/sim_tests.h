#pragma once

// Shared declarations for the sim test suites. The tests are split by subject
// across several files, all linked into one `sim_tests` binary, because one
// file of them had grown past the project's size limit.

namespace sim_tests {

// Determinism, purity and the fixed timestep.
void run_core();
// The character controller: mantle, climb, steps, walls, slide, lurch.
void run_movement();
// Shooting, the mech, grenades, reloading, and the balance between them.
void run_combat();
// Map loading and the bot waypoint graph.
void run_map();

}  // namespace sim_tests
