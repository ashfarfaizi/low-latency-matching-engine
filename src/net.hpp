#pragma once
// Small POSIX socket helpers shared by the server and tools (Linux).

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace lob::net {

[[noreturn]] inline void die(const char* what) {
  std::fprintf(stderr, "%s: %s\n", what, std::strerror(errno));
  std::exit(1);
}

inline void set_nonblocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) die("fcntl");
}

inline void set_nodelay(int fd) {
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));  // no Nagle: send small frames immediately
}

inline sockaddr_in make_addr(const std::string& host, std::uint16_t port) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  if (inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1) {
    std::fprintf(stderr, "bad address %s\n", host.c_str());
    std::exit(1);
  }
  return a;
}

inline int tcp_listen(std::uint16_t port) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) die("socket");
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in a = make_addr("0.0.0.0", port);
  if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) die("bind");
  if (listen(fd, 64) < 0) die("listen");
  set_nonblocking(fd);
  return fd;
}

inline int tcp_connect(const std::string& host, std::uint16_t port) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) die("socket");
  sockaddr_in a = make_addr(host, port);
  for (int attempt = 0; connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0; ++attempt) {
    if (attempt > 50) die("connect");
    usleep(100000);  // server may still be starting
  }
  set_nodelay(fd);
  return fd;
}

/// UDP socket for publishing. Multicast destinations (224.0.0.0/4) get TTL 1 + loopback.
inline int udp_sender(const std::string& host) {
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) die("socket");
  const sockaddr_in a = make_addr(host, 0);
  if ((ntohl(a.sin_addr.s_addr) >> 28) == 0xE) {
    unsigned char ttl = 1, loop = 1;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
  }
  int sndbuf = 4 << 20;
  setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
  return fd;
}

/// UDP socket for receiving on `port`; joins `group` if it is a multicast address.
inline int udp_receiver(const std::string& group, std::uint16_t port) {
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) die("socket");
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  int rcvbuf = 8 << 20;
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
  sockaddr_in a = make_addr("0.0.0.0", port);
  if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) die("bind udp");
  const sockaddr_in g = make_addr(group, port);
  if ((ntohl(g.sin_addr.s_addr) >> 28) == 0xE) {
    ip_mreq m{};
    m.imr_multiaddr = g.sin_addr;
    m.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof(m)) < 0) die("IP_ADD_MEMBERSHIP");
  }
  return fd;
}

/// Writes all bytes to a blocking socket.
inline bool write_all(int fd, const std::uint8_t* p, std::size_t n) {
  while (n) {
    const ssize_t w = ::send(fd, p, n, MSG_NOSIGNAL);
    if (w < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    p += w;
    n -= static_cast<std::size_t>(w);
  }
  return true;
}

}  // namespace lob::net
