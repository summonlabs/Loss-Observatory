#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "loss_observatory/core/checked.hpp"

namespace loss_observatory {

/// Every externally driven collection in the runtime is bounded, and every
/// bound that actually bites is reported. Silent truncation would be
/// indistinguishable from a genuine absence of evidence, which this runtime
/// must never conflate.
enum class BoundKind : std::uint8_t {
  None = 0,
  QueueDepth,
  EvidenceItems,
  EvidencePerSubject,
  DistinctSubjects,
  DistinctSources,
  TopologyEntities,
  HistoryEpisodes,
  AggregationWindows,
  AggregationSources,
  ResultSet,
  PayloadBytes,
  MetadataBytes,
  TextBytes,
  PersistenceBytes,
  PersistenceRecords,
  OpenEpisodes,
  ExplanationReasons,
  ExplanationClaims,
  ExportItems,
};

[[nodiscard]] std::string_view to_string(BoundKind kind) noexcept;

/// Records that a configured bound was reached, together with what was
/// observed. Included verbatim in classification, localization, and export
/// output so a caller can distinguish "nothing there" from "we stopped
/// looking".
struct BoundNote {
  BoundKind kind{BoundKind::None};
  std::uint64_t limit{0};
  std::uint64_t observed{0};
  std::string subject{};

  friend bool operator==(const BoundNote& lhs, const BoundNote& rhs) {
    return lhs.kind == rhs.kind && lhs.limit == rhs.limit && lhs.observed == rhs.observed &&
           lhs.subject == rhs.subject;
  }
};

/// Appends a note at most once per (kind, subject) pair, keeping notes in
/// first-observed order so explanations are deterministic.
class BoundNotes {
 public:
  void add(BoundKind kind, std::uint64_t limit, std::uint64_t observed, std::string subject,
           std::size_t max_notes = 64);

  [[nodiscard]] bool empty() const noexcept { return notes_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return notes_.size(); }
  [[nodiscard]] const std::vector<BoundNote>& notes() const noexcept { return notes_; }
  [[nodiscard]] bool truncated() const noexcept { return truncated_; }
  void clear() noexcept;

 private:
  std::vector<BoundNote> notes_{};
  bool truncated_{false};
};

enum class PushOutcome : std::uint8_t {
  Accepted = 0,
  RejectedFull,
  RejectedClosed,
  RejectedInvalid,
};

[[nodiscard]] std::string_view to_string(PushOutcome outcome) noexcept;

struct QueueStats {
  std::uint64_t pushed{0};
  std::uint64_t popped{0};
  std::uint64_t rejected_full{0};
  std::uint64_t rejected_closed{0};
  std::uint64_t high_water{0};
};

/// Bounded multi-producer / multi-consumer queue with real cancellation.
///
/// Cancellation is expressed with std::stop_token, so a blocked producer or
/// consumer wakes and returns rather than waiting for work that will never
/// arrive. No function here blocks indefinitely without a stop token or an
/// explicit deadline.
template <class T>
class BoundedQueue {
 public:
  explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {}

  BoundedQueue(const BoundedQueue&) = delete;
  BoundedQueue& operator=(const BoundedQueue&) = delete;
  BoundedQueue(BoundedQueue&&) = delete;
  BoundedQueue& operator=(BoundedQueue&&) = delete;

  [[nodiscard]] PushOutcome try_push(T value) {
    {
      std::lock_guard<std::mutex> guard(mutex_);
      if (closed_) {
        ++stats_.rejected_closed;
        return PushOutcome::RejectedClosed;
      }
      if (queue_.size() >= capacity_) {
        ++stats_.rejected_full;
        return PushOutcome::RejectedFull;
      }
      queue_.push_back(std::move(value));
      const auto depth = static_cast<std::uint64_t>(queue_.size());
      if (depth > stats_.high_water) {
        stats_.high_water = depth;
      }
      ++stats_.pushed;
    }
    not_empty_.notify_one();
    return PushOutcome::Accepted;
  }

  /// Blocks until space is available, the queue is closed, or p token is
  /// stop-requested.
  [[nodiscard]] PushOutcome push(T value, std::stop_token token) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (closed_) {
      ++stats_.rejected_closed;
      return PushOutcome::RejectedClosed;
    }
    if (queue_.size() >= capacity_) {
      // Space that is already available is used immediately: requesting
      // cancellation must not discard work the queue can still hold.
      const bool room =
          not_full_.wait(lock, token, [this] { return closed_ || queue_.size() < capacity_; });
      if (!room || closed_) {
        ++stats_.rejected_closed;
        return PushOutcome::RejectedClosed;
      }
      if (queue_.size() >= capacity_) {
        ++stats_.rejected_full;
        return PushOutcome::RejectedFull;
      }
    }
    queue_.push_back(std::move(value));
    const auto depth = static_cast<std::uint64_t>(queue_.size());
    if (depth > stats_.high_water) {
      stats_.high_water = depth;
    }
    ++stats_.pushed;
    lock.unlock();
    not_empty_.notify_one();
    return PushOutcome::Accepted;
  }

  /// Waits for an item. Returns false when the queue is closed and drained, or
  /// when p token was stop-requested.
  [[nodiscard]] bool pop(T& out, std::stop_token token) {
    std::unique_lock<std::mutex> lock(mutex_);
    const bool available = not_empty_.wait(lock, token, [this] { return closed_ || !queue_.empty(); });
    if (!available) {
      return false;
    }
    if (queue_.empty()) {
      return false;
    }
    out = std::move(queue_.front());
    queue_.pop_front();
    ++stats_.popped;
    lock.unlock();
    not_full_.notify_one();
    return true;
  }

  /// Non-blocking pop.
  [[nodiscard]] bool try_pop(T& out) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (queue_.empty()) {
      return false;
    }
    out = std::move(queue_.front());
    queue_.pop_front();
    ++stats_.popped;
    return true;
  }

  /// Wakes every waiter and drains remaining items. Idempotent.
  void close() {
    {
      std::lock_guard<std::mutex> guard(mutex_);
      closed_ = true;
      queue_.clear();
    }
    not_empty_.notify_all();
    not_full_.notify_all();
  }

  /// Requests an orderly close that lets consumers drain what is already
  /// queued before they observe the closed state.
  void close_after_drain() {
    {
      std::lock_guard<std::mutex> guard(mutex_);
      draining_ = true;
      closed_ = true;
    }
    not_empty_.notify_all();
    not_full_.notify_all();
  }

  [[nodiscard]] bool closed() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return closed_;
  }

  [[nodiscard]] std::size_t size() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return queue_.size();
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

  [[nodiscard]] QueueStats stats() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return stats_;
  }

 private:
  mutable std::mutex mutex_{};
  std::condition_variable_any not_empty_{};
  std::condition_variable_any not_full_{};
  std::deque<T> queue_{};
  std::size_t capacity_{0};
  bool closed_{false};
  bool draining_{false};
  QueueStats stats_{};
};

/// Fixed-capacity history with explicit eviction accounting.
///
/// Eviction is counted so that "no episodes in range" can never be confused
/// with "episodes were evicted before you asked".
template <class T>
class RingHistory {
 public:
  explicit RingHistory(std::size_t capacity) : capacity_(capacity) {}

  /// Returns false when the item was dropped because capacity is zero.
  [[nodiscard]] bool push(T value) {
    if (capacity_ == 0) {
      ++dropped_;
      return false;
    }
    if (entries_.size() >= capacity_) {
      entries_.pop_front();
      ++evicted_;
    }
    entries_.push_back(std::move(value));
    ++pushed_;
    return true;
  }

  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] std::uint64_t pushed() const noexcept { return pushed_; }
  [[nodiscard]] std::uint64_t evicted() const noexcept { return evicted_; }
  [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_; }

  /// Oldest to newest.
  [[nodiscard]] std::vector<T> snapshot() const { return std::vector<T>(entries_.begin(), entries_.end()); }

  [[nodiscard]] const std::deque<T>& entries() const noexcept { return entries_; }

  void clear() noexcept { entries_.clear(); }

 private:
  std::deque<T> entries_{};
  std::size_t capacity_{0};
  std::uint64_t pushed_{0};
  std::uint64_t evicted_{0};
  std::uint64_t dropped_{0};
};

/// Checked accumulator that reports rather than hides saturation when summing
/// externally supplied counts.
class BoundedCounter {
 public:
  constexpr BoundedCounter() noexcept = default;

  void add(std::uint64_t value) noexcept { sum_.add(value); }

  [[nodiscard]] std::uint64_t value() const noexcept { return sum_.value(); }
  [[nodiscard]] bool saturated() const noexcept { return sum_.saturated(); }

  /// Convenience: records the saturation into p notes under p kind.
  void report_if_saturated(BoundNotes& notes, BoundKind kind, std::string subject) const {
    if (sum_.saturated()) {
      notes.add(kind, std::numeric_limits<std::uint64_t>::max(), sum_.value(), std::move(subject));
    }
  }

 private:
  checked::SaturatingSum sum_{};
};

}  // namespace loss_observatory
