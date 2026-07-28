// Transport tests. The acknowledgement bookkeeping is pure, so most of this
// feeds headers in by hand; only the last few tests touch a real socket.

#include "core/assert.h"
#include "core/log.h"
#include "core/types.h"
#include "net/net.h"

#include <cstdlib>

using namespace net;

// Plain comparison of sequence numbers breaks at the wrap, which at 60Hz
// arrives every eighteen minutes and would otherwise look like every packet
// suddenly going stale at once.
static void test_sequence_wraps() {
    ASSERT(sequence_newer(5, 4));
    ASSERT(!sequence_newer(4, 5));
    ASSERT(!sequence_newer(4, 4));
    // Across the wrap: 0 is newer than 65535, and 1 newer than 65534.
    ASSERT(sequence_newer(0, 65535));
    ASSERT(sequence_newer(1, 65534));
    ASSERT(!sequence_newer(65535, 0));
    // Wide gaps are read the short way round the circle, which is the whole
    // point: 40000 to 1000 is 26536 forward but 39000 back, so 1000 is the
    // newer of the two even though it is the smaller number.
    ASSERT(sequence_newer(1000, 40000));
    ASSERT(!sequence_newer(40000, 1000));
    // And a gap that is short the ordinary way round stays ordinary.
    ASSERT(sequence_newer(2000, 1000));
    ASSERT(!sequence_newer(1000, 2000));
}

// Sequence numbers come out in order and are consumed once each.
static void test_sequences_increment() {
    Connection c;
    for (u16 i = 0; i < 10; ++i) {
        const Header h = c.next_header();
        ASSERT(h.sequence == i);
        ASSERT(h.protocol == PROTOCOL_ID);
    }
    ASSERT(c.local_sequence() == 10);
}

/// A header as the far end would have sent it, carrying its own sequence and
/// acknowledging nothing.
static Header incoming(u16 sequence) {
    Header h{};
    h.protocol = PROTOCOL_ID;
    h.sequence = sequence;
    h.ack = 0xFFFFu;
    h.ack_bits = 0;
    return h;
}

// Packets that arrive are remembered, out-of-order ones are folded into the
// history behind the newest, and duplicates are rejected.
static void test_receive_window() {
    Connection c;
    ASSERT(c.on_received(incoming(0)));
    ASSERT(c.on_received(incoming(1)));
    ASSERT(c.on_received(incoming(2)));
    ASSERT(c.remote_sequence() == 2);
    // A duplicate is not news.
    ASSERT(!c.on_received(incoming(2)));
    ASSERT(!c.on_received(incoming(1)));

    // A jump forward, then the straggler that was skipped over.
    ASSERT(c.on_received(incoming(6)));
    ASSERT(c.remote_sequence() == 6);
    ASSERT(c.on_received(incoming(4)));
    ASSERT(!c.on_received(incoming(4)));
    ASSERT(c.remote_sequence() == 6);  // a late packet does not move the newest

    // Anything not ours is ignored outright.
    Header alien = incoming(7);
    alien.protocol = 0xDEADBEEFu;
    ASSERT(!c.on_received(alien));
}

// What the far end tells us it heard is what `acked` reports back. This is the
// half that decides whether a packet was lost, so it is worth being exact.
static void test_acks_report_what_arrived() {
    Connection sender;
    Connection receiver;

    // Send five, deliver all but the third.
    Header sent[5];
    for (u32 i = 0; i < 5; ++i) {
        sent[i] = sender.next_header();
        if (i != 2) {
            ASSERT(receiver.on_received(sent[i]));
        }
    }
    // The receiver replies, carrying its ack window; the sender folds it in.
    ASSERT(sender.on_received(receiver.next_header()));

    ASSERT(sender.acked(sent[0].sequence));
    ASSERT(sender.acked(sent[1].sequence));
    ASSERT(!sender.acked(sent[2].sequence));  // the one that never arrived
    ASSERT(sender.acked(sent[3].sequence));
    ASSERT(sender.acked(sent[4].sequence));
}

// A packet older than the window cannot be represented, and must read as
// un-acknowledged rather than quietly as delivered.
static void test_ack_window_has_an_edge() {
    Connection sender;
    Connection receiver;
    Header first = sender.next_header();
    ASSERT(receiver.on_received(first));
    // Push it far out of the 32-packet history.
    for (u32 i = 0; i < 80; ++i) {
        ASSERT(receiver.on_received(sender.next_header()));
    }
    ASSERT(sender.on_received(receiver.next_header()));
    ASSERT(!sender.acked(first.sequence));
}

// The whole loop over a real socket: two sockets on the loopback, a payload out
// and back, and the sender learning its packet arrived.
static void test_loopback_round_trip() {
    core::Result<Socket, const char*> server_r = Socket::open(0);
    ASSERT(server_r.is_ok());
    Socket server = static_cast<Socket&&>(server_r.value());
    core::Result<Socket, const char*> client_r = Socket::open(0);
    ASSERT(client_r.is_ok());
    Socket client = static_cast<Socket&&>(client_r.value());
    ASSERT(server.port() != 0 && client.port() != 0);

    Connection client_conn;
    Connection server_conn;

    // Client sends a header plus a small payload.
    u8 out[sizeof(Header) + 4];
    const Header sent = client_conn.next_header();
    __builtin_memcpy(out, &sent, sizeof(Header));
    out[sizeof(Header) + 0] = 'p';
    out[sizeof(Header) + 1] = 'i';
    out[sizeof(Header) + 2] = 'n';
    out[sizeof(Header) + 3] = 'g';
    ASSERT(client.send(Address::loopback(server.port()), out, sizeof(out)));

    // The server reads it. Loopback delivery is immediate in practice, but a
    // non-blocking socket can still report empty once, so this spins briefly
    // rather than asserting on the first try.
    u8 in[MAX_PACKET];
    Address from{};
    u32 got = 0;
    for (u32 spin = 0; spin < 10000 && got == 0; ++spin) {
        got = server.receive(&from, in, sizeof(in));
    }
    ASSERT(got == sizeof(out));
    ASSERT(from.port == client.port());
    Header received{};
    __builtin_memcpy(&received, in, sizeof(Header));
    ASSERT(received.sequence == sent.sequence);
    ASSERT(in[sizeof(Header) + 3] == 'g');
    ASSERT(server_conn.on_received(received));

    // The server replies, and its reply tells the client the ping landed.
    const Header reply = server_conn.next_header();
    ASSERT(server.send(from, &reply, sizeof(reply)));
    got = 0;
    for (u32 spin = 0; spin < 10000 && got == 0; ++spin) {
        got = client.receive(nullptr, in, sizeof(in));
    }
    ASSERT(got == sizeof(Header));
    Header back{};
    __builtin_memcpy(&back, in, sizeof(Header));
    ASSERT(client_conn.on_received(back));
    ASSERT(client_conn.acked(sent.sequence));
}

// An oversized datagram is refused rather than truncated: silently sending a
// short packet would corrupt whatever it carried.
static void test_oversized_send_refused() {
    core::Result<Socket, const char*> s_r = Socket::open(0);
    ASSERT(s_r.is_ok());
    Socket s = static_cast<Socket&&>(s_r.value());
    static u8 big[MAX_PACKET + 1];
    ASSERT(!s.send(Address::loopback(s.port()), big, sizeof(big)));
}


// A stand-in for a snapshot: big enough that the mask matters, and a multiple
// of four like the real structs are. net knows nothing about sim, so the tests
// do not either.
struct Snapshot {
    u32 words[400];
};

static void fill(Snapshot* s, u32 seed) {
    u32 x = seed;
    for (u32 i = 0; i < 400; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        s->words[i] = x;
    }
}

// The whole point: encode against a baseline, rebuild from the same baseline,
// get the original back exactly.
static void test_delta_round_trips() {
    Snapshot base;
    Snapshot cur;
    fill(&base, 1);
    fill(&cur, 1);
    // Change a scattered handful, the way a few moving players would.
    cur.words[0] = 0xAAAAAAAAu;
    cur.words[37] = 0;
    cur.words[200] = 0x12345678u;
    cur.words[399] = 0xFFFFFFFFu;

    static u8 buf[4096];
    const u32 n = delta_encode(&base, &cur, sizeof(Snapshot), buf, sizeof(buf));
    ASSERT(n > 0);
    // Four changed words plus the count and the mask, nowhere near the 1600
    // bytes the snapshot itself would cost.
    ASSERT(n == sizeof(u16) + 50 + 4 * 4);

    Snapshot out;
    ASSERT(delta_decode(&base, buf, n, &out, sizeof(Snapshot)));
    for (u32 i = 0; i < 400; ++i) {
        ASSERT(out.words[i] == cur.words[i]);
    }
}

// A state that did not move costs the mask and nothing else.
static void test_identical_delta_is_empty() {
    Snapshot base;
    fill(&base, 7);
    static u8 buf[4096];
    const u32 n = delta_encode(&base, &base, sizeof(Snapshot), buf, sizeof(buf));
    ASSERT(n == sizeof(u16) + 50);
    Snapshot out;
    ASSERT(delta_decode(&base, buf, n, &out, sizeof(Snapshot)));
    for (u32 i = 0; i < 400; ++i) {
        ASSERT(out.words[i] == base.words[i]);
    }
}

// A delta that does not fit must say so rather than truncate: a short packet
// applied over a baseline would corrupt the far end's copy silently, and it
// would stay corrupt because every later delta builds on it.
static void test_encode_refuses_to_truncate() {
    Snapshot base;
    Snapshot cur;
    fill(&base, 3);
    fill(&cur, 9);  // every word differs
    static u8 small[256];
    ASSERT(delta_encode(&base, &cur, sizeof(Snapshot), small, sizeof(small)) == 0);
}

// A damaged or mismatched delta is rejected, never half-applied.
static void test_decode_rejects_bad_input() {
    Snapshot base;
    Snapshot cur;
    fill(&base, 11);
    fill(&cur, 11);
    cur.words[5] = 1234;
    cur.words[300] = 5678;

    static u8 buf[4096];
    const u32 n = delta_encode(&base, &cur, sizeof(Snapshot), buf, sizeof(buf));
    ASSERT(n > 0);
    Snapshot out;

    // Truncated: the mask promises words the packet does not carry. The copy
    // is sized to exactly the truncated length and heap-allocated, so a decoder
    // that reads past the end is a heap overflow the debug preset's sanitizer
    // reports. Decoding out of a generous fixed buffer would hide that.
    u8* exact = static_cast<u8*>(std::malloc(n - 4));
    ASSERT(exact != nullptr);
    __builtin_memcpy(exact, buf, n - 4);
    ASSERT(!delta_decode(&base, exact, n - 4, &out, sizeof(Snapshot)));
    std::free(exact);
    // Trailing bytes mean the two ends disagree about the format.
    buf[n] = 0;
    ASSERT(!delta_decode(&base, buf, n + 1, &out, sizeof(Snapshot)));
    // Encoded against a struct of a different size.
    ASSERT(!delta_decode(&base, buf, n, &out, sizeof(Snapshot) - 4));
    // Nothing at all.
    ASSERT(!delta_decode(&base, buf, 1, &out, sizeof(Snapshot)));
}

int main() {
    test_sequence_wraps();
    test_sequences_increment();
    test_receive_window();
    test_acks_report_what_arrived();
    test_ack_window_has_an_edge();
    test_loopback_round_trip();
    test_oversized_send_refused();
    test_delta_round_trips();
    test_identical_delta_is_empty();
    test_encode_refuses_to_truncate();
    test_decode_rejects_bad_input();
    core::log_info("net_tests: all passed");
    return 0;
}
