#pragma once
// Bounded lock-free single-producer / single-consumer ring buffer.
//
//  * Capacity is a power of two so the slot index is `seq & mask`.
//  * head (consumer) and tail (producer) are on separate cache lines to avoid
//    false sharing between the two cores.
//  * Each side caches the other side's index and only re-reads the shared
//    atomic when its cached copy says "full" / "empty". That keeps cross-core
//    cache traffic to a minimum in the common case.
//  * Memory ordering: the producer publishes a slot with a release store of
//    tail; the consumer acquires tail before reading the slot (and vice versa
//    for head), so the slot contents are always visible before the index.

#include <atomic>
#include <cstddef>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace lob {

inline constexpr std::size_t kCacheLine = 64;

template <class T>
class SpscQueue {
  static_assert(std::is_trivially_copyable_v<T>, "SpscQueue holds trivially copyable messages");

 public:
  explicit SpscQueue(std::size_t capacity_pow2)
      : capacity_(capacity_pow2), mask_(capacity_pow2 - 1), slots_(new T[capacity_pow2]) {
    if (capacity_pow2 < 2 || (capacity_pow2 & mask_) != 0) throw std::invalid_argument("capacity must be a power of two >= 2");
    // Touch every slot now so the hot path never page-faults.
    for (std::size_t i = 0; i < capacity_; ++i) slots_[i] = T{};
  }

  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;

  /// Producer side. Returns false if the queue is full.
  bool try_push(const T& value) noexcept {
    const std::size_t t = tail_.load(std::memory_order_relaxed);
    if (t - head_cache_ == capacity_) {
      head_cache_ = head_.load(std::memory_order_acquire);
      if (t - head_cache_ == capacity_) return false;
    }
    slots_[t & mask_] = value;
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }

  /// Consumer side. Returns false if the queue is empty.
  bool try_pop(T& out) noexcept {
    const std::size_t h = head_.load(std::memory_order_relaxed);
    if (h == tail_cache_) {
      tail_cache_ = tail_.load(std::memory_order_acquire);
      if (h == tail_cache_) return false;
    }
    out = slots_[h & mask_];
    head_.store(h + 1, std::memory_order_release);
    return true;
  }

  std::size_t capacity() const noexcept { return capacity_; }

  /// Approximate size (exact only when both sides are quiescent).
  std::size_t size_approx() const noexcept {
    return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire);
  }

 private:
  const std::size_t capacity_;
  const std::size_t mask_;
  std::unique_ptr<T[]> slots_;

  // Consumer-owned line.
  alignas(kCacheLine) std::atomic<std::size_t> head_{0};
  std::size_t tail_cache_ = 0;
  // Producer-owned line.
  alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
  std::size_t head_cache_ = 0;
  char pad_[kCacheLine - sizeof(std::size_t) * 2];
};

}  // namespace lob
