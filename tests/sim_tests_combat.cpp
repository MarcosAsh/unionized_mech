// Shooting, the mech, grenades, reloading, and the balance between them.

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

// One scripted fight: slot 0 against `enemies` bots on a 25m ring around the
// chassis, tracking the nearest one with perfect aim and holding the trigger.
// Merging on the first tick is the only difference between the two loadouts,
// so anything the results differ by is the loadout.
struct FightResult {
    u32 lasted = 0;
    u32 kills = 0;
};

static FightResult scripted_fight(bool merge, u32 enemies, u32 seed) {
    World w{};
    init_match(w);
    w.seed = 0x4d454348u + seed * 0x9e3779b9u;
    for (u32 i = 1; i < MAX_PLAYERS; ++i) {
        w.chars[i].x = 900.0f;  // allies parked out of the fight
        w.chars[i].z = 900.0f;
        w.chars[i].team = 0;
    }
    for (u32 e = 0; e < enemies; ++e) {
        Character& c = w.chars[5 + e];
        c.team = 1;
        const f32 a = 6.2831853f * static_cast<f32>(e) / static_cast<f32>(enemies);
        c.x = 25.0f * std::sin(a);
        c.z = 25.0f * std::cos(a);
    }
    // Within merge range of the chassis, which stands at the origin.
    w.chars[0].x = 0.0f;
    w.chars[0].y = 0.0f;
    w.chars[0].z = 3.0f;

    for (u32 i = 0; i < 1200; ++i) {
        InputCmd c{};
        c.tick = TickId{i};
        c.buttons = static_cast<u16>(merge && i == 0 ? Button::Use : Button::Fire);

        const Character& me = w.player();
        const f32 eye = me.merged != 0 ? 3.9f : 1.7f;
        f32 best = 1e9f;
        f32 yaw = me.yaw;
        f32 pitch = me.pitch;
        for (u32 j = 5; j < 5 + enemies; ++j) {
            const Character& t = w.chars[j];
            if (t.alive == 0) {
                continue;
            }
            const f32 dx = t.x - me.x;
            const f32 dy = (t.y + 0.9f) - (me.y + eye);
            const f32 dz = t.z - me.z;
            const f32 d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (d < best) {
                best = d;
                yaw = std::atan2(dx, -dz);
                pitch = std::asin(dy / d);
            }
        }
        f32 dyaw = yaw - me.yaw;
        while (dyaw > 3.14159265f) dyaw -= 6.2831853f;
        while (dyaw < -3.14159265f) dyaw += 6.2831853f;
        c.look_dx = static_cast<i16>(dyaw / 0.0025f);
        c.look_dy = static_cast<i16>((pitch - me.pitch) / 0.0025f);

        World next{};
        simulate(w, c, next);
        w = next;
        if (merge ? w.mech.alive == 0 : w.player().alive == 0) {
            return FightResult{i, w.player().kills};
        }
    }
    return FightResult{1200, w.player().kills};
}

// Merging has to be worth doing — it is the game's title mechanic and it used to
// be the losing option. The chassis is ten times a pilot's area to shoot at, so
// unarmoured it died in 54 ticks against four enemies where a pilot lasted 762.
// Eight seeds a side, so one unlucky bot cannot decide it.
static void test_merging_beats_fighting_on_foot() {
    core::Arena arena = core::Arena::with_capacity(1u << 20);
    ASSERT(load_level(arena, MAP_PATH).is_ok());

    u32 foot_kills = 0;
    u32 mech_kills = 0;
    for (u32 seed = 0; seed < 8; ++seed) {
        foot_kills += scripted_fight(false, 3, seed).kills;
        mech_kills += scripted_fight(true, 3, seed).kills;
    }
    // Comfortably ahead, not marginally: 93 against 41 when this was written,
    // and 24 against 41 with the armour removed.
    ASSERT(mech_kills > foot_kills + foot_kills / 2);
}

// The other half of the same balance: a chassis a whole team cannot remove is
// its own problem. Focused by five, it dies inside the window.
static void test_a_team_can_still_wreck_the_mech() {
    core::Arena arena = core::Arena::with_capacity(1u << 20);
    ASSERT(load_level(arena, MAP_PATH).is_ok());

    u32 survived_whole_fight = 0;
    for (u32 seed = 0; seed < 8; ++seed) {
        if (scripted_fight(true, 5, seed).lasted >= 1200) {
            ++survived_whole_fight;
        }
    }
    ASSERT(survived_whole_fight <= 2);
}

// The reload readout the viewmodel animates from. Its one subtlety is telling
// the two reload durations apart: the magazine is not refilled until the reload
// completes, so a magazine still empty means the longer reload from dry is what
// is running. Getting that wrong makes the animation finish early or late.
static void test_reload_phase_runs_start_to_finish() {
    World w{};
    init_match(w);
    Character& c = w.chars[0];
    ASSERT(reload_phase(c) == 0.0f);  // not reloading

    // Empty the magazine, then pull the trigger, which forces a reload.
    c.ammo = 0;
    InputCmd fire{};
    fire.buttons = static_cast<u16>(Button::Fire);
    World next{};
    simulate(w, fire, next);
    w = next;
    ASSERT(w.player().reload_ticks > 0);

    // The animation has to start when the reload does. Reading the wrong one of
    // the two durations leaves the phase pinned at zero for the first stretch,
    // which looks like the weapon hanging still and then hurrying.
    for (u32 i = 0; i < 10; ++i) {
        World n{};
        simulate(w, InputCmd{}, n);
        w = n;
    }
    ASSERT(reload_phase(w.player()) > 0.02f);

    // It climbs from near zero to one and never runs backwards.
    f32 last = -1.0f;
    u32 steps = 0;
    while (w.player().reload_ticks > 0 && steps < 600) {
        const f32 phase = reload_phase(w.player());
        ASSERT(phase >= 0.0f && phase <= 1.0f);
        ASSERT(phase >= last);
        last = phase;
        World n{};
        simulate(w, InputCmd{}, n);
        w = n;
        ++steps;
    }
    ASSERT(last > 0.9f);              // it got most of the way there
    ASSERT(reload_phase(w.player()) == 0.0f);  // and stops once the reload lands
    ASSERT(w.player().ammo > 0);
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

void run_combat() {
    test_hitscan_kills();
    test_bot_fights_back();
    test_union_mech();
    test_mech_crush();
    test_merging_beats_fighting_on_foot();
    test_a_team_can_still_wreck_the_mech();
    test_reload_phase_runs_start_to_finish();
    test_grenade_damages_enemy();
}

}  // namespace sim_tests
