// lob_server: TCP order entry + UDP market data around the matching engine.
//
//   clients ──TCP──▶ gateway thread (epoll) ──SPSC──▶ engine thread (pinned, busy-poll)
//   clients ◀──TCP── gateway thread         ◀──SPSC── engine thread
//   listeners ◀──UDP── market data (batched level updates, gapless sequence numbers)
//
// Gateway responsibilities: framing, per-session routing, order ownership
// (a session can only cancel its own orders), maker fill notifications, and
// batching market data into datagrams. Polling is adaptive: while commands
// are in flight the gateway spins on epoll with a 0 ms timeout; when idle it
// blocks for up to 1 ms so it does not burn a core.
//
// Every command sent to the engine can be journaled (--journal file) and every
// event is folded into a digest, so `lob_replay file` can prove the live run
// is reproduced exactly.
//
// Usage: lob_server [--port 9000] [--md 127.0.0.1:9001] [--gw-cpu 0] [--engine-cpu 1] [--journal path]

#include <sys/epoll.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "lob/engine.hpp"
#include "lob/journal.hpp"
#include "lob/wire.hpp"
#include "net.hpp"

using namespace lob;

namespace {

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

struct Conn {
  int fd = -1;
  std::uint64_t session = 0;
  wire::FrameReader reader;
  std::vector<std::uint8_t> out;
  bool want_write = false;
  bool dirty = false;
  bool alive = true;
};

struct Stats {
  std::uint64_t commands = 0, acks = 0, fills = 0, cancels = 0, rejects = 0, owner_rejects = 0;
  std::uint64_t md_updates = 0, md_datagrams = 0, sessions = 0;
};

class Gateway {
 public:
  Gateway(MatchingEngine& eng, int listen_fd, int md_fd, sockaddr_in md_dst, JournalWriter* journal)
      : eng_(eng), listen_fd_(listen_fd), md_fd_(md_fd), md_dst_(md_dst), journal_(journal) {
    ep_ = epoll_create1(0);
    if (ep_ < 0) net::die("epoll_create1");
    add(listen_fd_, EPOLLIN, nullptr);
    sessions_.push_back(nullptr);  // session 0 unused
    owner_.assign(1 << 20, 0);
  }

  void run() {
    epoll_event evs[64];
    while (!g_stop) {
      const int timeout_ms = outstanding_ ? 0 : 1;
      const int n = epoll_wait(ep_, evs, 64, timeout_ms);
      if (n < 0 && errno != EINTR) net::die("epoll_wait");
      for (int i = 0; i < n; ++i) {
        if (evs[i].data.ptr == nullptr) {
          accept_all();
          continue;
        }
        Conn* c = static_cast<Conn*>(evs[i].data.ptr);
        if (evs[i].events & (EPOLLERR | EPOLLHUP)) {
          close_conn(c);
          continue;
        }
        if (evs[i].events & EPOLLIN) read_conn(c);
        if (c->alive && (evs[i].events & EPOLLOUT)) flush_conn(c);
        if (c->alive && (evs[i].events & EPOLLRDHUP)) close_conn(c);
      }
      pump_events();
      flush_dirty();
      flush_md();
      if (journal_ && !outstanding_ && n == 0) journal_->flush();  // idle: push journal to the OS
    }
    // Shutdown: collect every event the engine still owes us so the digest is complete.
    auto idle_since = std::chrono::steady_clock::now();
    while (outstanding_ || std::chrono::steady_clock::now() - idle_since < std::chrono::milliseconds(50)) {
      const auto before = digest_.count();
      pump_events();
      if (digest_.count() != before) idle_since = std::chrono::steady_clock::now();
    }
    flush_dirty();
    flush_md();
    if (journal_) journal_->flush();
  }

  const Stats& stats() const { return st_; }
  const EventDigest& digest() const { return digest_; }

 private:
  void add(int fd, std::uint32_t events, void* ptr) {
    epoll_event ev{};
    ev.events = events;
    ev.data.ptr = ptr;
    if (epoll_ctl(ep_, EPOLL_CTL_ADD, fd, &ev) < 0) net::die("epoll_ctl add");
  }
  void modify(Conn* c, std::uint32_t events) {
    epoll_event ev{};
    ev.events = events;
    ev.data.ptr = c;
    epoll_ctl(ep_, EPOLL_CTL_MOD, c->fd, &ev);
  }

  void accept_all() {
    for (;;) {
      const int fd = accept(listen_fd_, nullptr, nullptr);
      if (fd < 0) return;  // EAGAIN: no more pending
      net::set_nonblocking(fd);
      net::set_nodelay(fd);
      auto c = std::make_unique<Conn>();
      c->fd = fd;
      c->session = sessions_.size();
      add(fd, EPOLLIN | EPOLLRDHUP, c.get());
      sessions_.push_back(c.get());
      conns_.push_back(std::move(c));
      ++st_.sessions;
    }
  }

  void close_conn(Conn* c) {
    if (!c->alive) return;
    c->alive = false;
    epoll_ctl(ep_, EPOLL_CTL_DEL, c->fd, nullptr);
    ::close(c->fd);
    sessions_[c->session] = nullptr;  // late events for this session are dropped
  }

  void read_conn(Conn* c) {
    std::uint8_t buf[64 * 1024];
    for (;;) {
      const ssize_t r = ::recv(c->fd, buf, sizeof(buf), 0);
      if (r > 0) {
        c->reader.feed(buf, static_cast<std::size_t>(r));
        continue;
      }
      if (r == 0) {
        close_conn(c);
        return;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      if (errno == EINTR) continue;
      close_conn(c);
      return;
    }
    const bool ok = c->reader.drain([&](const wire::Msg& m) { on_client_msg(c, m); });
    if (!ok) close_conn(c);  // protocol violation
  }

  void send_to_engine(const Command& cmd) {
    while (!eng_.try_send(cmd)) pump_events();  // never block while the engine waits on us
    if (journal_) journal_->append(cmd);           // single producer: journal order == engine order
    ++outstanding_;
    ++st_.commands;
  }

  void on_client_msg(Conn* c, const wire::Msg& m) {
    Command cmd{};
    cmd.client_seq = m.seq;
    cmd.session = c->session;
    if (m.type == wire::kNew) {
      cmd.type = Command::Type::New;
      cmd.side = m.side;
      cmd.order_type = m.order_type;
      cmd.price = m.price;
      cmd.qty = m.qty;
      send_to_engine(cmd);
    } else if (m.type == wire::kCancel) {
      if (m.order_id >= owner_.size() || owner_[m.order_id] != c->session) {
        wire::encode_cancel_result(c->out, false, m.seq, m.order_id);  // not yours (or unknown)
        mark_dirty(c);
        ++st_.owner_rejects;
        return;
      }
      cmd.type = Command::Type::Cancel;
      cmd.cancel_id = m.order_id;
      send_to_engine(cmd);
    } else {
      close_conn(c);  // clients may not send server message types
    }
  }

  Conn* session(std::uint64_t s) { return s < sessions_.size() ? sessions_[s] : nullptr; }

  void mark_dirty(Conn* c) {
    if (!c->dirty) {
      c->dirty = true;
      dirty_.push_back(c);
    }
  }

  void pump_events() {
    Event e;
    while (eng_.poll(e)) {
      digest_.add(e);
      switch (e.type) {
        case Event::Type::Ack: {
          --outstanding_;
          ++st_.acks;
          if (e.resting) {
            if (e.order_id >= owner_.size()) owner_.resize(owner_.size() * 2, 0);
            owner_[e.order_id] = static_cast<std::uint32_t>(e.session);
          }
          if (Conn* c = session(e.session)) {
            wire::encode_ack(c->out, e.client_seq, e.order_id, e.qty, e.resting);
            mark_dirty(c);
          }
          break;
        }
        case Event::Type::Fill: {
          ++st_.fills;
          if (Conn* c = session(e.session)) {  // taker
            wire::encode_fill(c->out, e.client_seq, e.order_id, e.maker_id, e.price, e.qty);
            mark_dirty(c);
          }
          const std::uint64_t maker_session = e.maker_id < owner_.size() ? owner_[e.maker_id] : 0;
          if (Conn* m = session(maker_session)) {  // maker: seq 0 = unsolicited fill
            wire::encode_fill(m->out, 0, e.maker_id, e.maker_id, e.price, e.qty);
            mark_dirty(m);
          }
          break;
        }
        case Event::Type::Cancelled:
        case Event::Type::CancelReject:
          --outstanding_;
          ++st_.cancels;
          if (Conn* c = session(e.session)) {
            wire::encode_cancel_result(c->out, e.type == Event::Type::Cancelled, e.client_seq, e.order_id);
            mark_dirty(c);
          }
          break;
        case Event::Type::Reject:
          --outstanding_;
          ++st_.rejects;
          if (Conn* c = session(e.session)) {
            wire::encode_reject(c->out, e.client_seq);
            mark_dirty(c);
          }
          break;
        case Event::Type::LevelUpdate:
          md_.add(e.md_seq, e.side, e.price, e.qty);
          ++st_.md_updates;
          if (md_.full()) flush_md();
          break;
      }
    }
  }

  void flush_conn(Conn* c) {
    std::size_t sent = 0;
    while (sent < c->out.size()) {
      const ssize_t w = ::send(c->fd, c->out.data() + sent, c->out.size() - sent, MSG_NOSIGNAL | MSG_DONTWAIT);
      if (w > 0) {
        sent += static_cast<std::size_t>(w);
        continue;
      }
      if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
      if (w < 0 && errno == EINTR) continue;
      close_conn(c);
      return;
    }
    c->out.erase(c->out.begin(), c->out.begin() + static_cast<std::ptrdiff_t>(sent));
    const bool need = !c->out.empty();
    if (need != c->want_write) {
      c->want_write = need;
      modify(c, EPOLLIN | EPOLLRDHUP | (need ? EPOLLOUT : 0u));
    }
  }

  void flush_dirty() {
    for (Conn* c : dirty_) {
      c->dirty = false;
      if (c->alive) flush_conn(c);
    }
    dirty_.clear();
  }

  void flush_md() {
    if (md_.empty()) return;
    ::sendto(md_fd_, md_.data(), md_.size(), MSG_DONTWAIT, reinterpret_cast<const sockaddr*>(&md_dst_), sizeof(md_dst_));
    ++st_.md_datagrams;
    md_.clear();
  }

  MatchingEngine& eng_;
  int listen_fd_;
  int md_fd_;
  sockaddr_in md_dst_;
  int ep_ = -1;
  std::vector<std::unique_ptr<Conn>> conns_;
  std::vector<Conn*> sessions_;
  std::vector<Conn*> dirty_;
  std::vector<std::uint32_t> owner_;  // order id -> session that owns it
  std::uint64_t outstanding_ = 0;    // commands sent to the engine without a reply yet
  wire::MdBatch md_;
  JournalWriter* journal_;
  EventDigest digest_;
  Stats st_;
};

std::string arg(int argc, char** argv, const char* name, const char* def) {
  for (int i = 1; i + 1 < argc; ++i)
    if (std::string(argv[i]) == name) return argv[i + 1];
  return def;
}

}  // namespace

int main(int argc, char** argv) {
  const auto port = static_cast<std::uint16_t>(std::stoi(arg(argc, argv, "--port", "9000")));
  const std::string md = arg(argc, argv, "--md", "127.0.0.1:9001");
  const int gw_cpu = std::stoi(arg(argc, argv, "--gw-cpu", "0"));
  const int engine_cpu = std::stoi(arg(argc, argv, "--engine-cpu", "1"));
  const std::string journal_path = arg(argc, argv, "--journal", "");
  const auto colon = md.find(':');
  const std::string md_host = md.substr(0, colon);
  const auto md_port = static_cast<std::uint16_t>(std::stoi(md.substr(colon + 1)));

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  std::signal(SIGPIPE, SIG_IGN);

  EngineConfig cfg;
  cfg.min_price = 1;
  cfg.max_price = 100000;
  cfg.max_resting_orders = 1 << 21;
  cfg.expected_order_ids = 1 << 23;
  cfg.queue_capacity = 1 << 16;
  MatchingEngine engine(cfg);
  std::thread engine_thread([&] { engine.run(engine_cpu); });
  pin_current_thread(gw_cpu);

  const int lfd = net::tcp_listen(port);
  const int mfd = net::udp_sender(md_host);
  std::unique_ptr<JournalWriter> journal;
  if (!journal_path.empty()) journal = std::make_unique<JournalWriter>(journal_path);
  Gateway gw(engine, lfd, mfd, net::make_addr(md_host, md_port), journal.get());
  std::printf("lob_server: orders on tcp :%u, market data to udp %s:%u, gateway cpu %d, engine cpu %d\n", port, md_host.c_str(),
              md_port, gw_cpu, engine_cpu);
  std::fflush(stdout);
  gw.run();

  Command stop{};
  stop.type = Command::Type::Stop;
  engine.send(stop);
  engine_thread.join();
  const Stats& s = gw.stats();
  std::printf(
      "lob_server stopped: sessions %llu, commands %llu, acks %llu, fills %llu, cancels %llu, rejects %llu, "
      "not-owner cancel rejects %llu, md updates %llu in %llu datagrams\n",
      (unsigned long long)s.sessions, (unsigned long long)s.commands, (unsigned long long)s.acks, (unsigned long long)s.fills,
      (unsigned long long)s.cancels, (unsigned long long)s.rejects, (unsigned long long)s.owner_rejects,
      (unsigned long long)s.md_updates, (unsigned long long)s.md_datagrams);
  std::printf("event digest %016llx over %llu events%s\n", (unsigned long long)gw.digest().value(),
              (unsigned long long)gw.digest().count(), journal ? " (journal written)" : "");
  return 0;
}
