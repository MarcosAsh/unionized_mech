#include "session/link.h"

#include <cstring>

namespace session {

namespace {

/// Bytes ahead of the payload in every packet: the connection header and the
/// one byte saying what this is.
constexpr u32 PREFIX = static_cast<u32>(sizeof(net::Header)) + 1;

using HostResult = core::Result<ServerHost, const char*>;
using LinkResult = core::Result<ClientLink, const char*>;

}  // namespace

HostResult ServerHost::open(u16 port, u32 max_clients) {
    core::Result<net::Socket, const char*> sock = net::Socket::open(port);
    if (sock.is_err()) {
        return HostResult::err(sock.error());
    }
    ServerHost host;
    host.socket_ = static_cast<net::Socket&&>(sock.value());
    host.max_clients_ = max_clients < sim::MAX_PLAYERS ? max_clients : sim::MAX_PLAYERS;
    host.server_.start(host.max_clients_);
    return HostResult::ok(static_cast<ServerHost&&>(host));
}

u32 ServerHost::slot_for(net::Address from) {
    for (u32 i = 0; i < max_clients_; ++i) {
        if (peers_[i].live && peers_[i].address == from) {
            return i;
        }
    }
    for (u32 i = 0; i < max_clients_; ++i) {
        if (!peers_[i].live) {
            peers_[i].address = from;
            peers_[i].live = true;
            ++client_count_;
            return i;
        }
    }
    return sim::MAX_PLAYERS;  // full
}

void ServerHost::poll() {
    u8 buffer[net::MAX_PACKET];
    net::Address from{};
    for (;;) {
        const u32 got = socket_.receive(&from, buffer, sizeof(buffer));
        if (got == 0) {
            break;  // nothing waiting
        }
        if (got < PREFIX + sizeof(InputHeader) || buffer[sizeof(net::Header)] !=
                                                      static_cast<u8>(Packet::Input)) {
            continue;  // not an input packet, or too short to be one
        }
        net::Header header{};
        std::memcpy(&header, buffer, sizeof(header));
        // Check the packet is ours *before* handing out a slot. Adopting the
        // sender first means anyone can take a player slot with a garbage
        // datagram, and a handful of them fills the match.
        if (header.protocol != net::PROTOCOL_ID) {
            continue;
        }
        const u32 slot = slot_for(from);
        if (slot >= sim::MAX_PLAYERS) {
            continue;  // match is full
        }
        if (!peers_[slot].connection.on_received(header)) {
            continue;  // a duplicate, or out of the ack window
        }
        InputHeader input{};
        std::memcpy(&input, buffer + PREFIX, sizeof(input));
        sim::InputCmd cmd{};
        std::memcpy(&cmd, buffer + PREFIX + sizeof(InputHeader), sizeof(cmd));
        server_.on_input(slot, input.tick, cmd, input.baseline);
    }
}

void ServerHost::tick() { server_.tick(); }

void ServerHost::broadcast() {
    u8 buffer[net::MAX_PACKET];
    for (u32 i = 0; i < max_clients_; ++i) {
        if (!peers_[i].live) {
            continue;
        }
        const net::Header header = peers_[i].connection.next_header();
        std::memcpy(buffer, &header, sizeof(header));
        buffer[sizeof(net::Header)] = static_cast<u8>(Packet::Snapshot);
        const u32 body = server_.write_snapshot(i, buffer + PREFIX,
                                                static_cast<u32>(sizeof(buffer)) - PREFIX);
        if (body == 0) {
            continue;  // would not fit; skip this client this tick
        }
        (void)socket_.send(peers_[i].address, buffer, PREFIX + body);
    }
}

LinkResult ClientLink::open(net::Address server, u32 slot, u32 clients) {
    core::Result<net::Socket, const char*> sock = net::Socket::open(0);
    if (sock.is_err()) {
        return LinkResult::err(sock.error());
    }
    ClientLink link;
    link.socket_ = static_cast<net::Socket&&>(sock.value());
    link.server_ = server;
    link.client_.start(slot, clients);
    return LinkResult::ok(static_cast<ClientLink&&>(link));
}

void ClientLink::send(const sim::InputCmd& cmd) {
    const u32 tick = client_.predict(cmd);

    u8 buffer[PREFIX + sizeof(InputHeader) + sizeof(sim::InputCmd)];
    const net::Header header = connection_.next_header();
    std::memcpy(buffer, &header, sizeof(header));
    buffer[sizeof(net::Header)] = static_cast<u8>(Packet::Input);
    InputHeader input{};
    input.tick = tick;
    input.baseline = client_.baseline();
    std::memcpy(buffer + PREFIX, &input, sizeof(input));
    std::memcpy(buffer + PREFIX + sizeof(InputHeader), &cmd, sizeof(cmd));
    (void)socket_.send(server_, buffer, sizeof(buffer));
}

u32 ClientLink::poll() {
    u8 buffer[net::MAX_PACKET];
    u32 applied = 0;
    for (;;) {
        const u32 got = socket_.receive(nullptr, buffer, sizeof(buffer));
        if (got == 0) {
            break;
        }
        if (got < PREFIX + sizeof(SnapshotHeader) || buffer[sizeof(net::Header)] !=
                                                         static_cast<u8>(Packet::Snapshot)) {
            continue;
        }
        net::Header header{};
        std::memcpy(&header, buffer, sizeof(header));
        if (!connection_.on_received(header)) {
            continue;
        }
        SnapshotHeader snapshot{};
        std::memcpy(&snapshot, buffer + PREFIX, sizeof(snapshot));
        // A snapshot the client cannot use — diffed against a baseline it has
        // dropped, or already overtaken — is skipped, not fatal. The next one
        // will be diffed against something newer.
        if (client_.on_snapshot(snapshot, buffer + PREFIX + sizeof(SnapshotHeader),
                                got - PREFIX - static_cast<u32>(sizeof(SnapshotHeader)))) {
            ++applied;
        }
    }
    return applied;
}

}  // namespace session
