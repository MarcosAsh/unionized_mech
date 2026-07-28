#include "session/session.h"

namespace session {

namespace {

/// The state both ends agree on before a single packet is exchanged. The client
/// builds it for itself, which is what lets even the first snapshot be a delta
/// rather than a full world too big to send.
[[nodiscard]] sim::World fresh_match() {
    sim::World w{};
    sim::init_match(w);
    return w;
}

}  // namespace

void Server::start() {
    world_ = fresh_match();
    for (u32 i = 0; i < HISTORY; ++i) {
        history_[i] = world_;
    }
    latest_ = sim::InputCmd{};
    latest_tick_ = NO_TICK;
    client_baseline_ = NO_TICK;
}

void Server::on_input(u32 tick, const sim::InputCmd& cmd, u32 client_baseline) {
    // Only the newest command matters: the next step uses one command, and an
    // older one arriving late describes a moment already simulated.
    if (latest_tick_ != NO_TICK && tick <= latest_tick_) {
        return;
    }
    latest_tick_ = tick;
    latest_ = cmd;
    client_baseline_ = client_baseline;
}

void Server::tick() {
    sim::World next{};
    sim::simulate(world_, latest_, next);
    world_ = next;
    history_[world_.tick.raw % HISTORY] = world_;
}

u32 Server::write_snapshot(u8* out, u32 capacity) const {
    const u32 client_baseline = client_baseline_;
    if (capacity < sizeof(SnapshotHeader)) {
        return 0;
    }
    // The client's baseline is only usable while it is still in the ring; older
    // than that and the cold-start world is the only thing both ends still
    // agree on.
    const bool have_baseline =
        client_baseline != NO_TICK && world_.tick.raw >= client_baseline &&
        world_.tick.raw - client_baseline < HISTORY;

    const sim::World cold = fresh_match();
    const sim::World& baseline = have_baseline ? history_[client_baseline % HISTORY] : cold;

    SnapshotHeader header{};
    header.tick = world_.tick.raw;
    header.baseline_tick = have_baseline ? client_baseline : NO_TICK;
    header.last_input = latest_tick_;

    const u32 body = net::delta_encode(&baseline, &world_, sizeof(sim::World),
                                       out + sizeof(SnapshotHeader),
                                       capacity - sizeof(SnapshotHeader));
    if (body == 0) {
        return 0;  // will not fit; the caller sends nothing rather than a lie
    }
    __builtin_memcpy(out, &header, sizeof(SnapshotHeader));
    return sizeof(SnapshotHeader) + body;
}

void Client::start() {
    predicted_ = fresh_match();
    confirmed_ = predicted_;
    confirmed_tick_ = NO_TICK;
    for (u32 i = 0; i < HISTORY; ++i) {
        history_[i] = sim::InputCmd{};
        confirmed_valid_[i] = false;
        confirmed_ticks_[i] = NO_TICK;
    }
    last_replay_ = 0;
}

u32 Client::predict(const sim::InputCmd& cmd) {
    const u32 tick = predicted_.tick.raw + 1;
    history_[tick % HISTORY] = cmd;
    sim::World next{};
    sim::simulate(predicted_, cmd, next);
    predicted_ = next;
    return tick;
}

bool Client::on_snapshot(const SnapshotHeader& header, const u8* delta, u32 delta_size) {
    // Rebuild the authoritative state from whichever baseline the server used.
    // A cold-start snapshot needs nothing from us; any other one needs the very
    // tick we told the server we held.
    // The snapshot names the tick it was diffed against, and that is rarely the
    // newest one we hold: a packet in flight was written before our latest ack
    // reached the server. So look the baseline up by tick rather than assuming
    // the most recent, which is the difference between tolerating latency and
    // rejecting every packet under it.
    sim::World baseline{};
    if (header.baseline_tick == NO_TICK) {
        baseline = fresh_match();
    } else {
        const u32 slot = header.baseline_tick % HISTORY;
        if (!confirmed_valid_[slot] || confirmed_ticks_[slot] != header.baseline_tick) {
            return false;  // diffed against something we no longer have
        }
        baseline = confirmed_history_[slot];
    }

    sim::World authoritative{};
    if (!net::delta_decode(&baseline, delta, delta_size, &authoritative, sizeof(sim::World))) {
        return false;
    }
    // A snapshot older than one already applied is stale; adopting it would
    // rewind the world for no reason.
    if (confirmed_tick_ != NO_TICK && header.tick <= confirmed_tick_) {
        return false;
    }
    confirmed_ = authoritative;
    confirmed_tick_ = header.tick;
    const u32 slot = header.tick % HISTORY;
    confirmed_history_[slot] = authoritative;
    confirmed_ticks_[slot] = header.tick;
    confirmed_valid_[slot] = true;

    // Replay: the server's state accounts for input up to `last_input`, so
    // everything the client has sent since then still has to be applied on top.
    // This is the step that makes prediction invisible when the server agrees,
    // and a correction when it does not.
    const u32 target = predicted_.tick.raw;
    sim::World rolled = authoritative;
    u32 replayed = 0;
    for (u32 tick = header.tick + 1; tick <= target; ++tick) {
        if (target - tick >= HISTORY) {
            continue;  // that command has fallen out of the ring
        }
        sim::World next{};
        sim::simulate(rolled, history_[tick % HISTORY], next);
        rolled = next;
        ++replayed;
    }
    last_replay_ = replayed;
    // Never move the prediction backwards: if the server is ahead of us, take
    // its tick, otherwise keep our own clock running.
    predicted_ = rolled;
    return true;
}

}  // namespace session
