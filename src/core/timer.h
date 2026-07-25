#pragma once

#include "core/types.h"

namespace core {

/// A monotonic clock for frame pacing and profiling only.
/// # Invariants
/// Never read by simulation. Simulation time comes from the fixed tick, not
/// wall-clock, so determinism holds.
struct Timer {
    /// Nanoseconds since an unspecified monotonic origin.
    [[nodiscard]] static u64 now_ns();
};

}  // namespace core
