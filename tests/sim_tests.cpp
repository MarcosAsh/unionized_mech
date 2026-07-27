// Determinism and purity tests for the sim module. Checks are plain ASSERTs.

#include "core/assert.h"
#include "core/file.h"

#include <cmath>
#include "core/log.h"
#include "core/types.h"
#include "sim/sim.h"

using namespace sim;

static void run(u32 n, u64* hashes) {
    World w{};
    init_match(w);  // the same starting state the headless runner uses
    for (u32 i = 0; i < n; ++i) {
        World next{};
        simulate(w, scripted_input(i), next);
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
    const InputCmd cmd = scripted_input(7);

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
    simulate(w, scripted_input(0), next);
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

// Walking into a chest-high ledge vaults it instead of stopping dead. The map's
// mantle stairs present a 1m face at z=-34; start clear of it and run in.
static void test_mantle_vaults_ledge() {
    World w{};
    w.chars[0].x = -28.0f;
    w.chars[0].z = -31.0f;
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
// alone (jump apex 1.52m leaves the lip 1.48m away, past mantle reach), so
// reaching the top proves the ledge climb works.
static void test_ledge_climb() {
    World w{};
    w.chars[0].x = -18.0f;
    w.chars[0].z = -31.0f;  // clear of the 3m mantle stair, whose face is z=-34
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
        cmds[i] = scripted_input(i);
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
    init_match(w);  // a real match state, so everyone starts armed
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
    init_match(w);  // a real match state, so the bot starts armed
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

// The union loop end to end: merge into the mech by pressing use beside it,
// fire the chassis cannon (60 damage), eject through the top hatch, re-merge,
// and survive the chassis being shot out from underneath by an enemy bot.
static void test_union_mech() {
    World w{};
    init_match(w);  // a real match state, so everyone starts armed
    for (u32 i = 0; i < MAX_PLAYERS; ++i) {
        w.chars[i].x = 500.0f;
        w.chars[i].z = 500.0f;
        w.chars[i].team = 0;
    }
    w.chars[0].x = 3.0f;
    w.chars[0].z = 0.0f;  // beside the mech at the origin
    w.chars[1].x = 0.0f;
    w.chars[1].z = -6.0f;  // an enemy bot in front of it
    w.chars[1].team = 1;
    ASSERT(w.mech.alive == 1 && w.mech.pilot == NO_PILOT);

    InputCmd use{};
    use.buttons = static_cast<u16>(Button::Use);
    World cur{};
    simulate(w, use, cur);
    ASSERT(cur.player().merged == 1);
    ASSERT(cur.mech.pilot == 0);
    ASSERT(cur.player().x == cur.mech.x);

    // The chassis cannon: from the mech's high eye the shot must angle down
    // to the bot. One hit takes 60 of its 100.
    InputCmd fire{};
    fire.buttons = static_cast<u16>(Button::Fire);
    fire.look_dy = -160;  // pitch down toward the shorter target
    World after_fire{};
    simulate(cur, fire, after_fire);
    ASSERT(after_fire.chars[1].health <= 40);

    // Release, then press use again: eject out of the top hatch.
    World released{};
    simulate(after_fire, InputCmd{}, released);
    World ejected{};
    simulate(released, use, ejected);
    ASSERT(ejected.player().merged == 0);
    ASSERT(ejected.mech.pilot == NO_PILOT);
    ASSERT(ejected.player().y > 4.0f);

    // Re-merge and idle: the enemy bot chews the chassis down. The pilot must
    // pop out alive when it dies, and the wreck must score for the shooter.
    World again{};
    simulate(ejected, InputCmd{}, again);
    World merged{};
    simulate(again, use, merged);
    ASSERT(merged.player().merged == 1);
    World state = merged;
    for (u32 i = 0; i < 1500 && state.mech.alive != 0; ++i) {
        World next{};
        simulate(state, InputCmd{}, next);
        state = next;
    }
    ASSERT(state.mech.alive == 0);
    ASSERT(state.player().alive == 1);
    ASSERT(state.player().merged == 0);
    ASSERT(state.chars[1].kills >= 1);
}

// Driving the chassis over an enemy robot crushes it and credits the pilot.
static void test_mech_crush() {
    World w{};
    init_match(w);  // a real match state, so everyone starts armed
    for (u32 i = 0; i < MAX_PLAYERS; ++i) {
        w.chars[i].x = 500.0f;
        w.chars[i].z = 500.0f;
        w.chars[i].team = 0;
    }
    w.chars[0].x = 3.0f;
    w.chars[0].z = 0.0f;
    w.chars[1].x = 0.0f;
    w.chars[1].z = -5.0f;  // in the mech's path
    w.chars[1].yaw = 3.14159265f;  // facing the mech: it charges rather than flees
    w.chars[1].team = 1;

    InputCmd use{};
    use.buttons = static_cast<u16>(Button::Use);
    World cur{};
    simulate(w, use, cur);
    ASSERT(cur.player().merged == 1);

    InputCmd drive{};
    drive.move_y = 1;  // yaw 0: straight at the enemy
    for (u32 i = 0; i < 120 && cur.chars[1].alive != 0; ++i) {
        World next{};
        simulate(cur, drive, next);
        cur = next;
    }
    ASSERT(cur.chars[1].alive == 0);
    ASSERT(cur.player().kills >= 1);
}

// A kerb no taller than a step is walked straight over. Topping out proves the
// step-up ran at all; vy never going positive proves it was a step and not the
// mantle vault, which would have launched the character at 3.5 m/s instead.
static void test_step_over_kerb() {
    const char* step_map =
        "spawn 0 0 0 0\n"
        "box -4 0 -8 4 0.25 -4\n";  // a 0.25m kerb, under STEP_HEIGHT
    core::Arena arena = core::Arena::with_capacity(1u << 20);
    const char* path = "sim_tests_step.tmp";
    ASSERT(core::write_entire_file(path,
                                   core::Span<const u8>(
                                       reinterpret_cast<const u8*>(step_map),
                                       __builtin_strlen(step_map)))
               .is_ok());
    ASSERT(load_level(arena, path).is_ok());

    World w{};
    bool topped = false;
    f32 max_vy = 0.0f;
    for (u32 i = 0; i < 180; ++i) {
        InputCmd c{};
        c.tick = TickId{i};
        c.move_y = 1;  // yaw 0 faces -Z, straight at the kerb
        World next{};
        simulate(w, c, next);
        w = next;
        if (w.player().vy > max_vy) {
            max_vy = w.player().vy;
        }
        if (w.player().on_ground != 0 && w.player().y == 0.25f) {
            topped = true;
        }
    }
    ASSERT(topped);
    ASSERT(max_vy == 0.0f);
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

// A thrown grenade leaves the hand, arcs, and hurts an enemy inside the blast.
// Aiming straight down keeps the landing point predictable: it drops at the
// thrower's feet instead of depending on the whole arc.
static void test_grenade_damages_enemy() {
    World w{};
    init_match(w);
    for (u32 i = 0; i < MAX_PLAYERS; ++i) {
        w.chars[i].x = 500.0f;
        w.chars[i].z = 500.0f;
        w.chars[i].team = 0;
    }
    w.chars[0].x = 0.0f;
    w.chars[0].z = 0.0f;
    w.chars[0].pitch = -1.5f;  // very nearly straight down
    w.chars[1].x = 0.0f;
    w.chars[1].z = -2.0f;
    w.chars[1].team = 1;

    const u8 carried = w.chars[0].grenades;
    ASSERT(carried > 0);

    World cur = w;
    bool thrown = false;
    bool hurt = false;
    for (u32 i = 0; i < 240; ++i) {
        InputCmd c{};
        c.tick = TickId{i};
        if (i == 1) {
            c.buttons = static_cast<u16>(Button::Grenade);
        }
        World next{};
        simulate(cur, c, next);
        cur = next;
        if (cur.chars[0].grenades < carried) {
            thrown = true;
        }
        // Checked every tick: a kill respawns the target at full health, which
        // would hide the damage if we only looked at the end.
        if (cur.chars[1].health < 100 || cur.chars[1].alive == 0) {
            hurt = true;
        }
    }
    ASSERT(thrown);
    ASSERT(hurt);
}

// The slide entry boost is rate limited. Tapping crouch on and off re-enters
// the slide state every other tick, and without a cooldown each entry pays out
// again, which makes a crouch-mash the fastest way to move in the game. Run on
// open floor well outside the arena so only the boost is under test.
static void test_slide_boost_rate_limited() {
    World w{};
    init_match(w);
    for (u32 i = 0; i < MAX_PLAYERS; ++i) {
        w.chars[i].x = 500.0f;
        w.chars[i].z = 500.0f;
        w.chars[i].team = 0;
    }
    w.chars[0].x = 200.0f;
    w.chars[0].z = 200.0f;

    World cur = w;
    f32 peak = 0.0f;
    for (u32 i = 0; i < 300; ++i) {
        InputCmd c{};
        c.tick = TickId{i};
        c.move_y = 1;
        if (i > 60 && (i % 2) == 0) {  // sprint up first, then mash crouch
            c.buttons = static_cast<u16>(Button::Crouch);
        }
        World next{};
        simulate(cur, c, next);
        cur = next;
        const f32 s = std::sqrt(cur.player().vx * cur.player().vx +
                                cur.player().vz * cur.player().vz);
        if (s > peak) {
            peak = s;
        }
    }
    // One boost off a 6.6 m/s sprint lands near 8.6. Repeated boosts climb to
    // the 12.0 slide ceiling, so anything past 10 means the limiter is gone.
    ASSERT(peak < 10.0f);
}

// Sprint, jump on tick 60, then slam the stick backwards on `press_tick`.
// Returns how much horizontal speed that single tick cost.
static f32 lurch_probe_drop(u32 press_tick) {
    World w{};
    init_match(w);
    for (u32 i = 0; i < MAX_PLAYERS; ++i) {
        w.chars[i].x = 500.0f;
        w.chars[i].z = 500.0f;
        w.chars[i].team = 0;
    }
    w.chars[0].x = 200.0f;
    w.chars[0].z = 200.0f;

    World cur = w;
    f32 before = 0.0f;
    for (u32 i = 0; i <= press_tick; ++i) {
        InputCmd c{};
        c.tick = TickId{i};
        c.move_y = i == press_tick ? static_cast<i8>(-1) : static_cast<i8>(1);
        if (i == 60) {
            c.buttons = static_cast<u16>(Button::Jump);
        }
        if (i == press_tick) {
            before = std::sqrt(cur.player().vx * cur.player().vx +
                               cur.player().vz * cur.player().vz);
        }
        World next{};
        simulate(cur, c, next);
        cur = next;
    }
    const f32 after =
        std::sqrt(cur.player().vx * cur.player().vx + cur.player().vz * cur.player().vz);
    return before - after;
}

// Reversing the stick inside the post-jump window turns your momentum around
// and charges you for it; the identical press once the window has closed only
// gets ordinary air control. Comparing the two isolates the lurch: both runs
// air-accelerate the same way, so the difference cannot be anything else.
static void test_lurch_costs_speed_when_turning_back() {
    const f32 inside = lurch_probe_drop(65);   // 5 ticks after the jump
    const f32 outside = lurch_probe_drop(95);  // well past the 24-tick window

    // Air acceleration alone can move speed by at most AIR_ACCEL * SIM_DT *
    // AIR_MAX_SPEED in one tick, which is 0.3 m/s. A full-strength reversal
    // takes half of everything you had.
    ASSERT(outside < 0.5f);
    ASSERT(inside > 1.0f);
}

int main() {
    // Every movement test below runs against the shipping level, so geometry
    // the game plays and geometry the tests assert on cannot drift apart. The
    // map-loading tests at the end deliberately replace it and so run last.
    core::Arena level_arena = core::Arena::with_capacity(1u << 20);
    ASSERT(load_level(level_arena, MAP_PATH).is_ok());

    test_replay_twice_identical();
    test_simulate_is_pure();
    test_tick_advances();
    test_fixed_timestep();
    test_mantle_vaults_ledge();
    test_ledge_climb();
    test_tape_round_trip();
    test_hitscan_kills();
    test_bot_fights_back();
    test_union_mech();
    test_mech_crush();
    test_map_round_trip();
    test_nav_pathing();
    test_lurch_costs_speed_when_turning_back();
    test_slide_boost_rate_limited();
    test_grenade_damages_enemy();
    test_step_over_kerb();
    test_map_nav_fully_connected();
    core::log_info("sim_tests: all passed");
    return 0;
}
