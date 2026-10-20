// Tests for the wire protocol: encode/decode round trips, stream reassembly
// when frames are split across reads, malformed input, market-data batches.

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "lob/wire.hpp"

using namespace lob;
using namespace lob::wire;

static int g_failed = 0;
static int g_checks = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(cond)) {                                                           \
      ++g_failed;                                                            \
      std::fprintf(stderr, "  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    }                                                                        \
  } while (0)

static std::vector<std::uint8_t> sample_stream() {
  std::vector<std::uint8_t> s;
  s.reserve(256);
  encode_new(s, 1, Side::Sell, OrderType::IOC, -5, 70000);
  encode_cancel(s, 2, 0xABCDEF0123ULL);
  encode_ack(s, 3, 42, 7, true);
  encode_fill(s, 4, 42, 41, 100123, 9);
  encode_cancel_result(s, true, 5, 42);
  encode_cancel_result(s, false, 6, 43);
  encode_reject(s, 7);
  return s;
}

static void check_sample(const std::vector<Msg>& m) {
  CHECK(m.size() == 7);
  if (m.size() != 7) return;
  CHECK(m[0].type == kNew && m[0].seq == 1 && m[0].side == Side::Sell && m[0].order_type == OrderType::IOC && m[0].price == -5 &&
        m[0].qty == 70000);
  CHECK(m[1].type == kCancel && m[1].order_id == 0xABCDEF0123ULL);
  CHECK(m[2].type == kAck && m[2].order_id == 42 && m[2].qty == 7 && m[2].resting);
  CHECK(m[3].type == kFill && m[3].order_id == 42 && m[3].maker_id == 41 && m[3].price == 100123 && m[3].qty == 9);
  CHECK(m[4].type == kCancelled && m[4].seq == 5);
  CHECK(m[5].type == kCancelReject && m[5].order_id == 43);
  CHECK(m[6].type == kReject && m[6].seq == 7);
}

static void test_round_trip_whole() {
  auto s = sample_stream();
  FrameReader r;
  r.feed(s.data(), s.size());
  std::vector<Msg> got;
  CHECK(r.drain([&](const Msg& m) { got.push_back(m); }));
  check_sample(got);
  CHECK(r.buffered() == 0);
}

static void test_round_trip_byte_by_byte() {
  auto s = sample_stream();
  FrameReader r;
  std::vector<Msg> got;
  for (std::uint8_t b : s) {  // worst case: TCP delivers one byte at a time
    r.feed(&b, 1);
    CHECK(r.drain([&](const Msg& m) { got.push_back(m); }));
  }
  check_sample(got);
}

static void test_malformed() {
  {
    std::vector<std::uint8_t> s = {5, 0, 'Z', 1, 2, 3, 4};  // unknown type
    FrameReader r;
    r.feed(s.data(), s.size());
    CHECK(!r.drain([](const Msg&) {}));
  }
  {
    std::vector<std::uint8_t> s;
    encode_new(s, 1, Side::Buy, OrderType::Limit, 1, 1);
    s[2 + 1 + 8] = 7;  // invalid side byte
    FrameReader r;
    r.feed(s.data(), s.size());
    CHECK(!r.drain([](const Msg&) {}));
  }
  {
    std::vector<std::uint8_t> s = {0xFF, 0x7F};  // absurd length
    FrameReader r;
    r.feed(s.data(), s.size());
    CHECK(!r.drain([](const Msg&) {}));
  }
}

static void test_market_data_batch() {
  MdBatch b;
  for (int i = 0; i < 100; ++i) b.add(1000 + i, i % 2 ? Side::Sell : Side::Buy, 500 + i, static_cast<Qty>(i));
  CHECK(b.full());
  CHECK(b.size() == kMdHeader + 100 * kMdEntry);
  CHECK(b.size() < 1400);
  std::uint64_t first = 0;
  std::uint16_t count = 0;
  int seen = 0;
  bool ordered = true;
  CHECK(parse_md(b.data(), b.size(), first, count, [&](std::uint64_t seq, const MdEntry& e) {
    ordered = ordered && seq == 1000u + seen && e.price == 500 + seen && e.qty == static_cast<Qty>(seen);
    ++seen;
  }));
  CHECK(first == 1000 && count == 100 && seen == 100 && ordered);
  CHECK(!parse_md(b.data(), b.size() - 1, first, count, [](std::uint64_t, const MdEntry&) {}));  // truncated
}

int main() {
  struct { const char* name; void (*fn)(); } tests[] = {
      {"round_trip_whole", test_round_trip_whole},
      {"round_trip_byte_by_byte", test_round_trip_byte_by_byte},
      {"malformed", test_malformed},
      {"market_data_batch", test_market_data_batch},
  };
  for (auto& t : tests) {
    int before = g_failed;
    t.fn();
    std::printf("%s %s\n", g_failed == before ? "PASS" : "FAIL", t.name);
  }
  std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
