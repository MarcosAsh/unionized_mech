#pragma once

// The wire. `session.h` is the loop — who is authoritative, what gets predicted,
// what gets replayed — and says nothing about sockets, which is what lets the
// tests drive it with exact latency and loss. This is the other half: the same
// loop with a real UDP socket under it.
//
// Packet layout, both directions:
//
//     [net::Header][u8 kind][InputHeader | SnapshotHeader][payload]
//
// The net header carries the sequence numbers and acknowledgements; everything
// after it is the session's business.

#include "core/types.h"
#include "net/net.h"
#include "session/session.h"

namespace session {

/// What a datagram is. A byte rather than a separate port or socket, so one
/// connection carries everything.
enum class Packet : u8 {
    Input = 1,     ///< Client to server: one command.
    Snapshot = 2,  ///< Server to client: a delta against a baseline it holds.
};

/// The authoritative server with a socket under it. Clients are recognised by
/// address and handed the next free slot; there is no handshake beyond that
/// because the first packet from an address is as good an announcement as any.
class ServerHost {
public:
    /// Bind to `port` and start a match with room for `max_clients` people.
    /// # Errors
    /// A static message when the socket cannot be opened.
    [[nodiscard]] static core::Result<ServerHost, const char*> open(u16 port, u32 max_clients);

    /// Read everything waiting and feed it to the simulation. Cheap when the
    /// socket is empty, which is the common case between ticks.
    void poll();

    /// Advance the simulation one tick.
    void tick();

    /// Send each connected client the snapshot it is owed. Separate from `tick`
    /// so a server can run the simulation faster than it talks.
    void broadcast();

    [[nodiscard]] const sim::World& world() const { return server_.world(); }
    [[nodiscard]] u32 client_count() const { return client_count_; }

    /// The client tick last heard from `slot`; see `Server::last_input`.
    [[nodiscard]] u32 last_input(u32 slot) const { return server_.last_input(slot); }
    [[nodiscard]] u16 port() const { return socket_.port(); }

private:
    /// One connected player: where to reach them and what they have heard.
    struct Peer {
        net::Address address;
        net::Connection connection;
        bool live = false;
    };

    /// The slot for `from`, adopting a new client when there is room. Returns
    /// MAX_PLAYERS when the match is full, and the caller drops the packet.
    [[nodiscard]] u32 slot_for(net::Address from);

    Server server_;
    net::Socket socket_;
    Peer peers_[sim::MAX_PLAYERS];
    u32 client_count_ = 0;
    u32 max_clients_ = 1;
};

/// The client with a socket under it.
class ClientLink {
public:
    /// Bind an ephemeral port and aim at `server`. `slot` and `clients` have to
    /// match what the server will run, or the two simulate different worlds
    /// from the same inputs.
    /// # Errors
    /// A static message when the socket cannot be opened.
    [[nodiscard]] static core::Result<ClientLink, const char*> open(net::Address server, u32 slot,
                                                                    u32 clients);

    /// Predict `cmd` locally and send it. The prediction is what gets drawn;
    /// the send is what the server will eventually agree or disagree with.
    void send(const sim::InputCmd& cmd);

    /// Apply every snapshot waiting. Returns how many were applied, which is 0
    /// on a quiet tick and more than 1 after a stall.
    u32 poll();

    [[nodiscard]] const sim::World& world() const { return client_.world(); }
    [[nodiscard]] const sim::Character& me() const { return client_.me(); }
    [[nodiscard]] u16 port() const { return socket_.port(); }

private:
    Client client_;
    net::Socket socket_;
    net::Connection connection_;
    net::Address server_{};
};

}  // namespace session
