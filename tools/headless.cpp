// Headless determinism runner. Steps the simulation from a scripted or taped
// input sequence and prints the world hash. Same inputs, same hash, on every
// machine. Used by CI, by cross-machine determinism checks, and later by the
// dedicated server.
//
//   um_headless [ticks]              scripted run
//   um_headless record <file> [ticks]  write the scripted tape, then run it
//   um_headless play <file>          replay a tape

#include "core/arena.h"
#include "core/log.h"
#include "core/types.h"
#include "sim/sim.h"

#include <cstring>

using namespace sim;

// A deterministic input sequence built from integer arithmetic only. The button
// schedules are coprime so the run drifts through combinations rather than
// repeating one narrow path. Coverage is the point: a harness that only ever
// ran forward and jumped would not notice determinism breaking in the crouch,
// slide, aim, or firing paths, which is precisely where netcode will live.
static InputCmd scripted(u32 i) {
    InputCmd c{};
    c.tick = TickId{i};
    c.move_x = static_cast<i8>((i % 4 < 2) ? 1 : -1);
    c.move_y = static_cast<i8>((i % 8 < 4) ? 1 : 0);
    c.look_dx = static_cast<i16>(static_cast<i32>(i % 16) - 8);
    c.look_dy = static_cast<i16>(static_cast<i32>((i / 2) % 8) - 4);
    u16 buttons = 0;
    if (i % 30u == 0u) {
        buttons |= static_cast<u16>(Button::Jump);
    }
    if (i % 53u < 21u) {
        buttons |= static_cast<u16>(Button::Crouch);  // slides while carrying speed
    }
    if (i % 67u < 17u) {
        buttons |= static_cast<u16>(Button::Aim);  // drops the sprint to a walk
    }
    if (i % 97u < 11u) {
        buttons |= static_cast<u16>(Button::Fire);
    }
    c.buttons = buttons;
    return c;
}

static u32 parse_u32(const char* s, u32 fallback) {
    if (s == nullptr) {
        return fallback;
    }
    u32 value = 0;
    bool any = false;
    for (const char* p = s; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') {
            return fallback;
        }
        value = value * 10u + static_cast<u32>(*p - '0');
        any = true;
    }
    return any ? value : fallback;
}

static u64 run(core::Span<const InputCmd> cmds) {
    World w{};
    init_match(w);  // a real 5v5 at team spawns, not ten characters at the origin
    for (u64 i = 0; i < cmds.size(); ++i) {
        World next{};
        simulate(w, cmds[i], next);
        w = next;
    }
    return hash(w);
}

int main(int argc, char** argv) {
    core::Arena arena = core::Arena::with_capacity(16ull << 20);

    // Determinism has to be checked against the level the game actually plays,
    // not the built-in fallback.
    core::Result<core::Unit, const char*> level = load_level(arena, MAP_PATH);
    if (level.is_err()) {
        core::log_errorf("headless: %s", level.error());
        return 1;
    }

    if (argc >= 3 && std::strcmp(argv[1], "play") == 0) {
        core::Result<core::Span<const InputCmd>, const char*> tape = tape_load(arena, argv[2]);
        if (tape.is_err()) {
            core::log_errorf("headless: %s", tape.error());
            return 1;
        }
        const core::Span<const InputCmd> cmds = tape.value();
        core::log_infof("headless: played %llu ticks, world hash %016llx",
                        static_cast<unsigned long long>(cmds.size()),
                        static_cast<unsigned long long>(run(cmds)));
        return 0;
    }

    const bool record = argc >= 3 && std::strcmp(argv[1], "record") == 0;
    const u32 ticks = parse_u32(argc > (record ? 3 : 1) ? argv[record ? 3 : 1] : nullptr, 600);

    const core::Span<InputCmd> cmds = arena.alloc_n<InputCmd>(ticks);
    for (u32 i = 0; i < ticks; ++i) {
        cmds[i] = scripted(i);
    }
    const core::Span<const InputCmd> view(cmds.data(), cmds.size());

    if (record) {
        core::Result<core::Unit, const char*> saved = tape_save(argv[2], view);
        if (saved.is_err()) {
            core::log_errorf("headless: %s", saved.error());
            return 1;
        }
        core::log_infof("headless: recorded %u ticks to %s", ticks, argv[2]);
    }

    core::log_infof("headless: %u ticks, world hash %016llx", ticks,
                    static_cast<unsigned long long>(run(view)));
    return 0;
}
