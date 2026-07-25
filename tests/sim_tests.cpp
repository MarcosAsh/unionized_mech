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
    w.spin_angle = 1.0f;
    const InputCmd cmd = scripted(7);

    World a{};
    World b{};
    simulate(w, cmd, a);
    simulate(w, cmd, b);

    ASSERT(hash(a) == hash(b));
    ASSERT(w.cam_x == 3.0f);
    ASSERT(w.spin_angle == 1.0f);
}

static void test_tick_advances() {
    World w{};
    World next{};
    simulate(w, scripted(0), next);
    ASSERT(next.tick == TickId{1});
}

int main() {
    test_replay_twice_identical();
    test_simulate_is_pure();
    test_tick_advances();
    core::log_info("sim_tests: all passed");
    return 0;
}
