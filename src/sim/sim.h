#pragma once

#include "core/arena.h"
#include "core/result.h"
#include "core/span.h"
#include "core/types.h"

#include <type_traits>

namespace sim {

/// Identifies one simulation tick. A newtype over u32 so it cannot be confused
/// with counts or indices.
struct TickId {
    u32 raw = 0;

    [[nodiscard]] friend constexpr bool operator==(TickId a, TickId b) {
        return a.raw == b.raw;
    }
    [[nodiscard]] friend constexpr bool operator!=(TickId a, TickId b) {
        return a.raw != b.raw;
    }
};

/// Simulation rate in ticks per second.
constexpr u32 SIM_HZ = 60;

/// Fixed simulation timestep in seconds. A compile-time constant with identical
/// bits on every platform.
constexpr f32 SIM_DT = 1.0f / static_cast<f32>(SIM_HZ);

/// Player action buttons, one bit each, packed into InputCmd::buttons.
enum class Button : u16 {
    Jump = 0x0001,
    Crouch = 0x0002,
    Fire = 0x0004,
    Aim = 0x0008,
    Use = 0x0010,
    Reload = 0x0020,
};

/// True when `b` is held in `buttons`.
[[nodiscard]] constexpr bool button_down(u16 buttons, Button b) {
    return (buttons & static_cast<u16>(b)) != 0;
}

/// Set or clear `b` in `buttons`.
inline void set_button(u16& buttons, Button b, bool down) {
    const u16 mask = static_cast<u16>(b);
    buttons = down ? static_cast<u16>(buttons | mask)
                   : static_cast<u16>(buttons & static_cast<u16>(~mask));
}

/// One tick of player intent. POD and bit-packable. This is the unit that
/// crosses the wire in M6, so its layout is fixed and asserted below.
struct InputCmd {
    TickId tick;      ///< Tick this command applies to.
    i16 look_dx = 0;  ///< Raw mouse delta x for this tick.
    i16 look_dy = 0;  ///< Raw mouse delta y for this tick.
    i8 move_x = 0;    ///< Strafe intent, -1 left to +1 right.
    i8 move_y = 0;    ///< Forward intent, -1 back to +1 forward.
    u16 buttons = 0;  ///< Button bitfield, see Button.
};

static_assert(sizeof(InputCmd) == 12, "InputCmd layout must stay wire-stable");
static_assert(alignof(InputCmd) == 4, "InputCmd alignment must stay wire-stable");
static_assert(std::is_trivially_copyable_v<InputCmd>);
static_assert(std::is_standard_layout_v<InputCmd>);

/// The character's movement mode this tick.
enum class MoveState : u8 { Ground, Slide, Air, Wallrun };

/// The whole simulation state. Scalars stand in for vectors and quaternions
/// until the M1 math module lands. Trivially copyable so it can be snapshotted
/// and hashed by value.
struct World {
    TickId tick;
    f32 cam_x = 0.0f;      ///< Feet position, world X.
    f32 cam_y = 0.0f;      ///< Feet height above the floor.
    f32 cam_z = 0.0f;      ///< Feet position, world Z.
    f32 cam_yaw = 0.0f;    ///< Radians, accumulated. Trig lives in render only.
    f32 cam_pitch = 0.0f;  ///< Radians, clamped.
    f32 vel_x = 0.0f;      ///< Velocity, world units per second.
    f32 vel_y = 0.0f;
    f32 vel_z = 0.0f;
    f32 wall_nx = 0.0f;    ///< Nearby wall normal (wallrun or airborne approach).
    f32 wall_nz = 0.0f;
    f32 land_impact = 0.0f;  ///< Fall speed at the last landing, decaying.
    u16 wallrun_ticks = 0;      ///< Ticks spent on the current wall.
    MoveState state = MoveState::Air;
    u8 on_ground = 0;      ///< 1 while standing on the floor.
    u8 ducked = 0;         ///< 1 while crouching or sliding.
    u8 air_jumps = 0;      ///< Remaining mid-air jumps (double jump).
    u8 jump_was_down = 0;  ///< Jump button state last tick, for edge detection.
    u8 coyote_ticks = 0;   ///< Ticks since last grounded, saturating.
    u8 jump_buffer = 0;    ///< Ticks a buffered jump press stays valid.
};

static_assert(std::is_trivially_copyable_v<World>);

/// An axis-aligned collision box in world space.
struct Aabb {
    f32 min_x, min_y, min_z;
    f32 max_x, max_y, max_z;
};

/// The static collision boxes for the level. The floor plane at y = 0 is
/// implicit. Both the simulation and the renderer read these, so what you see is
/// exactly what you collide with.
[[nodiscard]] core::Span<const Aabb> level_boxes();

/// Advance the world by one tick. Pure: it reads `prev` and `cmd` and writes
/// `next`, with no globals and no wall-clock. The same inputs always yield the
/// same `next`, on every target platform.
void simulate(const World& prev, const InputCmd& cmd, World& next);

/// A 64-bit hash of the world state, stable across platforms. Used by the
/// determinism harness and, later, by the server.
[[nodiscard]] u64 hash(const World& w);

/// Write an input tape: the exact InputCmd sequence of a run, replayable for
/// the determinism harness and, later, for server-side verification.
/// # Errors
/// A static message when the file cannot be written.
[[nodiscard]] core::Result<core::Unit, const char*> tape_save(const char* path,
                                                              core::Span<const InputCmd> cmds);

/// Load an input tape written by tape_save, backed by `arena` storage.
/// # Errors
/// A static message when the file is missing, truncated, or not a tape.
[[nodiscard]] core::Result<core::Span<const InputCmd>, const char*> tape_load(core::Arena& arena,
                                                                              const char* path);

/// Fixed-timestep accumulator. Converts variable frame time into a whole number
/// of 60Hz ticks, capping how many run in one frame but keeping the leftover
/// time for later frames, so no simulation time is lost (keep-the-debt).
class FixedTimestep {
public:
    /// Fold in `elapsed` seconds and return how many ticks to run now, at most
    /// `max_ticks`.
    [[nodiscard]] u32 advance(f64 elapsed, u32 max_ticks);

    /// Fraction into the next tick, in [0, 1), for render interpolation.
    [[nodiscard]] f32 alpha() const;

private:
    f64 accumulator_ = 0.0;
};

}  // namespace sim
