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
enum class MoveState : u8 { Ground, Slide, Air, Wallrun, Climb };

/// Characters in a match: the player in slot 0 and nine bots, five per team.
constexpr u32 MAX_PLAYERS = 10;

/// One character's full state: the movement controller plus combat. A plain
/// struct rather than structure-of-arrays because ten of these fit in cache
/// either way and byte-wise hashing wants a packed layout (asserted below).
struct Character {
    f32 x = 0.0f;  ///< Feet position.
    f32 y = 0.0f;
    f32 z = 0.0f;
    f32 yaw = 0.0f;    ///< Radians. Trig in sim goes through sim_math only.
    f32 pitch = 0.0f;  ///< Radians, clamped.
    f32 vx = 0.0f;     ///< Velocity, units per second.
    f32 vy = 0.0f;
    f32 vz = 0.0f;
    f32 wall_nx = 0.0f;  ///< Nearby wall normal (wallrun or airborne approach).
    f32 wall_nz = 0.0f;
    f32 land_impact = 0.0f;  ///< Fall speed at the last landing, decaying.
    f32 last_shot_t = 0.0f;  ///< Distance the last shot travelled.
    f32 shot_x = 0.0f;       ///< Fire-time eye position of the last shot.
    f32 shot_y = 0.0f;
    f32 shot_z = 0.0f;
    f32 shot_yaw = 0.0f;     ///< Fire-time aim of the last shot.
    f32 shot_pitch = 0.0f;
    u16 wallrun_ticks = 0;   ///< Ticks spent on the current wall.
    u16 respawn_ticks = 0;   ///< Ticks until respawn while dead.
    u16 kills = 0;
    i16 health = 100;
    MoveState state = MoveState::Air;
    u8 team = 0;           ///< 0 or 1.
    u8 alive = 1;
    u8 on_ground = 0;      ///< 1 while standing on the floor.
    u8 ducked = 0;         ///< 1 while crouching or sliding.
    u8 air_jumps = 0;      ///< Remaining mid-air jumps (double jump).
    u8 jump_was_down = 0;  ///< Jump button state last tick, for edge detection.
    u8 coyote_ticks = 0;   ///< Ticks since last grounded, saturating.
    u8 jump_buffer = 0;    ///< Ticks a buffered jump press stays valid.
    u8 fire_cooldown = 0;  ///< Ticks until the weapon can fire again.
    u8 shot_age = 255;     ///< Ticks since this character last fired, saturating.
    u8 shot_hit = 0;       ///< 1 when the last shot damaged someone.
    u8 hurt_age = 255;     ///< Ticks since this character was last damaged.
    u8 pad0 = 0;
    u8 pad1 = 0;
    u8 pad2 = 0;
};

static_assert(sizeof(Character) == 92, "Character layout must stay packed for hashing");

/// The whole simulation state: the match. Trivially copyable so it can be
/// snapshotted and hashed by value. Slot 0 is the player; bot inputs are
/// generated inside simulate from this state, deterministically.
/// Kills a team needs to win the match.
constexpr u32 WIN_KILLS = 30;

struct World {
    TickId tick;
    u32 seed = 0x4d454348u;  ///< Feeds every in-simulation random choice.
    u16 end_ticks = 0;  ///< Countdown of the end-of-match banner phase.
    u8 winner = 0;      ///< 0 none, otherwise winning team + 1.
    u8 pad = 0;
    Character chars[MAX_PLAYERS];

    /// The local player's character.
    [[nodiscard]] const Character& player() const { return chars[0]; }
    [[nodiscard]] Character& player() { return chars[0]; }
};

static_assert(std::is_trivially_copyable_v<World>);

/// An axis-aligned collision box in world space.
struct Aabb {
    f32 min_x, min_y, min_z;
    f32 max_x, max_y, max_z;
};

/// The static collision boxes for the level. The floor plane at y = 0 is
/// implicit. The renderer draws the visible prefix of this list; the remainder
/// are invisible volumes standing in for imported scenery, rough on purpose.
[[nodiscard]] core::Span<const Aabb> level_boxes();

/// The prefix of level_boxes drawn as visible geometry.
[[nodiscard]] core::Span<const Aabb> visible_boxes();

/// Where the player starts, from the loaded map.
struct Spawn {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
    f32 yaw = 0.0f;
};

/// The loaded map's spawn point.
[[nodiscard]] Spawn level_spawn();

/// A team's spawn point for the character in `slot` (spread so characters do
/// not stack).
[[nodiscard]] Spawn team_spawn(u32 team, u32 slot);

/// True when nothing solid blocks the segment from `a` to `b` (eye to eye
/// visibility for bots and hitscan).
[[nodiscard]] bool segment_clear(f32 ax, f32 ay, f32 az, f32 bx, f32 by, f32 bz);

/// Load a map file, replacing the active level. The format is line-based text:
/// `spawn x y z yaw`, `box x0 y0 z0 x1 y1 z1` for visible collision boxes, and
/// `cbox ...` for invisible collision volumes. Without a loaded map the
/// built-in level is active. Loading is an editor event, not a frame-loop
/// operation; `scratch` holds the file bytes during parsing.
/// # Errors
/// A static message when the file is missing or malformed. The previous level
/// stays active on error.
[[nodiscard]] core::Result<core::Unit, const char*> load_level(core::Arena& scratch,
                                                               const char* path);

/// Reset the world to a fresh 5v5 match: slot 0 is the player on team 0, the
/// rest are bots, everyone at their team spawns.
void init_match(World& world);

/// Advance the world by one tick. Pure: it reads `prev` and the player's `cmd`
/// and writes `next`, with no globals and no wall-clock. Bot inputs derive
/// deterministically from `prev` inside. The same inputs always yield the same
/// `next`, on every target platform.
void simulate(const World& prev, const InputCmd& cmd, World& next);

/// A 64-bit hash of the world state, stable across platforms. Used by the
/// determinism harness and, later, by the server.
[[nodiscard]] u64 hash(const World& w);

/// A team's total kills.
[[nodiscard]] u32 team_kills(const World& w, u32 team);

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
