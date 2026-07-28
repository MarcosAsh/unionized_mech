// The character controller: mantle, climb, steps, walls, slide, lurch.

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

// Walking into a chest-high ledge vaults it instead of stopping dead. The map's
// mantle stairs present a 1m face at z=18; start clear of it and run in.
static void test_mantle_vaults_ledge() {
    World w{};
    w.chars[0].x = -40.0f;
    w.chars[0].z = 21.0f;
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
    w.chars[0].x = -30.0f;
    w.chars[0].z = 21.0f;  // clear of the 3m mantle stair, whose face is z=18
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

// A wall fixture and nothing else, so the only thing a character can interact
// with is the face. Deliberately far taller than the shipping map's 8m walls:
// the run timer only gets a chance to expire when the floor is not in the way.
static void load_wall_map(core::Arena& arena, const char* text) {
    const char* path = "sim_tests_wall.tmp";
    ASSERT(core::write_entire_file(
               path, core::Span<const u8>(reinterpret_cast<const u8*>(text),
                                          __builtin_strlen(text)))
               .is_ok());
    ASSERT(load_level(arena, path).is_ok());
}

// One face, at x = -8, running the length of the z axis.
static const char* ONE_WALL =
    "spawn 0 0 0 0\n"
    "box -10 0 -400 -8 60 400\n";

// A character stuck to that face, high up, running along it fast enough to hold
// a wallrun. The hull's left edge sits 0.05m off the face.
static World on_the_wall() {
    World w{};
    w.player().x = -7.55f;
    w.player().y = 40.0f;
    w.player().z = -200.0f;
    w.player().vz = 9.0f;  // along the wall, over the wallrun minimum
    return w;
}

// Counts ticks spent wallrunning over `ticks`, pressing jump whenever the
// character is on a wall, and steering with `move_x` the whole way. Returns the
// count through `out_end` so callers can also see where it finished.
static u32 wallrun_ticks_over(World& w, u32 ticks, i8 move_x, f32* out_max_y) {
    u32 count = 0;
    f32 max_y = w.player().y;
    for (u32 i = 0; i < ticks; ++i) {
        InputCmd c{};
        c.tick = TickId{i};
        c.move_x = move_x;
        if (w.player().state == MoveState::Wallrun) {
            c.buttons = static_cast<u16>(Button::Jump);
        }
        World next{};
        simulate(w, c, next);
        w = next;
        if (w.player().state == MoveState::Wallrun) {
            ++count;
        }
        if (w.player().y > max_y) {
            max_y = w.player().y;
        }
    }
    *out_max_y = max_y;
    return count;
}

// A wallrun's timer belongs to the wall, not to the moment-to-moment contact.
// It used to reset the instant contact was lost, which handed back both the
// timer and the gravity ramp every time: a probe measured 595 of 600 ticks
// spent wallrunning, still at full speed, having fallen 29m in ten seconds.
// The wall has to let go, and the character has to reach the floor.
static void test_wallrun_timer_ends_the_run() {
    core::Arena arena = core::Arena::with_capacity(1u << 20);
    load_wall_map(arena, ONE_WALL);

    World w = on_the_wall();
    u32 wallrun_ticks = 0;
    for (u32 i = 0; i < 600; ++i) {
        InputCmd c{};
        c.tick = TickId{i};
        World next{};
        simulate(w, c, next);
        w = next;
        if (w.player().state == MoveState::Wallrun) {
            ++wallrun_ticks;
        }
    }
    // One run is 1.75s of ticks. The bound is loose because the exact entry
    // tick is not the property under test; anything far past it means the wall
    // handed itself back.
    ASSERT(wallrun_ticks < 150);
    ASSERT(w.player().on_ground != 0);
    ASSERT(w.player().y == 0.0f);
}

// Nor can a character hang on one face by jumping off it and catching it again.
// The timer is spent either way, so the wall charges height for the attempt.
static void test_wall_jump_cannot_hold_one_wall() {
    core::Arena arena = core::Arena::with_capacity(1u << 20);
    load_wall_map(arena, ONE_WALL);

    World w = on_the_wall();
    const f32 start_y = w.player().y;
    f32 max_y = 0.0f;
    (void)wallrun_ticks_over(w, 600, -1, &max_y);  // -1 steers back into the face

    // A wall jump is worth 1.14m, and one double jump can slip in behind it
    // because the press is decided from last tick's state, which the sim may
    // already have left. Two lifts is the attempt; a third would be a ratchet.
    ASSERT(max_y - start_y < 3.0f);
    ASSERT(w.player().on_ground != 0);
    ASSERT(w.player().y < start_y - 30.0f);
}

// The other half of the rule, and the one the map is built on: the timer is
// owed to a *face*, so a different one is a fresh run. The west canyon's whole
// design is staggered opposite walls, run one and jump to the next, and it only
// works if crossing the gap buys a full new run. Same script, same ticks; the
// only difference is that there is a second face to land on.
static void test_facing_wall_grants_a_fresh_run() {
    core::Arena arena = core::Arena::with_capacity(1u << 20);
    // A 2m slot: the near face at x = -8 and a facing one at x = -6. Narrow on
    // purpose, so only ever one of them is inside wall-detect range and the
    // character cannot be touching both at once.
    load_wall_map(arena,
                  "spawn 0 0 0 0\n"
                  "box -10 0 -400 -8 60 400\n"
                  "box -6 0 -400 -4 60 400\n");

    // Ride the first face until it is spent, then push across to the other one.
    // The switch is late enough that the first wall's timer has certainly run
    // out, so anything the second face gives is a run it granted by itself.
    constexpr u32 CROSS_TICK = 130;
    World w = on_the_wall();
    u32 before = 0;
    u32 after = 0;
    for (u32 i = 0; i < 400; ++i) {
        InputCmd c{};
        c.tick = TickId{i};
        c.move_x = i < CROSS_TICK ? static_cast<i8>(0) : static_cast<i8>(1);
        World next{};
        simulate(w, c, next);
        w = next;
        if (w.player().state == MoveState::Wallrun) {
            i < CROSS_TICK ? ++before : ++after;
        }
    }
    // The first face gave its full run and then stopped giving.
    ASSERT(before > 100);
    ASSERT(before < 130);
    // The second one owes nothing to the first, so it grants a run of its own.
    // Without that, the spent timer would carry across and this would be zero.
    ASSERT(after > 60);
}

// Touching a wall gives the air jump back once, not once per tick of contact.
// Held down, jump is the crudest input there is, and it used to be a ladder:
// one tick of wall contact refilled the air jump, so a character could climb
// any wall in the game by mashing it. Nothing is being done well here — that is
// the point, since an exploit nobody has to be skilled to run is the one that
// matters.
static void test_mashing_jump_cannot_climb_a_wall() {
    core::Arena arena = core::Arena::with_capacity(1u << 20);
    load_wall_map(arena, ONE_WALL);

    World w = on_the_wall();
    const f32 start_y = w.player().y;
    f32 max_y = start_y;
    for (u32 i = 0; i < 600; ++i) {
        InputCmd c{};
        c.tick = TickId{i};
        c.move_x = -1;  // at yaw 0 this holds the character against the face
        if (i % 2 == 0) {
            c.buttons = static_cast<u16>(Button::Jump);  // mashed, so every press is an edge
        }
        World next{};
        simulate(w, c, next);
        w = next;
        if (w.player().y > max_y) {
            max_y = w.player().y;
        }
    }
    // One wall jump and one air jump is all the wall owes: about 2.7m of lift.
    // A single wall jump is all one face owes: about 1.14m of lift.
    ASSERT(max_y - start_y < 3.0f);
    // And 600 ticks of mashing ends back at the bottom of the wall, not up it.
    ASSERT(w.player().y < 3.0f);
}

// Crouching lets go of a wall. Same start, same wall, the only difference is
// the button, so nothing else can account for the wallrun never happening.
static void test_crouch_drops_off_wall() {
    core::Arena arena = core::Arena::with_capacity(1u << 20);
    load_wall_map(arena, ONE_WALL);

    u32 with_crouch = 0;
    u32 without = 0;
    for (u32 pass = 0; pass < 2; ++pass) {
        World w = on_the_wall();
        for (u32 i = 0; i < 120; ++i) {
            InputCmd c{};
            c.tick = TickId{i};
            if (pass == 0) {
                c.buttons = static_cast<u16>(Button::Crouch);
            }
            World next{};
            simulate(w, c, next);
            w = next;
            if (w.player().state == MoveState::Wallrun) {
                pass == 0 ? ++with_crouch : ++without;
            }
        }
    }
    ASSERT(with_crouch == 0);
    ASSERT(without > 60);  // the control run really does hold the wall
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

void run_movement() {
    test_mantle_vaults_ledge();
    test_ledge_climb();
    test_step_over_kerb();
    test_wallrun_timer_ends_the_run();
    test_wall_jump_cannot_hold_one_wall();
    test_facing_wall_grants_a_fresh_run();
    test_mashing_jump_cannot_climb_a_wall();
    test_crouch_drops_off_wall();
    test_slide_boost_rate_limited();
    test_lurch_costs_speed_when_turning_back();
}

}  // namespace sim_tests
