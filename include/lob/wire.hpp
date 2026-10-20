#pragma once
// Binary wire protocol.
//
// TCP order entry, every frame:  [u16 len][u8 type][payload ...]   (len = 1 + payload bytes)
//   client -> server   'N' NewOrder  seq u64, side u8, otype u8, price i64, qty u32      (22 B)
//                      'C' Cancel    seq u64, order_id u64                               (16 B)
//   server -> client   'A' Ack       seq u64, order_id u64, filled u32, resting u8       (21 B)
//                      'F' Fill      seq u64, order_id u64, maker u64, price i64, qty u32 (36 B)
//                      'X' Cancelled seq u64, order_id u64                               (16 B)
//                      'Y' CxlReject seq u64, order_id u64                               (16 B)
//                      'R' Reject    seq u64                                             (8 B)
//
// UDP market data, one datagram:  [u64 first_seq][u16 count] then count x [u8 side][i64 price][u32 qty]
//   Each entry is the new total quantity at a price level. Sequence numbers are
//   contiguous across datagrams, so a listener can detect drops.
//
// All integers are little-endian and encoded with memcpy (no packed structs,
// no alignment assumptions).

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "lob/types.hpp"

namespace lob::wire {

static_assert(std::endian::native == std::endian::little, "wire format assumes a little-endian host");

enum MsgType : std::uint8_t {
  kNew = 'N',
  kCancel = 'C',
  kAck = 'A',
  kFill = 'F',
  kCancelled = 'X',
  kCancelReject = 'Y',
  kReject = 'R',
};

inline constexpr std::size_t kHeader = 3;  // u16 len + u8 type
inline constexpr std::size_t kMdHeader = 10;
inline constexpr std::size_t kMdEntry = 13;
inline constexpr std::size_t kMdMaxEntries = 100;  // keeps datagrams < 1400 bytes

struct Writer {
  std::uint8_t* p;
  template <class T>
  void put(T v) {
    std::memcpy(p, &v, sizeof(T));
    p += sizeof(T);
  }
};

struct Reader {
  const std::uint8_t* p;
  template <class T>
  T get() {
    T v;
    std::memcpy(&v, p, sizeof(T));
    p += sizeof(T);
    return v;
  }
};

/// Appends one frame of `type` with `payload_len` bytes to `out`, returns a writer positioned at the payload.
inline Writer begin_frame(std::vector<std::uint8_t>& out, std::uint8_t type, std::size_t payload_len) {
  const std::size_t at = out.size();
  out.resize(at + kHeader + payload_len);
  Writer w{out.data() + at};
  w.put<std::uint16_t>(static_cast<std::uint16_t>(1 + payload_len));
  w.put<std::uint8_t>(type);
  return w;
}

inline void encode_new(std::vector<std::uint8_t>& out, std::uint64_t seq, Side side, OrderType ot, Price price, Qty qty) {
  Writer w = begin_frame(out, kNew, 22);
  w.put(seq);
  w.put(static_cast<std::uint8_t>(side));
  w.put(static_cast<std::uint8_t>(ot));
  w.put(price);
  w.put(qty);
}
inline void encode_cancel(std::vector<std::uint8_t>& out, std::uint64_t seq, OrderId id) {
  Writer w = begin_frame(out, kCancel, 16);
  w.put(seq);
  w.put(id);
}
inline void encode_ack(std::vector<std::uint8_t>& out, std::uint64_t seq, OrderId id, Qty filled, bool resting) {
  Writer w = begin_frame(out, kAck, 21);
  w.put(seq);
  w.put(id);
  w.put(filled);
  w.put(static_cast<std::uint8_t>(resting));
}
inline void encode_fill(std::vector<std::uint8_t>& out, std::uint64_t seq, OrderId id, OrderId maker, Price price, Qty qty) {
  Writer w = begin_frame(out, kFill, 36);
  w.put(seq);
  w.put(id);
  w.put(maker);
  w.put(price);
  w.put(qty);
}
inline void encode_cancel_result(std::vector<std::uint8_t>& out, bool ok, std::uint64_t seq, OrderId id) {
  Writer w = begin_frame(out, ok ? kCancelled : kCancelReject, 16);
  w.put(seq);
  w.put(id);
}
inline void encode_reject(std::vector<std::uint8_t>& out, std::uint64_t seq) {
  Writer w = begin_frame(out, kReject, 8);
  w.put(seq);
}

/// Decoded view of any frame (fields not used by a type are left at 0).
struct Msg {
  std::uint8_t type = 0;
  std::uint64_t seq = 0;
  Side side = Side::Buy;
  OrderType order_type = OrderType::Limit;
  Price price = 0;
  Qty qty = 0;
  OrderId order_id = 0;
  OrderId maker_id = 0;
  bool resting = false;
};

inline std::size_t payload_size(std::uint8_t type) {
  switch (type) {
    case kNew: return 22;
    case kCancel: return 16;
    case kAck: return 21;
    case kFill: return 36;
    case kCancelled:
    case kCancelReject: return 16;
    case kReject: return 8;
    default: return 0;
  }
}

/// Decodes a payload. Returns false for unknown types or wrong sizes.
inline bool decode(std::uint8_t type, const std::uint8_t* payload, std::size_t len, Msg& m) {
  if (payload_size(type) == 0 || payload_size(type) != len) return false;
  Reader r{payload};
  m = Msg{};
  m.type = type;
  m.seq = r.get<std::uint64_t>();
  switch (type) {
    case kNew: {
      const auto s = r.get<std::uint8_t>();
      const auto t = r.get<std::uint8_t>();
      if (s > 1 || t > 1) return false;
      m.side = static_cast<Side>(s);
      m.order_type = static_cast<OrderType>(t);
      m.price = r.get<Price>();
      m.qty = r.get<Qty>();
      break;
    }
    case kCancel:
    case kCancelled:
    case kCancelReject: m.order_id = r.get<OrderId>(); break;
    case kAck:
      m.order_id = r.get<OrderId>();
      m.qty = r.get<Qty>();
      m.resting = r.get<std::uint8_t>() != 0;
      break;
    case kFill:
      m.order_id = r.get<OrderId>();
      m.maker_id = r.get<OrderId>();
      m.price = r.get<Price>();
      m.qty = r.get<Qty>();
      break;
    default: break;
  }
  return true;
}

/// Reassembles frames from a TCP byte stream (handles frames split across reads).
class FrameReader {
 public:
  /// Append received bytes.
  void feed(const std::uint8_t* data, std::size_t n) { buf_.insert(buf_.end(), data, data + n); }

  /// Calls on_msg(const Msg&) for every complete frame. Returns false on a
  /// malformed frame (caller should drop the connection).
  template <class OnMsg>
  bool drain(OnMsg&& on_msg) {
    std::size_t off = 0;
    bool ok = true;
    while (buf_.size() - off >= 2) {
      std::uint16_t len;
      std::memcpy(&len, buf_.data() + off, 2);
      if (len < 1 || len > 64) {
        ok = false;
        break;
      }
      if (buf_.size() - off < 2u + len) break;  // incomplete frame: wait for more bytes
      Msg m;
      const std::uint8_t type = buf_[off + 2];
      if (!decode(type, buf_.data() + off + kHeader, len - 1u, m)) {
        ok = false;
        break;
      }
      on_msg(m);
      off += 2u + len;
    }
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(off));
    return ok;
  }

  std::size_t buffered() const { return buf_.size(); }

 private:
  std::vector<std::uint8_t> buf_;
};

// ── UDP market data ────────────────────────────────────────────────────────
struct MdEntry {
  Side side;
  Price price;
  Qty qty;
};

/// Builds datagrams of up to kMdMaxEntries level updates.
class MdBatch {
 public:
  MdBatch() { buf_.reserve(kMdHeader + kMdMaxEntries * kMdEntry); }
  bool empty() const { return count_ == 0; }
  bool full() const { return count_ == kMdMaxEntries; }
  void add(std::uint64_t seq, Side side, Price price, Qty qty) {
    if (count_ == 0) {
      buf_.resize(kMdHeader);
      Writer w{buf_.data()};
      w.put(seq);
      w.put<std::uint16_t>(0);
    }
    const std::size_t at = buf_.size();
    buf_.resize(at + kMdEntry);
    Writer w{buf_.data() + at};
    w.put(static_cast<std::uint8_t>(side));
    w.put(price);
    w.put(qty);
    ++count_;
    std::memcpy(buf_.data() + 8, &count_, 2);
  }
  const std::uint8_t* data() const { return buf_.data(); }
  std::size_t size() const { return buf_.size(); }
  void clear() {
    count_ = 0;
    buf_.clear();
  }

 private:
  std::vector<std::uint8_t> buf_;
  std::uint16_t count_ = 0;
};

/// Parses one datagram. Returns false if malformed.
template <class OnEntry>
bool parse_md(const std::uint8_t* data, std::size_t n, std::uint64_t& first_seq, std::uint16_t& count, OnEntry&& on_entry) {
  if (n < kMdHeader) return false;
  Reader r{data};
  first_seq = r.get<std::uint64_t>();
  count = r.get<std::uint16_t>();
  if (n != kMdHeader + static_cast<std::size_t>(count) * kMdEntry) return false;
  for (std::uint16_t i = 0; i < count; ++i) {
    MdEntry e;
    const auto s = r.get<std::uint8_t>();
    if (s > 1) return false;
    e.side = static_cast<Side>(s);
    e.price = r.get<Price>();
    e.qty = r.get<Qty>();
    on_entry(first_seq + i, e);
  }
  return true;
}

}  // namespace lob::wire
