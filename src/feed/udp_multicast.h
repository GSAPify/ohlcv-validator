#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

#include "feed/feed_protocol.h"

namespace ohlcv::feed {

// Thin RAII wrappers over POSIX UDP multicast sockets -- the transport an
// exchange market-data feed actually uses (the venue publishes to a multicast
// group; every subscriber on the network receives the same datagrams without the
// venue fanning out N TCP streams). One group per line; the receiver joins both A
// and B and the arbitrator merges them.
//
// A datagram is exactly one FeedPacket sent raw. Same-host loopback is little-
// endian on both ends, so no byte-swapping here; a cross-architecture production
// feed would put the header fields in network byte order. (POSIX only -- Linux
// and macOS; this isn't a Windows codebase.)

namespace detail {
[[noreturn]] inline void fail(const char* what) {
    throw std::runtime_error(std::string(what) + ": " + std::strerror(errno));
}
}  // namespace detail

// Joins a multicast group and receives FeedPackets from it.
class McastReceiver {
public:
    McastReceiver(const std::string& group, std::uint16_t port,
                  const std::string& iface = "127.0.0.1") {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) detail::fail("socket");

        int yes = 1;
        if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0)
            detail::fail("SO_REUSEADDR");

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
            detail::fail("bind");

        ip_mreq mreq{};
        mreq.imr_multiaddr.s_addr = ::inet_addr(group.c_str());
        mreq.imr_interface.s_addr = ::inet_addr(iface.c_str());
        if (::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0)
            detail::fail("IP_ADD_MEMBERSHIP");
    }

    ~McastReceiver() { if (fd_ >= 0) ::close(fd_); }
    McastReceiver(const McastReceiver&) = delete;
    McastReceiver& operator=(const McastReceiver&) = delete;

    [[nodiscard]] int fd() const noexcept { return fd_; }

    // Receive one packet. Returns true iff a full FeedPacket arrived (a short or
    // oversized datagram is rejected rather than partially interpreted).
    bool recv(FeedPacket& out) {
        const ssize_t n = ::recv(fd_, &out, sizeof(out), 0);
        return n == static_cast<ssize_t>(sizeof(out));
    }

private:
    int fd_ = -1;
};

// Sends FeedPackets to a multicast group.
class McastSender {
public:
    McastSender(const std::string& group, std::uint16_t port,
                const std::string& iface = "127.0.0.1") {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) detail::fail("socket");

        in_addr ifaddr{};
        ifaddr.s_addr = ::inet_addr(iface.c_str());
        if (::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_IF, &ifaddr, sizeof(ifaddr)) < 0)
            detail::fail("IP_MULTICAST_IF");
        int loop = 1;  // deliver our own datagrams back on this host (loopback demo)
        if (::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) < 0)
            detail::fail("IP_MULTICAST_LOOP");

        dst_.sin_family = AF_INET;
        dst_.sin_port = htons(port);
        dst_.sin_addr.s_addr = ::inet_addr(group.c_str());
    }

    ~McastSender() { if (fd_ >= 0) ::close(fd_); }
    McastSender(const McastSender&) = delete;
    McastSender& operator=(const McastSender&) = delete;

    bool send(const FeedPacket& pkt) {
        const ssize_t n = ::sendto(fd_, &pkt, sizeof(pkt), 0,
                                   reinterpret_cast<sockaddr*>(&dst_), sizeof(dst_));
        return n == static_cast<ssize_t>(sizeof(pkt));
    }

private:
    int fd_ = -1;
    sockaddr_in dst_{};
};

}  // namespace ohlcv::feed
