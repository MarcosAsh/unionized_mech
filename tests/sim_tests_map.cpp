// Map loading and the bot waypoint graph.

#include "core/arena.h"
#include "core/assert.h"
#include "core/file.h"
#include "core/log.h"
#include "core/types.h"
#include "sim/sim.h"
#include "sim_tests.h"

#include <cmath>
#include <cstdlib>

using namespace sim;

namespace sim_tests {

// A written map file parses back into the same boxes and spawn, and the
// visible prefix rule holds regardless of line order.
static void test_map_round_trip() {
    const char* map_text =
        "# test map\n"
        "spawn 1 2 3 0.5\n"
        "cbox -1 0 -1 1 4 1\n"
        "box 5 0 5 9 2.5 9\n"
        "box -20 0 -20 -18 6 -18\n"
        "node 1 0 3\n"
        "node 9 0 3\n";
    core::Arena arena = core::Arena::with_capacity(1u << 20);
    const char* path = "sim_tests_map.tmp";
    ASSERT(core::write_entire_file(path,
                                   core::Span<const u8>(
                                       reinterpret_cast<const u8*>(map_text),
                                       __builtin_strlen(map_text)))
               .is_ok());
    ASSERT(load_level(arena, path).is_ok());

    ASSERT(visible_boxes().size() == 2);
    ASSERT(level_boxes().size() == 3);
    ASSERT(visible_boxes()[0].max_y == 2.5f);
    ASSERT(level_boxes()[2].max_y == 4.0f);  // the cbox sits after the visibles
    ASSERT(level_spawn().x == 1.0f);
    ASSERT(level_spawn().yaw == 0.5f);
    ASSERT(nav_count() == 2);
    ASSERT(load_level(arena, "missing.umap").is_err());
    // The failed load keeps the previous level, and its nav graph, active.
    ASSERT(level_boxes().size() == 3);
    ASSERT(nav_count() == 2);
}

// The waypoint graph links reachable neighbours and routes around gaps: three
// nodes in a line link pairwise (the ends are out of range of each other), so
// the path from one end to the other hops through the middle. A wall between
// two nodes kills their link and the route.
static void test_nav_pathing() {
    core::Arena arena = core::Arena::with_capacity(1u << 20);
    const char* path = "sim_tests_nav.tmp";

    const char* line_map =
        "spawn 0 0 0 0\n"
        "node 0 0 0\n"
        "node 14 0 0\n"
        "node 28 0 0\n";
    ASSERT(core::write_entire_file(path,
                                   core::Span<const u8>(
                                       reinterpret_cast<const u8*>(line_map),
                                       __builtin_strlen(line_map)))
               .is_ok());
    ASSERT(load_level(arena, path).is_ok());
    ASSERT(nav_count() == 3);

    // From the left end toward the right end, the next hop is the middle.
    f32 hx = 0.0f;
    f32 hy = 0.0f;
    f32 hz = 0.0f;
    ASSERT(nav_next_hop(-2.0f, 0.0f, 0.0f, 2, &hx, &hy, &hz));
    ASSERT(hx == 14.0f);
    // From beside the middle node, the next hop is the goal itself.
    ASSERT(nav_next_hop(15.0f, 0.0f, 0.0f, 2, &hx, &hy, &hz));
    ASSERT(hx == 28.0f);

    const char* wall_map =
        "spawn 0 0 0 0\n"
        "node 0 0 0\n"
        "node 14 0 0\n"
        "cbox 6 0 -8 8 6 8\n";
    ASSERT(core::write_entire_file(path,
                                   core::Span<const u8>(
                                       reinterpret_cast<const u8*>(wall_map),
                                       __builtin_strlen(wall_map)))
               .is_ok());
    ASSERT(load_level(arena, path).is_ok());
    ASSERT(!nav_next_hop(0.0f, 0.0f, 0.0f, 1, &hx, &hy, &hz));
}

// Bots have to go and find each other. Roaming at random, contact was an
// accident on a 160m map and a match took 217s. Hunting puts it at 135s.
static void test_bots_find_each_other() {
    core::Arena arena = core::Arena::with_capacity(1u << 20);
    ASSERT(load_level(arena, MAP_PATH).is_ok());

    for (u32 seed = 0; seed < 4; ++seed) {
        World w{};
        init_match(w);
        w.seed = 0x4d454348u + seed * 0x9e3779b9u;
        w.chars[0].x = 2000.0f;  // the player sits this one out
        w.chars[0].z = 2000.0f;
        u32 decided = 0;
        for (u32 i = 0; i < 20000; ++i) {
            InputCmd c{};
            c.tick = TickId{i};
            World next{};
            simulate(w, c, next);
            w = next;
            if (w.winner != 0) {
                decided = i;
                break;
            }
        }
        // Hunting lands every seed in 7.5k-9.2k ticks; random roaming averaged
        // 13k. Anything past 11k means they have stopped looking.
        ASSERT(decided > 0);
        ASSERT(decided < 11000);
    }
}

// Every waypoint in the shipping map has to be reachable from both team spawns.
// Bots path by waypoint, so a wall dropped between two nodes silently strands
// them somewhere they cannot route out of. Level geometry changes land here.
static void test_map_nav_fully_connected() {
    core::Arena arena = core::Arena::with_capacity(1u << 20);
    ASSERT(load_level(arena, MAP_PATH).is_ok());
    const u32 nodes = nav_count();
    ASSERT(nodes > 0);
    for (u32 team = 0; team < 2; ++team) {
        const Spawn start = team_spawn(team, 0);
        for (u32 goal = 0; goal < nodes; ++goal) {
            f32 hx = 0.0f;
            f32 hy = 0.0f;
            f32 hz = 0.0f;
            ASSERT(nav_next_hop(start.x, start.y, start.z, goal, &hx, &hy, &hz));
        }
    }
}

void run_map() {
    test_map_round_trip();
    test_nav_pathing();
    test_bots_find_each_other();
    test_map_nav_fully_connected();
}

}  // namespace sim_tests
