// Client/server tests. No sockets: the two ends are driven directly and the
// packets handed between them, which is what lets latency and loss be exact
// rather than hoped for.

#include "core/arena.h"
#include "core/assert.h"
#include "core/log.h"
#include "core/types.h"
#include "session/link.h"
#include "session/session.h"

#include <cmath>

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

        server.on_input(0, tick, cmd, client.baseline());
        server.tick();

        const u32 n = server.write_snapshot(0, packet, sizeof(packet));
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
            server.on_input(0, tick - LATENCY, sim::scripted_input(i - LATENCY),
                            baseline_at[i - LATENCY]);
        }
        server.tick();

        const u32 n = server.write_snapshot(0, packet, sizeof(packet));
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
        server.on_input(0, client.predict(cmd), cmd, client.baseline());
        server.tick();
    }
    const u32 n = server.write_snapshot(0, packet, sizeof(packet));
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
    server.on_input(0, client.predict(cmd), cmd, client.baseline());
    server.tick();
    const u32 n1 = server.write_snapshot(0, first, sizeof(first));
    SnapshotHeader h1{};
    __builtin_memcpy(&h1, first, sizeof(h1));

    const sim::InputCmd cmd2 = sim::scripted_input(1);
    server.on_input(0, client.predict(cmd2), cmd2, client.baseline());
    server.tick();
    const u32 n2 = server.write_snapshot(0, second, sizeof(second));
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
            server.on_input(0, tick, cmd, client.baseline());
        }
        server.tick();
    }
    const u32 n = server.write_snapshot(0, packet, sizeof(packet));
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
        server.on_input(0, client.predict(cmd), cmd, client.baseline());
        server.tick();
        const u32 n = server.write_snapshot(0, packet, sizeof(packet));
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


// Two clients on one server, each driving its own slot. This is what the
// per-slot command change bought, so it is checked end to end: both predict,
// both reconcile, and both converge on the server's world rather than on each
// other's guess about what the other was doing.
static void test_two_clients_share_one_world() {
    Server server;
    Client a;
    Client b;
    server.start(2);
    a.start(0, 2);
    b.start(1, 2);

    static u8 packet[net::MAX_PACKET];
    for (u32 i = 0; i < 240; ++i) {
        // The two players do visibly different things.
        sim::InputCmd ca{};
        ca.move_y = 1;
        sim::InputCmd cb{};
        cb.move_x = 1;

        const u32 ta = a.predict(ca);
        const u32 tb = b.predict(cb);
        server.on_input(0, ta, ca, a.baseline());
        server.on_input(1, tb, cb, b.baseline());
        server.tick();

        for (u32 slot = 0; slot < 2; ++slot) {
            const u32 n = server.write_snapshot(slot, packet, sizeof(packet));
            ASSERT(n > sizeof(SnapshotHeader));
            SnapshotHeader header{};
            __builtin_memcpy(&header, packet, sizeof(header));
            Client& c = slot == 0 ? a : b;
            ASSERT(c.on_snapshot(header, packet + sizeof(SnapshotHeader),
                                 n - sizeof(SnapshotHeader)));
        }
        // Neither client's prediction drifts from the authority, and they agree
        // with each other because they agree with it.
        ASSERT(sim::hash(a.world()) == sim::hash(server.world()));
        ASSERT(sim::hash(b.world()) == sim::hash(server.world()));
    }

    // Each client drove its own character, not the other's, and both moved.
    ASSERT(a.slot() == 0 && b.slot() == 1);
    ASSERT(server.world().chars[0].z != server.world().chars[1].z ||
           server.world().chars[0].x != server.world().chars[1].x);
}


// A client predicts into its OWN slot. This needs no server, and deliberately
// so: with one in the loop every snapshot corrects the mistake within a tick,
// so a prediction aimed at the wrong character is invisible from the outside.
// That is precisely how it would ship unnoticed.
static void test_client_predicts_its_own_slot() {
    Client c;
    c.start(1, 2);  // this client drives slot 1; slot 0 is another person

    const f32 mine_x = c.world().chars[1].x;
    const f32 mine_z = c.world().chars[1].z;
    const f32 other_x = c.world().chars[0].x;
    const f32 other_z = c.world().chars[0].z;

    sim::InputCmd cmd{};
    cmd.move_y = 1;
    for (u32 i = 0; i < 90; ++i) {
        (void)c.predict(cmd);
    }

    const f32 mdx = c.world().chars[1].x - mine_x;
    const f32 mdz = c.world().chars[1].z - mine_z;
    const f32 odx = c.world().chars[0].x - other_x;
    const f32 odz = c.world().chars[0].z - other_z;
    // Its own character walked; the other person, who sent nothing, stood still.
    ASSERT(std::sqrt(mdx * mdx + mdz * mdz) > 3.0f);
    ASSERT(std::sqrt(odx * odx + odz * odz) < 1.0f);
}


// The whole thing over a real socket: two clients and a server on the loopback,
// exchanging real datagrams. Everything above this drives the loop directly,
// which is what makes latency exact — but that also means nothing above this
// has ever proved the packets are well formed, that a client is recognised by
// its address, or that the two halves agree on the layout.
static void test_loopback_server_carries_two_clients() {
    core::Result<ServerHost, const char*> host_r = ServerHost::open(0, 2);
    ASSERT(host_r.is_ok());
    ServerHost host = static_cast<ServerHost&&>(host_r.value());

    const net::Address at = net::Address::loopback(host.port());
    core::Result<ClientLink, const char*> a_r = ClientLink::open(at, 0, 2);
    core::Result<ClientLink, const char*> b_r = ClientLink::open(at, 1, 2);
    ASSERT(a_r.is_ok() && b_r.is_ok());
    ClientLink a = static_cast<ClientLink&&>(a_r.value());
    ClientLink b = static_cast<ClientLink&&>(b_r.value());

    sim::InputCmd forward{};
    forward.move_y = 1;
    sim::InputCmd strafe{};
    strafe.move_x = 1;

    u32 applied_a = 0;
    u32 applied_b = 0;
    for (u32 i = 0; i < 240; ++i) {
        a.send(forward);
        b.send(strafe);
        host.poll();
        host.tick();
        host.broadcast();
        applied_a += a.poll();
        applied_b += b.poll();
    }

    // Both were recognised and given their own slot.
    ASSERT(host.client_count() == 2);
    // Snapshots actually flowed, in both directions — without the sends landing
    // there would be nothing to apply, and the convergence below would be two
    // clients agreeing only because neither had heard anything.
    ASSERT(applied_a > 100);
    ASSERT(applied_b > 100);
    // And each client ended up on the server's world.
    ASSERT(sim::hash(a.world()) == sim::hash(host.world()));
    ASSERT(sim::hash(b.world()) == sim::hash(host.world()));
    // Each drove its own character: they were given different commands and are
    // no longer standing on top of each other.
    ASSERT(host.world().chars[0].x != host.world().chars[1].x ||
           host.world().chars[0].z != host.world().chars[1].z);
    // And the commands actually arrived. Convergence alone does not show this:
    // two clients agree just as well when the server is hearing nothing from
    // either and everyone is standing still.
    const sim::Spawn s0 = sim::team_spawn(0, 0);
    const sim::Spawn s1 = sim::team_spawn(0, 1);
    const f32 d0x = host.world().chars[0].x - s0.x;
    const f32 d0z = host.world().chars[0].z - s0.z;
    const f32 d1x = host.world().chars[1].x - s1.x;
    const f32 d1z = host.world().chars[1].z - s1.z;
    ASSERT(std::sqrt(d0x * d0x + d0z * d0z) > 5.0f);
    ASSERT(std::sqrt(d1x * d1x + d1z * d1z) > 5.0f);
    // Still hearing from both, right up to the end. Movement alone does not
    // show this: the server re-applies a client's last command every tick, so a
    // single packet that arrived 240 ticks ago walks a character just as far as
    // a client that never stopped talking.
    ASSERT(host.last_input(0) + 4 >= host.world().tick.raw);
    ASSERT(host.last_input(1) + 4 >= host.world().tick.raw);
}

// A packet that is not ours, or is truncated, is dropped rather than parsed.
// The socket is public; anything can arrive on it.
static void test_garbage_packets_are_ignored() {
    core::Result<ServerHost, const char*> host_r = ServerHost::open(0, 1);
    ASSERT(host_r.is_ok());
    ServerHost host = static_cast<ServerHost&&>(host_r.value());

    core::Result<net::Socket, const char*> raw_r = net::Socket::open(0);
    ASSERT(raw_r.is_ok());
    net::Socket raw = static_cast<net::Socket&&>(raw_r.value());
    const net::Address at = net::Address::loopback(host.port());

    u8 junk[64];
    for (u32 i = 0; i < sizeof(junk); ++i) {
        junk[i] = static_cast<u8>(i * 7 + 1);
    }
    ASSERT(raw.send(at, junk, sizeof(junk)));   // nonsense
    ASSERT(raw.send(at, junk, 4));              // far too short

    // A packet shaped exactly like an input packet, but stamped with someone
    // else's protocol id. The length and kind checks wave it through, so only
    // the connection check can catch it — and a port gets stray traffic.
    u8 forged[sizeof(net::Header) + 1 + sizeof(InputHeader) + sizeof(sim::InputCmd)] = {};
    net::Header bad{};
    bad.protocol = 0xDEADBEEFu;
    bad.ack = 0xFFFFu;
    __builtin_memcpy(forged, &bad, sizeof(bad));
    forged[sizeof(net::Header)] = static_cast<u8>(Packet::Input);
    ASSERT(raw.send(at, forged, sizeof(forged)));
    host.poll();
    host.tick();

    // Nothing was adopted as a client and the world still stepped normally.
    ASSERT(host.client_count() == 0);
    ASSERT(host.world().tick.raw == 1);
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
    test_two_clients_share_one_world();
    test_client_predicts_its_own_slot();
    test_loopback_server_carries_two_clients();
    test_garbage_packets_are_ignored();
    core::log_info("session_tests: all passed");
    return 0;
}
