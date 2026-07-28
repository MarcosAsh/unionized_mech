// Client/server tests. No sockets: the two ends are driven directly and the
// packets handed between them, which is what lets latency and loss be exact
// rather than hoped for.

#include "core/arena.h"
#include "core/assert.h"
#include "core/log.h"
#include "core/types.h"
#include "session/session.h"

using namespace session;

/// A packet in flight, held for as long as the test's latency says.
struct Wire {
    u8 bytes[net::MAX_PACKET];
    u32 size = 0;
    SnapshotHeader header{};
    u32 deliver_at = 0;
    bool live = false;
};

// With no loss and no latency, prediction has to be invisible: the client
// predicted a tick, the server simulated the same tick from the same input, and
// the reconciliation has to land on exactly the same bits. Anything less shows
// up in play as a twitch on every single tick, which is the failure mode this
// whole design exists to avoid.
static void test_prediction_matches_server_exactly() {
    Server server;
    Client client;
    server.start();
    client.start();

    static u8 packet[net::MAX_PACKET];
    for (u32 i = 0; i < 200; ++i) {
        const sim::InputCmd cmd = sim::scripted_input(i);
        const u32 tick = client.predict(cmd);

        server.on_input(tick, cmd, client.baseline());
        server.tick();

        const u32 n = server.write_snapshot(packet, sizeof(packet));
        ASSERT(n > sizeof(SnapshotHeader));
        SnapshotHeader header{};
        __builtin_memcpy(&header, packet, sizeof(header));
        ASSERT(client.on_snapshot(header, packet + sizeof(SnapshotHeader),
                                  n - sizeof(SnapshotHeader)));

        // The whole point, checked every tick rather than at the end.
        ASSERT(sim::hash(client.world()) == sim::hash(server.world()));
        ASSERT(client.tick_count() == server.tick_count());
    }
}

// The realistic case: the snapshot describing tick N does not arrive until the
// client has already predicted several ticks past it. The client must rewind to
// the authoritative state and replay what the server had not seen, ending up
// where its own prediction already was.
static void test_replay_absorbs_latency() {
    Server server;
    Client client;
    server.start();
    client.start();

    constexpr u32 LATENCY = 6;  // ticks each way
    Wire flight[64];
    static u8 packet[net::MAX_PACKET];
    static u32 baseline_at[512];
    u32 replays_seen = 0;

    for (u32 i = 0; i < 300; ++i) {
        const sim::InputCmd cmd = sim::scripted_input(i);
        const u32 tick = client.predict(cmd);
        baseline_at[i] = client.baseline();

        // The server hears the command LATENCY ticks late, and with it the
        // baseline the client held back then — not the one it holds now. That
        // lag is exactly why the server diffs against an older snapshot than
        // the client's newest, and why the client has to keep a ring of them.
        if (i >= LATENCY) {
            server.on_input(tick - LATENCY, sim::scripted_input(i - LATENCY),
                            baseline_at[i - LATENCY]);
        }
        server.tick();

        const u32 n = server.write_snapshot(packet, sizeof(packet));
        ASSERT(n > 0);
        Wire& slot = flight[i % 64];
        ASSERT(!slot.live);
        __builtin_memcpy(slot.bytes, packet, n);
        slot.size = n;
        __builtin_memcpy(&slot.header, packet, sizeof(SnapshotHeader));
        slot.deliver_at = i + LATENCY;
        slot.live = true;

        // Deliver whatever is due this tick.
        for (Wire& w : flight) {
            if (!w.live || w.deliver_at != i) {
                continue;
            }
            ASSERT(client.on_snapshot(w.header, w.bytes + sizeof(SnapshotHeader),
                                      w.size - sizeof(SnapshotHeader)));
            replays_seen += client.last_replay_count();
            w.live = false;
        }
        // The client is always at least as far along as the server it is
        // predicting ahead of.
        ASSERT(client.tick_count() >= server.tick_count());
    }
    // Replaying is the mechanism under test; if nothing was ever replayed the
    // test proved nothing about it.
    ASSERT(replays_seen > 0);
}

// A snapshot diffed against a baseline the client no longer holds must be
// refused, not decoded into nonsense over whatever it does hold. This is the
// case that turns a hiccup into a permanently corrupted world.
static void test_unknown_baseline_is_refused() {
    Server server;
    Client client;
    server.start();
    client.start();

    static u8 packet[net::MAX_PACKET];
    for (u32 i = 0; i < 5; ++i) {
        const sim::InputCmd cmd = sim::scripted_input(i);
        server.on_input(client.predict(cmd), cmd, client.baseline());
        server.tick();
    }
    const u32 n = server.write_snapshot(packet, sizeof(packet));
    SnapshotHeader header{};
    __builtin_memcpy(&header, packet, sizeof(header));
    // Claim it was diffed against a tick the client has never confirmed.
    header.baseline_tick = 12345;
    ASSERT(!client.on_snapshot(header, packet + sizeof(SnapshotHeader),
                               n - sizeof(SnapshotHeader)));
}

// An old snapshot overtaken by a newer one must be dropped: UDP reorders, and
// adopting the older one would drag the world backwards.
static void test_stale_snapshot_is_dropped() {
    Server server;
    Client client;
    server.start();
    client.start();

    static u8 first[net::MAX_PACKET];
    static u8 second[net::MAX_PACKET];

    const sim::InputCmd cmd = sim::scripted_input(0);
    server.on_input(client.predict(cmd), cmd, client.baseline());
    server.tick();
    const u32 n1 = server.write_snapshot(first, sizeof(first));
    SnapshotHeader h1{};
    __builtin_memcpy(&h1, first, sizeof(h1));

    const sim::InputCmd cmd2 = sim::scripted_input(1);
    server.on_input(client.predict(cmd2), cmd2, client.baseline());
    server.tick();
    const u32 n2 = server.write_snapshot(second, sizeof(second));
    SnapshotHeader h2{};
    __builtin_memcpy(&h2, second, sizeof(h2));

    // Deliver them out of order.
    ASSERT(client.on_snapshot(h2, second + sizeof(SnapshotHeader), n2 - sizeof(SnapshotHeader)));
    const u64 after_newer = sim::hash(client.confirmed());
    ASSERT(!client.on_snapshot(h1, first + sizeof(SnapshotHeader), n1 - sizeof(SnapshotHeader)));
    ASSERT(sim::hash(client.confirmed()) == after_newer);
}

// The server is the authority. When the client's prediction is wrong — here
// because the client never told the server about half its input — the snapshot
// has to drag it back, not be quietly averaged with it.
static void test_server_overrules_the_client() {
    Server server;
    Client client;
    server.start();
    client.start();

    static u8 packet[net::MAX_PACKET];
    for (u32 i = 0; i < 60; ++i) {
        const sim::InputCmd cmd = sim::scripted_input(i);
        const u32 tick = client.predict(cmd);
        // Half the commands never reach the server, so the two worlds genuinely
        // disagree rather than merely lagging.
        if (i % 2 == 0) {
            server.on_input(tick, cmd, client.baseline());
        }
        server.tick();
    }
    const u32 n = server.write_snapshot(packet, sizeof(packet));
    SnapshotHeader header{};
    __builtin_memcpy(&header, packet, sizeof(header));
    ASSERT(client.on_snapshot(header, packet + sizeof(SnapshotHeader),
                              n - sizeof(SnapshotHeader)));
    // Both ends are at the same tick and every command was accounted for, so
    // the client's world is now the server's exactly.
    ASSERT(sim::hash(client.confirmed()) == sim::hash(server.world()));
    ASSERT(sim::hash(client.world()) == sim::hash(server.world()));
}

// Snapshots have to fit the packet budget on real match state, or none of this
// works outside a test.
static void test_snapshots_fit_the_budget() {
    Server server;
    Client client;
    server.start();
    client.start();

    static u8 packet[net::MAX_PACKET];
    u32 worst = 0;
    for (u32 i = 0; i < 600; ++i) {
        const sim::InputCmd cmd = sim::scripted_input(i);
        server.on_input(client.predict(cmd), cmd, client.baseline());
        server.tick();
        const u32 n = server.write_snapshot(packet, sizeof(packet));
        ASSERT(n > 0);
        ASSERT(n <= net::MAX_PACKET);
        if (n > worst) {
            worst = n;
        }
        SnapshotHeader header{};
        __builtin_memcpy(&header, packet, sizeof(header));
        ASSERT(client.on_snapshot(header, packet + sizeof(SnapshotHeader),
                                  n - sizeof(SnapshotHeader)));
    }
    // Comfortably inside, not merely under: leaving no headroom would mean a
    // busier match silently stops sending snapshots.
    ASSERT(worst < net::MAX_PACKET / 2);
}

int main() {
    // The sim needs a level: bots path against it and hitscan traces it.
    core::Arena arena = core::Arena::with_capacity(1u << 20);
    ASSERT(sim::load_level(arena, MAP_PATH).is_ok());

    test_prediction_matches_server_exactly();
    test_replay_absorbs_latency();
    test_unknown_baseline_is_refused();
    test_stale_snapshot_is_dropped();
    test_server_overrules_the_client();
    test_snapshots_fit_the_budget();
    core::log_info("session_tests: all passed");
    return 0;
}
