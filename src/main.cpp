#include "core/arena.h"
#include "core/log.h"
#include "core/types.h"

// Startup entry point. Stands up the three arenas from CLAUDE.md section 5. The
// window, GPU, simulation, and frame loop bolt on in the following M0 commits.
int main() {
    core::Arena permanent = core::Arena::with_capacity(64ull << 20);
    core::Arena frame = core::Arena::with_capacity(16ull << 20);
    core::Arena scratch = core::Arena::with_capacity(16ull << 20);

    core::log_infof(
        "unionized_mech: core online. arenas permanent=%lluMiB frame=%lluMiB scratch=%lluMiB",
        static_cast<unsigned long long>(permanent.capacity() >> 20),
        static_cast<unsigned long long>(frame.capacity() >> 20),
        static_cast<unsigned long long>(scratch.capacity() >> 20));

    return 0;
}
