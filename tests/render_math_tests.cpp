// Acceptance check 7: interpolation reproduces the endpoints exactly, so
// rendering a snapshot pair at alpha 0 gives the earlier state and at alpha 1
// the later state, bit for bit.

#include "core/assert.h"
#include "core/log.h"
#include "core/types.h"
#include "render/render_math.h"

using namespace render;

static void test_lerp_endpoints() {
    ASSERT(lerp(2.0f, 5.0f, 0.0f) == 2.0f);
    ASSERT(lerp(2.0f, 5.0f, 1.0f) == 5.0f);
    ASSERT(lerp(-3.0f, 7.5f, 0.0f) == -3.0f);
    ASSERT(lerp(-3.0f, 7.5f, 1.0f) == 7.5f);
}

static void test_angle_lerp_endpoints() {
    ASSERT(angle_lerp(0.5f, 2.5f, 0.0f) == 0.5f);
    ASSERT(angle_lerp(0.5f, 2.5f, 1.0f) == 2.5f);
    // Endpoints stay exact even when the shortest path wraps.
    ASSERT(angle_lerp(3.0f, -3.0f, 0.0f) == 3.0f);
    ASSERT(angle_lerp(3.0f, -3.0f, 1.0f) == -3.0f);
}

int main() {
    test_lerp_endpoints();
    test_angle_lerp_endpoints();
    core::log_info("render_math_tests: all passed");
    return 0;
}
