#pragma once
// Append-only command journal + event digest, for deterministic replay.
//
// The engine is a pure function of its input sequence: the same commands in
// the same order always produce the same events. The gateway is the single
// producer feeding the engine, so if it journals every command it sends, the
// journal *is* the engine's input. Replaying it through a fresh engine must
// reproduce the exact event stream, which we check with a running FNV-1a digest
// over every event (live digest printed by the server == replay digest).
//
// File format: 8-byte header "LOBJRNL1", then raw 56-byte Command records
// (host byte order; the journal is for the machine that wrote it).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "lob/messages.hpp"

namespace lob {

inline constexpr char kJournalMagic[8] = {'L', 'O', 'B', 'J', 'R', 'N', 'L', '1'};

class JournalWriter {
 public:
  explicit JournalWriter(const std::string& path) : buf_(1 << 20) {
    f_ = std::fopen(path.c_str(), "wb");
    if (!f_) throw std::runtime_error("cannot open journal " + path);
    std::setvbuf(f_, buf_.data(), _IOFBF, buf_.size());  // big user-space buffer: one syscall per MB
    std::fwrite(kJournalMagic, 1, sizeof(kJournalMagic), f_);
  }
  ~JournalWriter() {
    if (f_) std::fclose(f_);
  }
  JournalWriter(const JournalWriter&) = delete;
  JournalWriter& operator=(const JournalWriter&) = delete;

  void append(const Command& c) {
    Command copy = c;
    copy.ts = 0;  // timestamps are not part of the engine's logical input
    std::fwrite(&copy, sizeof(copy), 1, f_);
    ++count_;
  }
  void flush() { std::fflush(f_); }
  std::uint64_t count() const { return count_; }

 private:
  std::FILE* f_ = nullptr;
  std::vector<char> buf_;
  std::uint64_t count_ = 0;
};

class JournalReader {
 public:
  explicit JournalReader(const std::string& path) {
    f_ = std::fopen(path.c_str(), "rb");
    if (!f_) throw std::runtime_error("cannot open journal " + path);
    char magic[8];
    if (std::fread(magic, 1, 8, f_) != 8 || std::memcmp(magic, kJournalMagic, 8) != 0)
      throw std::runtime_error("not a journal file: " + path);
  }
  ~JournalReader() {
    if (f_) std::fclose(f_);
  }
  JournalReader(const JournalReader&) = delete;
  JournalReader& operator=(const JournalReader&) = delete;

  bool next(Command& c) { return std::fread(&c, sizeof(c), 1, f_) == 1; }

 private:
  std::FILE* f_ = nullptr;
};

/// Running 64-bit FNV-1a digest over the logical content of events
/// (timestamps excluded, since they differ between live and replay).
class EventDigest {
 public:
  void add(const Event& e) {
    mix(static_cast<std::uint64_t>(e.type));
    mix(static_cast<std::uint64_t>(e.side));
    mix(e.resting);
    mix(e.client_seq);
    mix(e.session);
    mix(e.order_id);
    mix(e.maker_id);
    mix(static_cast<std::uint64_t>(e.price));
    mix(e.qty);
    mix(e.md_seq);
    ++count_;
  }
  std::uint64_t value() const { return h_; }
  std::uint64_t count() const { return count_; }

 private:
  void mix(std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
      h_ ^= (v >> (i * 8)) & 0xFF;
      h_ *= 0x100000001B3ULL;
    }
  }
  std::uint64_t h_ = 0xCBF29CE484222325ULL;
  std::uint64_t count_ = 0;
};

}  // namespace lob
