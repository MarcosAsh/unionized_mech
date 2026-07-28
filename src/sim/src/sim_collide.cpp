#include "sim_collide.h"

#include "sim_internal.h"
#include "sim_level.h"
#include "sim_math.h"

namespace sim {

namespace {

constexpr f32 LAND_IMPACT_DECAY = 0.85f;  // per-tick decay of the landing dip

/// True when a hull of `height` at (x, y, z) is inside any of `boxes`. Used to
/// check there is room before stepping up onto something.
[[nodiscard]] bool hull_blocked(f32 x, f32 y, f32 z, f32 height, core::Span<const Aabb> boxes) {
    for (u64 i = 0; i < boxes.size(); ++i) {
        if (hull_overlaps(x, y, z, height, boxes[i])) {
            return true;
        }
    }
    return false;
}

/// The upward speed that clears a ledge `height` above the feet, with enough
/// margin left over to land on top of it rather than scrape the lip.
[[nodiscard]] f32 mantle_speed(f32 height) {
    return sim_sqrt(2.0f * GRAVITY * height) + MANTLE_MARGIN;
}

}  // namespace

bool move_and_slide(Character& c, const Character& prev_c, f32 hull_h, bool grounded) {
    const core::Span<const Aabb> boxes = level_boxes();

    c.x += c.vx * SIM_DT;
    for (u64 i = 0; i < boxes.size(); ++i) {
        const Aabb& b = boxes[i];
        if (hull_overlaps(c.x, c.y, c.z, hull_h, b)) {
            const f32 ledge = b.max_y - c.y;
            // A low step is walked straight over, keeping speed, so long as
            // there is room to stand up there. Taller ledges fall through to
            // the mantle vault below.
            if (grounded && ledge > 0.0f && ledge <= STEP_HEIGHT &&
                !hull_blocked(c.x, b.max_y, c.z, hull_h, boxes)) {
                c.y = b.max_y;
                continue;
            }
            c.x = c.vx > 0.0f ? b.min_x - HULL_HALF_WIDTH : b.max_x + HULL_HALF_WIDTH;
            if (ledge > 0.0f && ledge <= MANTLE_REACH) {
                const f32 needed = mantle_speed(ledge);
                if (c.vy < needed) {
                    c.vy = needed;
                }
            } else {
                c.vx = 0.0f;
            }
        }
    }

    c.z += c.vz * SIM_DT;
    for (u64 i = 0; i < boxes.size(); ++i) {
        const Aabb& b = boxes[i];
        if (hull_overlaps(c.x, c.y, c.z, hull_h, b)) {
            const f32 ledge = b.max_y - c.y;
            if (grounded && ledge > 0.0f && ledge <= STEP_HEIGHT &&
                !hull_blocked(c.x, b.max_y, c.z, hull_h, boxes)) {
                c.y = b.max_y;
                continue;
            }
            c.z = c.vz > 0.0f ? b.min_z - HULL_HALF_WIDTH : b.max_z + HULL_HALF_WIDTH;
            if (ledge > 0.0f && ledge <= MANTLE_REACH) {
                const f32 needed = mantle_speed(ledge);
                if (c.vy < needed) {
                    c.vy = needed;
                }
            } else {
                c.vz = 0.0f;
            }
        }
    }

    // The landing dip eases back every tick and refills on impact.
    c.land_impact = prev_c.land_impact * LAND_IMPACT_DECAY;

    bool grounded_now = false;
    c.y += c.vy * SIM_DT;
    for (u64 i = 0; i < boxes.size(); ++i) {
        const Aabb& b = boxes[i];
        if (hull_overlaps(c.x, c.y, c.z, hull_h, b)) {
            if (c.vy <= 0.0f) {
                c.y = b.max_y;  // land on top
                grounded_now = true;
                if (prev_c.on_ground == 0 && -c.vy > c.land_impact) {
                    c.land_impact = -c.vy;
                }
            } else {
                c.y = b.min_y - hull_h;  // bumped head
            }
            c.vy = 0.0f;
        }
    }

    if (c.y <= 0.0f) {
        c.y = 0.0f;
        if (c.vy < 0.0f) {
            if (prev_c.on_ground == 0 && -c.vy > c.land_impact) {
                c.land_impact = -c.vy;
            }
            c.vy = 0.0f;
        }
        grounded_now = true;
    }
    return grounded_now;
}

}  // namespace sim
