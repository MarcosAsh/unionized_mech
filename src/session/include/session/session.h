#pragma once

// The authoritative loop. The server owns the world; the client sends input and
// is told what happened. Left there, every action would cost a round trip, so
// the client also runs the simulation itself on its own input and shows the
// result immediately — then, when the server's version of that tick arrives,
// rewinds to it and replays the inputs the server had not seen yet.
//
// That only works because `sim::simulate` is pure and deterministic: replaying
// the same inputs from the same state has to land on the same bits, or the
// correction is visible as a twitch on every tick. This module is the reason
// the sim has the rules it does.
//
// `net` deliberately knows nothing about `sim`, so the two are joined here.

#include "core/types.h"
#include "net/net.h"
#include "sim/sim.h"

namespace session {

/// Ticks of input the client remembers for replay, and of authoritative state
/// the server keeps to diff against. At 60Hz this is half a second, which
/// covers any round trip a player would tolerate.
constexpr u32 HISTORY = 32;

/// Tick value meaning "no snapshot yet", so a baseline of zero is not confused
/// with the genuine tick zero.
constexpr u32 NO_TICK = 0xFFFFFFFFu;

/// The fixed part of a snapshot packet, ahead of the delta bytes.
struct SnapshotHeader {
    u32 tick;           ///< Server tick this snapshot describes.
    u32 baseline_tick;  ///< Tick it was diffed against, or NO_TICK for a cold start.
    u32 last_input;     ///< Last client input tick the server had consumed.
};

/// The fixed part of an input packet. The client's baseline rides along with
/// its input rather than travelling on its own: the server needs to know which
/// snapshot the client actually holds before it can diff against it, and input
/// is already flowing every tick.
struct InputHeader {
    u32 tick;      ///< Client tick this command belongs to.
    u32 baseline;  ///< Newest snapshot tick the client has applied, or NO_TICK.
};

/// The authoritative simulation. Steps on its own clock and answers with a
/// delta against whatever the client last confirmed it holds.
class Server {
public:
    /// Start a fresh match.
    void start();

    /// Take a client command for `tick`, along with the snapshot tick the
    /// client says it holds. Out-of-order and duplicate commands are ignored:
    /// only the newest matters, since the next step uses it.
    void on_input(u32 tick, const sim::InputCmd& cmd, u32 client_baseline);

    /// Advance one tick using the most recent command received.
    void tick();

    /// Write a snapshot into `out`, diffed against the snapshot the client last
    /// said it holds when the server still has that tick, and against the
    /// cold-start world otherwise. Returns bytes written, or 0 when it will not
    /// fit, which the caller must treat as "send nothing this tick".
    [[nodiscard]] u32 write_snapshot(u8* out, u32 capacity) const;

    [[nodiscard]] const sim::World& world() const { return world_; }
    [[nodiscard]] u32 tick_count() const { return world_.tick.raw; }

private:
    sim::World world_{};
    sim::World history_[HISTORY]{};  ///< Past states, indexed by tick % HISTORY.
    sim::InputCmd latest_{};
    u32 latest_tick_ = NO_TICK;
    u32 client_baseline_ = NO_TICK;
};

/// The client's copy: a predicted world it draws, plus the last authoritative
/// state it was given and the inputs the server has not confirmed yet.
class Client {
public:
    /// Start from the same fresh match the server did, so the first delta has a
    /// baseline to land on without anything being sent.
    void start();

    /// Record and apply `cmd` for the current tick, advancing the predicted
    /// world immediately. Returns the tick the command was stamped with.
    u32 predict(const sim::InputCmd& cmd);

    /// Apply a snapshot: adopt the authoritative state, then replay every
    /// command the server had not yet consumed so the predicted world catches
    /// back up to the present. False when the delta was malformed or its
    /// baseline is one this client no longer holds.
    bool on_snapshot(const SnapshotHeader& header, const u8* delta, u32 delta_size);

    /// What to draw: the prediction, which is ahead of the server by the round
    /// trip.
    [[nodiscard]] const sim::World& world() const { return predicted_; }

    /// The last authoritative state, for reference and tests.
    [[nodiscard]] const sim::World& confirmed() const { return confirmed_; }

    /// The tick the client's prediction has reached.
    [[nodiscard]] u32 tick_count() const { return predicted_.tick.raw; }

    /// The baseline the server should diff against, or NO_TICK when nothing has
    /// been confirmed yet.
    [[nodiscard]] u32 baseline() const { return confirmed_tick_; }

    /// How many commands were replayed by the last snapshot. Zero means the
    /// server was fully caught up; a persistent high number means the link is
    /// long, and it is the number to watch when prediction misbehaves.
    [[nodiscard]] u32 last_replay_count() const { return last_replay_; }

private:
    sim::World predicted_{};
    sim::World confirmed_{};
    u32 confirmed_tick_ = NO_TICK;
    /// Recent authoritative states, so a snapshot delayed in flight can still
    /// be decoded against the baseline it was actually diffed against rather
    /// than only against the newest one. Keyed by tick, with the tick stored
    /// alongside because the ring wraps.
    sim::World confirmed_history_[HISTORY]{};
    u32 confirmed_ticks_[HISTORY]{};
    bool confirmed_valid_[HISTORY]{};
    sim::InputCmd history_[HISTORY]{};  ///< Sent commands, indexed by tick % HISTORY.
    u32 last_replay_ = 0;
};

}  // namespace session
