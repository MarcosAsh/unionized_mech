// Determinism and purity tests for the sim module. Checks are plain ASSERTs.

#include "core/assert.h"
#include "core/file.h"

#include <cmath>
#include "core/log.h"
#include "core/types.h"
#include "sim/sim.h"

using namespace sim;

// Same deterministic sequence the headless runner uses.
static InputCmd scripted(u32 i) {
    InputCmd c{};
    c.tick = TickId{i};
    c.move_x = static_cast<i8>((i % 4 < 2) ? 1 : -1);
    c.move_y = static_cast<i8>((i % 8 < 4) ? 1 : 0);
    c.look_dx = static_cast<i16>(static_cast<i32>(i % 16) - 8);
    c.look_dy = static_cast<i16>(static_cast<i32>((i / 2) % 8) - 4);
    c.buttons = (i % 30u == 0u) ? static_cast<u16>(Button::Jump) : static_cast<u16>(0);
    return c;
}

static void run(u32 n, u64* hashes) {
    World w{};
    for (u32 i = 0; i < n; ++i) {
        World next{};
        simulate(w, scripted(i), next);
        w = next;
        hashes[i] = hash(w);
    }
}

// Replaying the same input tape twice must produce identical per-tick hashes.
static void test_replay_twice_identical() {
    constexpr u32 N = 600;
    static u64 first[N];
    static u64 second[N];
    run(N, first);
    run(N, second);
    for (u32 i = 0; i < N; ++i) {
        ASSERT(first[i] == second[i]);
    }
}

// simulate must not touch prev, and identical inputs must give identical output.
static void test_simulate_is_pure() {
    World w{};
    w.chars[0].x = 3.0f;
    w.chars[0].vx = 1.0f;
    const InputCmd cmd = scripted(7);

    World a{};
    World b{};
    simulate(w, cmd, a);
    simulate(w, cmd, b);

    ASSERT(hash(a) == hash(b));
    ASSERT(w.chars[0].x == 3.0f);
    ASSERT(w.chars[0].vx == 1.0f);
}

static void test_tick_advances() {
    World w{};
    World next{};
    simulate(w, scripted(0), next);
    ASSERT(next.tick == TickId{1});
}

// Acceptance check 6: a stall runs the right number of ticks (capping does not
// lose time), and a stream of tiny frames sums to the right tick count.
static void test_fixed_timestep() {
    const f64 dt = static_cast<f64>(SIM_DT);
    const u32 expected = static_cast<u32>(0.2 / dt);
    ASSERT(expected > 8);  // a 200ms stall exceeds the per-frame cap

    // Capped path drains over several frames to the same total.
    FixedTimestep capped;
    u32 total = capped.advance(0.2, 8);
    ASSERT(total == 8);
    while (total < expected) {
        total += capped.advance(0.0, 8);
    }
    ASSERT(total == expected);

    // Uncapped path runs it all at once.
    FixedTimestep uncapped;
    ASSERT(uncapped.advance(0.2, 1000) == expected);

    // A second of tiny frames is exactly one second of ticks.
    FixedTimestep tiny;
    u32 count = 0;
    for (u32 i = 0; i < 1000; ++i) {
        count += tiny.advance(0.001, 8);
    }
    ASSERT(count == static_cast<u32>(1.0 / dt));
}

// Walking into a chest-high ledge vaults it instead of stopping dead. The
// level's mantle stairs put a 1m step at z=-36 in front of a spawn at z=-34.
static void test_mantle_vaults_ledge() {
    World w{};
    w.chars[0].x = -28.0f;
    w.chars[0].z = -34.0f;
    bool topped = false;
    for (u32 i = 0; i < 180; ++i) {
        InputCmd c{};
        c.tick = TickId{i};
        c.move_y = 1;  // yaw 0 faces -Z, straight at the step
        World next{};
        simulate(w, c, next);
        w = next;
        if (w.chars[0].on_ground != 0 && w.chars[0].y == 1.0f) {
            topped = true;
        }
    }
    ASSERT(topped);
}

// Holding jump against a 3m wall climbs it. Too tall for jump plus mantle
// alone (jump apex 1.2m leaves the lip 1.8m away, past mantle reach), so
// reaching the top proves the ledge climb works.
static void test_ledge_climb() {
    World w{};
    w.chars[0].x = -18.0f;
    w.chars[0].z = -34.0f;  // south of the 3m mantle stair at z=-36
    bool topped = false;
    for (u32 i = 0; i < 300; ++i) {
        InputCmd c{};
        c.tick = TickId{i};
        c.move_y = 1;  // yaw 0 faces -Z, straight at the wall
        c.buttons = static_cast<u16>(Button::Jump);
        World next{};
        simulate(w, c, next);
        w = next;
        if (w.chars[0].on_ground != 0 && w.chars[0].y == 3.0f) {
            topped = true;
        }
    }
    ASSERT(topped);
}

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

// A tape written to disk and loaded back must replay to the identical hash.
static void test_tape_round_trip() {
    constexpr u32 N = 200;
    core::Arena arena = core::Arena::with_capacity(1u << 20);

    const core::Span<InputCmd> cmds = arena.alloc_n<InputCmd>(N);
    for (u32 i = 0; i < N; ++i) {
        cmds[i] = scripted(i);
    }
    const core::Span<const InputCmd> view(cmds.data(), cmds.size());

    const char* path = "sim_tests_tape.tmp";
    ASSERT(tape_save(path, view).is_ok());
    core::Result<core::Span<const InputCmd>, const char*> loaded = tape_load(arena, path);
    ASSERT(loaded.is_ok());
    const core::Span<const InputCmd> replay = loaded.value();
    ASSERT(replay.size() == N);

    World a{};
    World b{};
    for (u32 i = 0; i < N; ++i) {
        World next{};
        simulate(a, view[i], next);
        a = next;
        World next_b{};
        simulate(b, replay[i], next_b);
        b = next_b;
    }
    ASSERT(hash(a) == hash(b));
}

// Firing straight at an enemy damages and eventually kills it: the whole
// hitscan chain proven mechanically.
static void test_hitscan_kills() {
    World w{};
    // Only two characters matter: the shooter at the origin aiming -Z, and an
    // enemy 10m ahead. Everyone else is parked far away and on the shooter's
    // team so they neither block nor participate.
    for (u32 i = 0; i < MAX_PLAYERS; ++i) {
        w.chars[i].x = 500.0f;
        w.chars[i].z = 500.0f;
        w.chars[i].team = 0;
    }
    w.chars[0].x = 30.0f;
    w.chars[0].z = -60.0f;
    w.chars[0].yaw = 0.0f;  // facing -Z
    w.chars[1].x = 30.0f;
    w.chars[1].z = -70.0f;
    w.chars[1].team = 1;

    const i16 start_health = w.chars[1].health;
    InputCmd fire{};
    fire.buttons = static_cast<u16>(Button::Fire);

    World next{};
    simulate(w, fire, next);
    ASSERT(next.chars[1].health == start_health - 25);
    ASSERT(next.chars[0].shot_age == 0);
    ASSERT(next.chars[0].shot_hit == 1);

    // Keep firing while tracking the target, which is a live bot that strafes
    // and fights back. Four hits kill, credit the shooter, start the respawn.
    World cur = next;
    for (u32 i = 0; i < 300 && cur.chars[1].alive != 0; ++i) {
        const Character& me = cur.chars[0];
        const Character& tgt = cur.chars[1];
        const f32 dx = tgt.x - me.x;
        const f32 dz = tgt.z - me.z;
        f32 want_yaw = std::atan2(dx, -dz);
        f32 diff = want_yaw - me.yaw;
        while (diff > 3.14159265f) {
            diff -= 6.2831853f;
        }
        while (diff < -3.14159265f) {
            diff += 6.2831853f;
        }
        f32 turn = diff / 0.0025f;  // inverse of the sim's LOOK_SCALE
        if (turn > 3000.0f) {
            turn = 3000.0f;
        }
        if (turn < -3000.0f) {
            turn = -3000.0f;
        }
        InputCmd track = fire;
        track.look_dx = static_cast<i16>(turn);
        const f32 flat = std::sqrt(dx * dx + dz * dz);
        const f32 dy = (tgt.y + 0.9f) - (me.y + 1.7f);
        const f32 want_pitch = flat > 0.1f ? std::atan2(dy, flat) : 0.0f;
        f32 pdiff = (want_pitch - me.pitch) / 0.0025f;
        if (pdiff > 3000.0f) {
            pdiff = 3000.0f;
        }
        if (pdiff < -3000.0f) {
            pdiff = -3000.0f;
        }
        track.look_dy = static_cast<i16>(pdiff);
        World n{};
        simulate(cur, track, n);
        cur = n;
    }
    ASSERT(cur.chars[1].alive == 0);
    ASSERT(cur.chars[0].kills == 1);
}

// A bot spots an enemy it starts facing away from, turns to it, and shoots:
// the whole see-aim-fire chain proven mechanically. Guards the steering sign,
// which once pointed every bot away from its target.
static void test_bot_fights_back() {
    World w{};
    for (u32 i = 0; i < MAX_PLAYERS; ++i) {
        w.chars[i].x = 500.0f;
        w.chars[i].z = 500.0f;
        w.chars[i].team = 0;
    }
    w.chars[0].x = 0.0f;
    w.chars[0].z = 0.0f;  // the stationary target
    w.chars[1].x = 6.0f;
    w.chars[1].z = 8.0f;
    w.chars[1].team = 1;
    w.chars[1].yaw = 2.5f;  // facing well away from the target

    World cur = w;
    for (u32 i = 0; i < 240 && cur.chars[0].health == 100; ++i) {
        InputCmd idle{};
        idle.tick = cur.tick;
        World next{};
        simulate(cur, idle, next);
        cur = next;
    }
    ASSERT(cur.chars[0].health < 100);
}

int main() {
    test_replay_twice_identical();
    test_simulate_is_pure();
    test_tick_advances();
    test_fixed_timestep();
    test_mantle_vaults_ledge();
    test_ledge_climb();
    test_tape_round_trip();
    test_hitscan_kills();
    test_bot_fights_back();
    test_map_round_trip();
    test_nav_pathing();
    core::log_info("sim_tests: all passed");
    return 0;
}
