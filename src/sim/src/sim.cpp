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
    }
}

void simulate(const World& prev, const InputCmd& cmd, World& next) {
    next = prev;
    next.tick = TickId{prev.tick.raw + 1};

    // Bot inputs derive from the previous world, deterministically, through
    // the same InputCmd the player and, later, the network use.
    InputCmd cmds[MAX_PLAYERS];
    bool fired[MAX_PLAYERS];
    cmds[0] = cmd;
    for (u32 i = 1; i < MAX_PLAYERS; ++i) {
        cmds[i] = bot_think(prev, i);
    }
    for (u32 i = 0; i < MAX_PLAYERS; ++i) {
        fired[i] = button_down(cmds[i].buttons, Button::Fire);
        if (next.chars[i].alive != 0) {
            step_character(next.chars[i], prev.chars[i], cmds[i]);
        }
    }

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
    resolve_combat(next, fired);
    for (u32 team = 0; team < 2; ++team) {
        if (team_kills(next, team) >= WIN_KILLS) {
            next.winner = static_cast<u8>(team + 1);
            next.end_ticks = 600;  // ten seconds of banner
        }
    }
}

u64 hash(const World& w) {
    // Character is asserted padding-free, so the world hashes byte-wise.
    static_assert(sizeof(World) == 12 + sizeof(Character) * MAX_PLAYERS);
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
