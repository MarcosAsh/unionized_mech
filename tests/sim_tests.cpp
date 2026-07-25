// Determinism and purity tests for the sim module. Checks are plain ASSERTs.

#include "core/assert.h"
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
    w.cam_x = 3.0f;
    w.vel_x = 1.0f;
    const InputCmd cmd = scripted(7);

    World a{};
    World b{};
    simulate(w, cmd, a);
    simulate(w, cmd, b);

    ASSERT(hash(a) == hash(b));
    ASSERT(w.cam_x == 3.0f);
    ASSERT(w.vel_x == 1.0f);
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
    w.cam_x = -28.0f;
    w.cam_z = -34.0f;
    bool topped = false;
    for (u32 i = 0; i < 180; ++i) {
        InputCmd c{};
        c.tick = TickId{i};
        c.move_y = 1;  // yaw 0 faces -Z, straight at the step
        World next{};
        simulate(w, c, next);
        w = next;
        if (w.on_ground != 0 && w.cam_y == 1.0f) {
            topped = true;
        }
    }
    ASSERT(topped);
}

int main() {
    test_replay_twice_identical();
    test_simulate_is_pure();
    test_tick_advances();
    test_fixed_timestep();
    test_mantle_vaults_ledge();
    core::log_info("sim_tests: all passed");
    return 0;
}
