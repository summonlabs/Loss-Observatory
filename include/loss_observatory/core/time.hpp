#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

#include "loss_observatory/core/status.hpp"

namespace loss_observatory {

/// Nanoseconds since the Unix epoch, UTC. Signed because the runtime must be
/// able to represent and reason about pre-epoch and negative skew values
/// instead of silently wrapping them.
class Timestamp {
 public:
  constexpr Timestamp() noexcept = default;

  [[nodiscard]] static constexpr Timestamp from_unix_nanos(std::int64_t nanos) noexcept {
    return Timestamp{nanos};
  }
  [[nodiscard]] static constexpr Timestamp from_unix_micros(std::int64_t micros) noexcept {
    return Timestamp{micros * 1000};
  }
  [[nodiscard]] static constexpr Timestamp from_unix_millis(std::int64_t millis) noexcept {
    return Timestamp{millis * 1000000};
  }
  [[nodiscard]] static constexpr Timestamp from_unix_seconds(std::int64_t seconds) noexcept {
    return Timestamp{seconds * 1000000000};
  }

  [[nodiscard]] constexpr std::int64_t unix_nanos() const noexcept { return nanos_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return nanos_ == 0; }

  friend constexpr bool operator==(Timestamp lhs, Timestamp rhs) noexcept { return lhs.nanos_ == rhs.nanos_; }
  friend constexpr bool operator!=(Timestamp lhs, Timestamp rhs) noexcept { return lhs.nanos_ != rhs.nanos_; }
  friend constexpr bool operator<(Timestamp lhs, Timestamp rhs) noexcept { return lhs.nanos_ < rhs.nanos_; }
  friend constexpr bool operator>(Timestamp lhs, Timestamp rhs) noexcept { return lhs.nanos_ > rhs.nanos_; }
  friend constexpr bool operator<=(Timestamp lhs, Timestamp rhs) noexcept { return lhs.nanos_ <= rhs.nanos_; }
  friend constexpr bool operator>=(Timestamp lhs, Timestamp rhs) noexcept { return lhs.nanos_ >= rhs.nanos_; }

  /// ISO-8601 UTC with nanosecond precision: 2026-01-02T03:04:05.123456789Z
  [[nodiscard]] std::string to_string() const;

  /// Accepts the format produced by to_string(), plus a reduced form without
  /// fractional seconds. Rejects anything else rather than guessing.
  [[nodiscard]] static Result<Timestamp> parse_iso8601(std::string_view text);

 private:
  explicit constexpr Timestamp(std::int64_t nanos) noexcept : nanos_(nanos) {}

  std::int64_t nanos_{0};
};

class Duration {
 public:
  constexpr Duration() noexcept = default;

  [[nodiscard]] static constexpr Duration from_nanos(std::int64_t nanos) noexcept { return Duration{nanos}; }
  [[nodiscard]] static constexpr Duration from_micros(std::int64_t micros) noexcept { return Duration{micros * 1000}; }
  [[nodiscard]] static constexpr Duration from_millis(std::int64_t millis) noexcept {
    return Duration{millis * 1000000};
  }
  [[nodiscard]] static constexpr Duration from_seconds(std::int64_t seconds) noexcept {
    return Duration{seconds * 1000000000};
  }

  [[nodiscard]] constexpr std::int64_t nanos() const noexcept { return nanos_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return nanos_ == 0; }
  [[nodiscard]] constexpr bool is_negative() const noexcept { return nanos_ < 0; }
  [[nodiscard]] constexpr Duration abs() const noexcept { return Duration{nanos_ < 0 ? -nanos_ : nanos_}; }

  friend constexpr bool operator==(Duration lhs, Duration rhs) noexcept { return lhs.nanos_ == rhs.nanos_; }
  friend constexpr bool operator!=(Duration lhs, Duration rhs) noexcept { return lhs.nanos_ != rhs.nanos_; }
  friend constexpr bool operator<(Duration lhs, Duration rhs) noexcept { return lhs.nanos_ < rhs.nanos_; }
  friend constexpr bool operator>(Duration lhs, Duration rhs) noexcept { return lhs.nanos_ > rhs.nanos_; }
  friend constexpr bool operator<=(Duration lhs, Duration rhs) noexcept { return lhs.nanos_ <= rhs.nanos_; }
  friend constexpr bool operator>=(Duration lhs, Duration rhs) noexcept { return lhs.nanos_ >= rhs.nanos_; }

  [[nodiscard]] std::string to_string() const;

 private:
  explicit constexpr Duration(std::int64_t nanos) noexcept : nanos_(nanos) {}

  std::int64_t nanos_{0};
};

[[nodiscard]] constexpr Timestamp operator+(Timestamp lhs, Duration rhs) noexcept {
  return Timestamp::from_unix_nanos(lhs.unix_nanos() + rhs.nanos());
}
[[nodiscard]] constexpr Timestamp operator-(Timestamp lhs, Duration rhs) noexcept {
  return Timestamp::from_unix_nanos(lhs.unix_nanos() - rhs.nanos());
}
[[nodiscard]] constexpr Duration operator-(Timestamp lhs, Timestamp rhs) noexcept {
  return Duration::from_nanos(lhs.unix_nanos() - rhs.unix_nanos());
}

/// Time source abstraction. Every time-dependent decision in the runtime reads
/// the clock through this interface so deterministic replay is possible and so
/// no subsystem can quietly reach for wall time.
class Clock {
 public:
  Clock() = default;
  virtual ~Clock() = default;
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;
  Clock(Clock&&) = delete;
  Clock& operator=(Clock&&) = delete;

  [[nodiscard]] virtual Timestamp now() const noexcept = 0;
};

class SystemClock final : public Clock {
 public:
  [[nodiscard]] Timestamp now() const noexcept override;
};

/// Deterministic clock for tests, replay, and benchmarks. Thread-safe.
class ManualClock final : public Clock {
 public:
  ManualClock() noexcept = default;
  explicit ManualClock(Timestamp start) noexcept : now_(start.unix_nanos()) {}

  [[nodiscard]] Timestamp now() const noexcept override {
    return Timestamp::from_unix_nanos(now_.load(std::memory_order_acquire));
  }

  void set(Timestamp value) noexcept { now_.store(value.unix_nanos(), std::memory_order_release); }

  void advance(Duration delta) noexcept {
    now_.fetch_add(delta.nanos(), std::memory_order_acq_rel);
  }

 private:
  std::atomic<std::int64_t> now_{0};
};

}  // namespace loss_observatory
