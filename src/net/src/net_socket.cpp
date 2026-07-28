#include "net/net.h"

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

// The OS half: a non-blocking UDP socket. POSIX only for now, which is what
// this project targets; a Winsock variant slots in beside it when it needs one.

namespace net {

namespace {

using SocketResult = core::Result<Socket, const char*>;

}  // namespace

Address Address::loopback(u16 port) {
    Address a;
    a.host = 0x7F000001u;  // 127.0.0.1
    a.port = port;
    return a;
}

Address Address::any(u16 port) {
    Address a;
    a.host = 0;
    a.port = port;
    return a;
}

SocketResult Socket::open(u16 port) {
    const int handle = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (handle < 0) {
        return SocketResult::err("net: socket() failed");
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(handle, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(handle);
        return SocketResult::err("net: bind() failed");
    }

    // Non-blocking, so the frame loop can drain the socket and move on rather
    // than parking on an empty queue.
    const int flags = ::fcntl(handle, F_GETFL, 0);
    if (flags < 0 || ::fcntl(handle, F_SETFL, flags | O_NONBLOCK) < 0) {
        ::close(handle);
        return SocketResult::err("net: could not set non-blocking");
    }

    // Read back the port, which is the whole point of binding to 0.
    sockaddr_in bound{};
    socklen_t bound_len = sizeof(bound);
    if (::getsockname(handle, reinterpret_cast<sockaddr*>(&bound), &bound_len) < 0) {
        ::close(handle);
        return SocketResult::err("net: getsockname() failed");
    }

    Socket s;
    s.handle_ = handle;
    s.port_ = ntohs(bound.sin_port);
    return SocketResult::ok(static_cast<Socket&&>(s));
}

Socket::~Socket() {
    if (handle_ >= 0) {
        ::close(handle_);
        handle_ = -1;
    }
}

Socket::Socket(Socket&& other) noexcept : handle_(other.handle_), port_(other.port_) {
    other.handle_ = -1;
    other.port_ = 0;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        if (handle_ >= 0) {
            ::close(handle_);
        }
        handle_ = other.handle_;
        port_ = other.port_;
        other.handle_ = -1;
        other.port_ = 0;
    }
    return *this;
}

bool Socket::send(Address to, const void* data, u32 size) {
    if (handle_ < 0 || size > MAX_PACKET) {
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(to.host);
    addr.sin_port = htons(to.port);
    const ssize_t sent = ::sendto(handle_, data, size, 0,
                                  reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    return sent == static_cast<ssize_t>(size);
}

u32 Socket::receive(Address* from, void* data, u32 capacity) {
    if (handle_ < 0) {
        return 0;
    }
    sockaddr_in addr{};
    socklen_t addr_len = sizeof(addr);
    const ssize_t got = ::recvfrom(handle_, data, capacity, 0,
                                   reinterpret_cast<sockaddr*>(&addr), &addr_len);
    if (got <= 0) {
        return 0;  // EAGAIN on an empty queue, which is the common case
    }
    if (from != nullptr) {
        from->host = ntohl(addr.sin_addr.s_addr);
        from->port = ntohs(addr.sin_port);
    }
    return static_cast<u32>(got);
}

}  // namespace net
