// Determinism, purity and the fixed timestep.

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

void run_core() {
    test_replay_twice_identical();
    test_simulate_is_pure();
    test_tick_advances();
    test_fixed_timestep();
    test_tape_round_trip();
}

}  // namespace sim_tests

int main() {
    // Every movement test runs against the shipping level, so geometry the game
    // plays and geometry the tests assert on cannot drift apart. The map tests
    // deliberately replace it and so run last.
    core::Arena level_arena = core::Arena::with_capacity(1u << 20);
    ASSERT(load_level(level_arena, MAP_PATH).is_ok());

    sim_tests::run_core();
    sim_tests::run_movement();
    sim_tests::run_combat();
    sim_tests::run_map();
    core::log_info("sim_tests: all passed");
    return 0;
}
