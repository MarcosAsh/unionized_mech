// Headless determinism runner. Steps the simulation N ticks from a fixed input
// sequence and prints the world hash. Same binary, same hash every run, and the
// same hash across all target machines. Used by CI and, later, the server.

#include "core/log.h"
#include "core/types.h"
#include "sim/sim.h"

using namespace sim;

// A deterministic input sequence built from integer arithmetic only.
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

int main(int argc, char** argv) {
    const u32 ticks = parse_u32(argc > 1 ? argv[1] : nullptr, 600);

    World w{};
    for (u32 i = 0; i < ticks; ++i) {
        World next{};
        simulate(w, scripted(i), next);
        w = next;
    }

    core::log_infof("headless: %u ticks, world hash %016llx", ticks,
                    static_cast<unsigned long long>(hash(w)));
    return 0;
}
