// lob_md_listen: receives the UDP market-data feed, checks that sequence
// numbers are contiguous (reports gaps = dropped datagrams), and rebuilds the
// top of book from level updates.
//
//   lob_md_listen [group=127.0.0.1] [port=9001] [idle_exit_seconds=3]

#include <sys/socket.h>
#include <sys/time.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>

#include "lob/wire.hpp"
#include "net.hpp"

using namespace lob;

namespace {
volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }
}  // namespace

int main(int argc, char** argv) {
  const std::string group = argc > 1 ? argv[1] : "127.0.0.1";
  const auto port = static_cast<std::uint16_t>(argc > 2 ? std::atoi(argv[2]) : 9001);
  const int idle_exit = argc > 3 ? std::atoi(argv[3]) : 3;
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  const int fd = net::udp_receiver(group, port);
  timeval tv{1, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  std::printf("lob_md_listen: listening on udp %s:%u\n", group.c_str(), port);
  std::fflush(stdout);

  std::map<Price, Qty, std::greater<Price>> bids;
  std::map<Price, Qty> asks;
  std::uint64_t expected = 0, datagrams = 0, updates = 0, gaps = 0, missing = 0, malformed = 0;
  int idle = 0;
  bool started = false;
  std::uint8_t buf[2048];

  while (!g_stop) {
    const ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
    if (r < 0) {
      if (started && ++idle >= idle_exit) break;  // feed went quiet
      continue;
    }
    idle = 0;
    started = true;
    std::uint64_t first = 0;
    std::uint16_t count = 0;
    const bool ok = wire::parse_md(buf, static_cast<std::size_t>(r), first, count, [&](std::uint64_t, const wire::MdEntry& e) {
      ++updates;
      if (e.side == Side::Buy) {
        if (e.qty) bids[e.price] = e.qty; else bids.erase(e.price);
      } else {
        if (e.qty) asks[e.price] = e.qty; else asks.erase(e.price);
      }
    });
    if (!ok) {
      ++malformed;
      continue;
    }
    ++datagrams;
    if (expected != 0 && first != expected) {
      ++gaps;
      if (first > expected) missing += first - expected;
    }
    expected = first + count;
  }

  std::printf("lob_md_listen: %llu datagrams, %llu level updates, last seq %llu, gaps %llu (%llu updates missed), malformed %llu\n",
              (unsigned long long)datagrams, (unsigned long long)updates, (unsigned long long)(expected ? expected - 1 : 0),
              (unsigned long long)gaps, (unsigned long long)missing, (unsigned long long)malformed);
  if (!bids.empty() && !asks.empty())
    std::printf("rebuilt top of book: bid %lld x %u | ask %lld x %u  (%zu bid levels, %zu ask levels)\n",
                (long long)bids.begin()->first, bids.begin()->second, (long long)asks.begin()->first, asks.begin()->second,
                bids.size(), asks.size());
  return gaps ? 1 : 0;
}
