#pragma once

// UDP transport. A game like this cannot use TCP: a dropped packet must not
// stall the ones behind it, because by the time a retransmit arrives the state
// it described is already stale. So this is UDP plus the small amount of
// bookkeeping that makes it usable — sequence numbers, acknowledgements, and a
// notion of a connection that can time out.
//
// Nothing here is reliable delivery. It reports what arrived and what did not;
// deciding what to resend is the caller's problem, and for snapshots the answer
// is usually "nothing, send the next one instead".

#include "core/result.h"
#include "core/types.h"

namespace net {

/// An IPv4 endpoint, host byte order. Ports are the caller's business.
struct Address {
    u32 host = 0;
    u16 port = 0;

    [[nodiscard]] static Address loopback(u16 port);
    [[nodiscard]] static Address any(u16 port);

    [[nodiscard]] friend bool operator==(Address a, Address b) {
        return a.host == b.host && a.port == b.port;
    }
    [[nodiscard]] friend bool operator!=(Address a, Address b) { return !(a == b); }
};

/// Largest datagram sent or accepted. Under the 1500-byte Ethernet MTU with
/// room for the IP and UDP headers, so a packet never fragments at the IP
/// layer, where one lost fragment silently costs the whole datagram.
constexpr u32 MAX_PACKET = 1200;

/// A non-blocking UDP socket. Never waits: `receive` returns 0 when there is
/// nothing to read, which is what lets the frame loop poll it.
class Socket {
public:
    /// Bind a UDP socket to `port`, or to an OS-chosen port when it is 0.
    /// # Errors
    /// A static message when the socket cannot be created or bound.
    [[nodiscard]] static core::Result<Socket, const char*> open(u16 port);

    Socket() = default;
    ~Socket();
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    /// Send `size` bytes to `to`. False when the datagram could not be handed
    /// to the OS at all; a true return says nothing about arrival.
    bool send(Address to, const void* data, u32 size);

    /// Read one waiting datagram into `data`, writing its sender to `from`.
    /// Returns the byte count, or 0 when nothing was waiting.
    [[nodiscard]] u32 receive(Address* from, void* data, u32 capacity);

    /// The bound port, which is what an ephemeral bind is for.
    [[nodiscard]] u16 port() const { return port_; }
    [[nodiscard]] bool valid() const { return handle_ >= 0; }

private:
    int handle_ = -1;
    u16 port_ = 0;
};

/// Marks our datagrams as ours, so a stray packet on a reused port is dropped
/// rather than parsed as a header.
constexpr u32 PROTOCOL_ID = 0x554d4348u;  // 'UMCH'

/// The header every packet carries, ahead of the caller's payload.
struct Header {
    u32 protocol;   ///< PROTOCOL_ID, or the packet is not ours.
    u16 sequence;   ///< This packet's number, incrementing and wrapping.
    u16 ack;        ///< Highest sequence seen from the far end.
    u32 ack_bits;   ///< Bit i: we also received (ack - 1 - i).
};

static_assert(sizeof(Header) == 12, "Header layout must stay wire-stable");

/// True when `a` is the more recent of two wrapping sequence numbers. Plain
/// comparison breaks the moment the counter wraps past 65535, which at 60Hz is
/// every eighteen minutes.
[[nodiscard]] bool sequence_newer(u16 a, u16 b);

/// One end of a virtual connection: what we have sent, what we have heard, and
/// what the far end has told us it heard. Pure bookkeeping — it never touches a
/// socket, which is what makes the acknowledgement logic testable without one.
class Connection {
public:
    /// Bits of history in the ack field, so a packet is judged lost only after
    /// this many later packets have been acknowledged without it.
    static constexpr u32 ACK_HISTORY = 32;

    /// Fill in the header for the next outgoing packet and consume its
    /// sequence number.
    [[nodiscard]] Header next_header();

    /// Fold a received header in: record its sequence in our ack window, and
    /// learn which of our packets it acknowledges. Returns false when the
    /// packet is not ours or is a duplicate we have already recorded.
    bool on_received(const Header& header);

    /// True when our packet `sequence` has been acknowledged by the far end.
    /// Only meaningful for the last ACK_HISTORY packets; older ones fall out of
    /// the window and read false forever.
    [[nodiscard]] bool acked(u16 sequence) const;

    [[nodiscard]] u16 local_sequence() const { return local_sequence_; }
    [[nodiscard]] u16 remote_sequence() const { return remote_sequence_; }

private:
    u16 local_sequence_ = 0;   ///< Next sequence we will send.
    u16 remote_sequence_ = 0;  ///< Highest sequence we have received.
    u32 received_bits_ = 0;    ///< History behind remote_sequence_.
    u16 acked_sequence_ = 0;   ///< Highest of ours the far end has acknowledged.
    u32 acked_bits_ = 0;       ///< History behind acked_sequence_.
    bool have_remote_ = false;
    bool have_acked_ = false;
};

}  // namespace net
