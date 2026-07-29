#include "sim/sim.h"

#include "sim_internal.h"

namespace sim {

namespace {

u64 fnv1a(u64 h, const void* data, u64 n) {
    const u8* p = static_cast<const u8*>(data);
    for (u64 i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

}  // namespace

InputCmd scripted_input(u32 tick) {
    InputCmd c{};
    c.tick = TickId{tick};
    // Held long enough to actually build speed. Flipping the stick every couple
    // of ticks looks like coverage but never crosses the slide threshold, so
    // the crouch bit below would exercise nothing.
    c.move_x = static_cast<i8>((tick % 64 < 32) ? 1 : -1);
    c.move_y = static_cast<i8>((tick % 120 < 100) ? 1 : 0);
    c.look_dx = static_cast<i16>(static_cast<i32>(tick % 16) - 8);
    c.look_dy = static_cast<i16>(static_cast<i32>((tick / 2) % 8) - 4);
    u16 buttons = 0;
    if (tick % 30u == 0u) {
        buttons |= static_cast<u16>(Button::Jump);
    }
    if (tick % 53u < 21u) {
        buttons |= static_cast<u16>(Button::Crouch);  // slides while carrying speed
    }
    if (tick % 67u < 17u) {
        buttons |= static_cast<u16>(Button::Aim);  // drops the sprint to a walk
    }
    if (tick % 97u < 11u) {
        buttons |= static_cast<u16>(Button::Fire);
    }
    if (tick % 149u < 5u) {
        buttons |= static_cast<u16>(Button::Reload);
    }
    if (tick % 211u < 3u) {
        buttons |= static_cast<u16>(Button::Grenade);
    }
    c.buttons = buttons;
    return c;
}

void init_match(World& world) {
    world = World{};
    for (u32 i = 0; i < MAX_PLAYERS; ++i) {
        Character& c = world.chars[i];
        c = Character{};
        c.team = i < MAX_PLAYERS / 2 ? 0 : 1;
        const Spawn spawn = team_spawn(c.team, i);
        c.x = spawn.x;
        c.y = spawn.y;
        c.z = spawn.z;
        c.yaw = spawn.yaw;
        rearm(c);
    }
}

void simulate(const World& prev, const InputCmd given[MAX_PLAYERS], World& next) {
    next = prev;
    next.tick = TickId{prev.tick.raw + 1};

    // Bot inputs derive from the previous world, deterministically, through the
    // same InputCmd people and the network use. Which slots are people is world
    // state rather than a parameter, so a replay cannot disagree with the run it
    // came from about who was driving.
    const u32 humans = prev.humans < MAX_PLAYERS ? prev.humans : MAX_PLAYERS;
    InputCmd cmds[MAX_PLAYERS];
    for (u32 i = 0; i < MAX_PLAYERS; ++i) {
        cmds[i] = i < humans ? given[i] : bot_think(prev, i);
    }
    for (u32 i = 0; i < MAX_PLAYERS; ++i) {
        if (next.chars[i].alive != 0 && next.chars[i].merged == 0) {
            step_character(next.chars[i], prev.chars[i], cmds[i]);
        }
    }
    step_union(next, cmds);

    // During the end-of-match banner the fighting stops; when the countdown
    // runs out a fresh match starts with a shifted seed for variety.
    if (next.winner != 0) {
        if (next.end_ticks > 0) {
            next.end_ticks -= 1;
        } else {
            const u32 seed = next.seed + 1;
            const TickId tick = next.tick;
            init_match(next);
            next.seed = seed;
            next.tick = tick;
        }
        return;
    }
    step_grenades(next, cmds);
    resolve_combat(next, cmds);
    for (u32 team = 0; team < 2; ++team) {
        if (team_kills(next, team) >= WIN_KILLS) {
            next.winner = static_cast<u8>(team + 1);
            next.end_ticks = 600;  // ten seconds of banner
        }
    }
}

void simulate(const World& prev, const InputCmd& cmd, World& next) {
    InputCmd cmds[MAX_PLAYERS] = {};
    cmds[0] = cmd;
    simulate(prev, cmds, next);
}

u64 hash(const World& w) {
    // Character and Mech are asserted padding-free, so the world hashes
    // byte-wise.
    static_assert(sizeof(World) == 12 + sizeof(Mech) + sizeof(Character) * MAX_PLAYERS +
                                       sizeof(Grenade) * MAX_GRENADES);
    return fnv1a(0xcbf29ce484222325ull, &w, sizeof(World));
}

u32 team_kills(const World& w, u32 team) {
    u32 total = 0;
    for (u32 i = 0; i < MAX_PLAYERS; ++i) {
        if (w.chars[i].team == team) {
            total += w.chars[i].kills;
        }
    }
    return total;
}

u32 FixedTimestep::advance(f64 elapsed, u32 max_ticks) {
    accumulator_ += elapsed;
    const f64 dt = static_cast<f64>(SIM_DT);
    u32 ticks = 0;
    while (accumulator_ >= dt && ticks < max_ticks) {
        accumulator_ -= dt;
        ++ticks;
    }
    return ticks;
}

f32 FixedTimestep::alpha() const {
    return static_cast<f32>(accumulator_ / static_cast<f64>(SIM_DT));
}

}  // namespace sim
