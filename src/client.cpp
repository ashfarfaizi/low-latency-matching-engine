// lob_client: load / latency tool for lob_server.
//
//   lob_client latency [n=200000] [host=127.0.0.1] [port=9000]
//       One order in flight: send, wait for its Ack / Cancelled, record the TCP
//       round trip (client -> kernel -> gateway -> engine -> gateway -> kernel -> client).
//   lob_client flood [n=2000000] [host] [port]
//       Pipelined: a writer streams orders in batches while a reader counts replies.
//   lob_client selftest [_] [host] [port]
//       Two sessions; checks order ownership, maker fill notification, cancels. Exit 0 = pass.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "lob/wire.hpp"
#include "net.hpp"

using namespace lob;
using Clock = std::chrono::steady_clock;

namespace {

constexpr Price kMid = 50000;

struct OrderGen {
  std::mt19937_64 rng{1234};
  std::uint64_t seq = 0;
  // ~55% passive limit, ~30% cancel of a live order, ~15% aggressive IOC
  void next(std::vector<std::uint8_t>& out, std::vector<OrderId>& live, bool& is_cancel) {
    const unsigned r = rng() % 100;
    is_cancel = false;
    if (r < 30 && !live.empty()) {
      const std::size_t k = rng() % live.size();
      wire::encode_cancel(out, ++seq, live[k]);
      live[k] = live.back();
      live.pop_back();
      is_cancel = true;
    } else if (r < 85) {
      const Side s = (rng() & 1) ? Side::Buy : Side::Sell;
      const Price p = s == Side::Buy ? kMid - 1 - static_cast<Price>(rng() % 200) : kMid + 1 + static_cast<Price>(rng() % 200);
      wire::encode_new(out, ++seq, s, OrderType::Limit, p, 1 + static_cast<Qty>(rng() % 100));
    } else {
      const Side s = (rng() & 1) ? Side::Buy : Side::Sell;
      wire::encode_new(out, ++seq, s, OrderType::IOC, s == Side::Buy ? kMid + 210 : kMid - 210, 50 + static_cast<Qty>(rng() % 250));
    }
  }
};

void print_pct(const char* name, std::vector<double>& v) {
  if (v.empty()) return;
  std::sort(v.begin(), v.end());
  auto p = [&](double q) { return v[std::min(v.size() - 1, static_cast<std::size_t>(q * v.size()))]; };
  std::printf("%-10s %8zu  p50 %6.1f  p90 %6.1f  p99 %6.1f  p99.9 %7.1f us\n", name, v.size(), p(0.5), p(0.9), p(0.99), p(0.999));
}

int latency(std::size_t n, const std::string& host, std::uint16_t port) {
  const int fd = net::tcp_connect(host, port);
  OrderGen gen;
  std::vector<OrderId> live;
  std::vector<std::uint8_t> out;
  wire::FrameReader reader;
  std::vector<double> rt_new, rt_cancel;
  rt_new.reserve(n);
  rt_cancel.reserve(n);
  std::uint8_t buf[4096];

  for (std::size_t i = 0; i < n; ++i) {
    out.clear();
    bool is_cancel = false;
    gen.next(out, live, is_cancel);
    const std::uint64_t want = gen.seq;
    const auto t0 = Clock::now();
    if (!net::write_all(fd, out.data(), out.size())) net::die("send");
    bool done = false;
    while (!done) {
      const ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
      if (r <= 0) net::die("recv");
      reader.feed(buf, static_cast<std::size_t>(r));
      reader.drain([&](const wire::Msg& m) {
        if (m.seq != want) return;  // unsolicited maker fills (seq 0) etc.
        if (m.type == wire::kAck) {
          if (m.resting) live.push_back(m.order_id);
          done = true;
        } else if (m.type == wire::kCancelled || m.type == wire::kCancelReject || m.type == wire::kReject) {
          done = true;
        }
      });
    }
    const double us = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
    (is_cancel ? rt_cancel : rt_new).push_back(us);
  }
  std::printf("TCP round trip over loopback, one order in flight\n");
  print_pct("new", rt_new);
  print_pct("cancel", rt_cancel);
  ::close(fd);
  return 0;
}

int flood(std::size_t n, const std::string& host, std::uint16_t port) {
  const int fd = net::tcp_connect(host, port);
  std::atomic<std::size_t> replies{0};
  std::atomic<bool> stop{false};
  std::uint64_t fills = 0;
  // Reader thread: count final replies (Ack / Cancelled / CancelReject / Reject).
  std::thread reader_thread([&] {
    wire::FrameReader reader;
    std::uint8_t buf[256 * 1024];
    while (!stop) {
      const ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
      if (r <= 0) break;
      reader.feed(buf, static_cast<std::size_t>(r));
      reader.drain([&](const wire::Msg& m) {
        if (m.type == wire::kFill) ++fills;
        else if (m.seq != 0) replies.fetch_add(1, std::memory_order_relaxed);
      });
    }
  });

  // Cancels in flood mode target ids we cannot know yet, so the writer only
  // sends New orders here (passive + aggressive mix).
  OrderGen gen;
  std::vector<OrderId> no_live;
  std::vector<std::uint8_t> out;
  out.reserve(64 * 40);
  const auto t0 = Clock::now();
  std::size_t sent = 0;
  while (sent < n) {
    out.clear();
    for (int k = 0; k < 64 && sent < n; ++k, ++sent) {
      bool is_cancel;
      gen.next(out, no_live, is_cancel);
    }
    if (!net::write_all(fd, out.data(), out.size())) net::die("send");
  }
  while (replies.load() < n) std::this_thread::yield();
  const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
  stop = true;
  ::shutdown(fd, SHUT_RDWR);
  reader_thread.join();
  ::close(fd);
  std::printf("flood: %zu orders acknowledged in %.2fs -> %.2f M orders/s over one TCP connection (%llu fills)\n", n, secs,
              n / secs / 1e6, static_cast<unsigned long long>(fills));
  return 0;
}

// Blocking request/response helper for selftest.
struct Session {
  int fd;
  wire::FrameReader reader;
  std::vector<wire::Msg> inbox;
  explicit Session(int f) : fd(f) {}
  void send(const std::vector<std::uint8_t>& b) {
    if (!net::write_all(fd, b.data(), b.size())) net::die("send");
  }
  // Read until a message matching pred arrives (others are kept in inbox).
  wire::Msg wait(const std::function<bool(const wire::Msg&)>& pred) {
    for (;;) {
      for (std::size_t i = 0; i < inbox.size(); ++i)
        if (pred(inbox[i])) {
          wire::Msg m = inbox[i];
          inbox.erase(inbox.begin() + static_cast<std::ptrdiff_t>(i));
          return m;
        }
      std::uint8_t buf[4096];
      const ssize_t r = ::recv(fd, buf, sizeof(buf), 0);
      if (r <= 0) net::die("recv");
      reader.feed(buf, static_cast<std::size_t>(r));
      reader.drain([&](const wire::Msg& m) { inbox.push_back(m); });
    }
  }
};

int selftest(const std::string& host, std::uint16_t port) {
  Session a(net::tcp_connect(host, port)), b(net::tcp_connect(host, port));
  int failures = 0;
  auto expect = [&](bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    failures += ok ? 0 : 1;
  };
  std::vector<std::uint8_t> out;
  const Price px = 77777;

  // A rests a sell.
  out.clear();
  wire::encode_new(out, 1, Side::Sell, OrderType::Limit, px, 10);
  a.send(out);
  auto ack = a.wait([](const wire::Msg& m) { return m.type == wire::kAck && m.seq == 1; });
  expect(ack.resting && ack.qty == 0, "A's sell order rests in the book");

  // B tries to cancel A's order -> must be rejected by the gateway.
  out.clear();
  wire::encode_cancel(out, 2, ack.order_id);
  b.send(out);
  auto r1 = b.wait([](const wire::Msg& m) { return m.seq == 2; });
  expect(r1.type == wire::kCancelReject, "B cannot cancel A's order");

  // B buys 4 against it: B gets a taker fill, A gets an unsolicited maker fill.
  out.clear();
  wire::encode_new(out, 3, Side::Buy, OrderType::IOC, px, 4);
  b.send(out);
  auto bf = b.wait([](const wire::Msg& m) { return m.type == wire::kFill && m.seq == 3; });
  auto back = b.wait([](const wire::Msg& m) { return m.type == wire::kAck && m.seq == 3; });
  expect(bf.qty == 4 && bf.price == px && bf.maker_id == ack.order_id, "B's buy fills 4 @ A's price");
  expect(back.qty == 4 && !back.resting, "B's IOC ack reports 4 filled, nothing resting");
  auto af = a.wait([&](const wire::Msg& m) { return m.type == wire::kFill && m.seq == 0; });
  expect(af.order_id == ack.order_id && af.qty == 4, "A is notified that its resting order filled 4");

  // A cancels the remaining 6 -> succeeds; cancelling again -> rejected.
  out.clear();
  wire::encode_cancel(out, 4, ack.order_id);
  a.send(out);
  expect(a.wait([](const wire::Msg& m) { return m.seq == 4; }).type == wire::kCancelled, "A cancels its own remaining order");
  out.clear();
  wire::encode_cancel(out, 5, ack.order_id);
  a.send(out);
  expect(a.wait([](const wire::Msg& m) { return m.seq == 5; }).type == wire::kCancelReject, "double cancel is rejected");

  // Invalid price is rejected, not crashed on.
  out.clear();
  wire::encode_new(out, 6, Side::Buy, OrderType::Limit, 0, 1);
  a.send(out);
  expect(a.wait([](const wire::Msg& m) { return m.seq == 6; }).type == wire::kReject, "out-of-range price is rejected");

  ::close(a.fd);
  ::close(b.fd);
  std::printf("\n%s (%d failed)\n", failures ? "SELFTEST FAILED" : "selftest passed", failures);
  return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "latency";
  const std::size_t n = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : (mode == "flood" ? 2000000 : 200000);
  const std::string host = argc > 3 ? argv[3] : "127.0.0.1";
  const auto port = static_cast<std::uint16_t>(argc > 4 ? std::atoi(argv[4]) : 9000);
  if (mode == "latency") return latency(n, host, port);
  if (mode == "flood") return flood(n, host, port);
  if (mode == "selftest") return selftest(host, port);
  std::fprintf(stderr, "usage: lob_client latency|flood|selftest [n] [host] [port]\n");
  return 2;
}
