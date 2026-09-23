#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

namespace loss_observatory::checked {

/// Checked arithmetic for every size or counter derived from external input.
/// Each function returns false and leaves p out untouched when the operation
/// would overflow or lose information.

[[nodiscard]] constexpr bool add(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
  if (a > std::numeric_limits<std::uint64_t>::max() - b) {
    return false;
  }
  out = a + b;
  return true;
}

[[nodiscard]] constexpr bool sub(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
  if (a < b) {
    return false;
  }
  out = a - b;
  return true;
}

[[nodiscard]] constexpr bool mul(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
  if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) {
    return false;
  }
  out = a * b;
  return true;
}

[[nodiscard]] constexpr bool div(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
  if (b == 0) {
    return false;
  }
  out = a / b;
  return true;
}

/// Narrowing cast that fails instead of truncating.
template <class To, class From>
  requires(std::is_integral_v<To> && std::is_integral_v<From>)
[[nodiscard]] constexpr bool narrow(From value, To& out) noexcept {
  // std::cmp_* compares across mixed signedness without the usual arithmetic
  // conversions, so a narrowing cast can be decided exactly.
  if constexpr (std::is_signed_v<From>) {
    if (value < 0) {
      return false;
    }
  }
  if (std::cmp_greater(value, std::numeric_limits<To>::max())) {
    return false;
  }
  if (std::cmp_less(value, std::numeric_limits<To>::min())) {
    return false;
  }
  out = static_cast<To>(value);
  return true;
}

/// Saturating accumulator that records whether saturation happened. Used for
/// aggregate counters where an overflow must be reported, never hidden.
class SaturatingSum {
 public:
  constexpr SaturatingSum() noexcept = default;

  constexpr void add(std::uint64_t value) noexcept {
    if (std::numeric_limits<std::uint64_t>::max() - value_ < value) {
      value_ = std::numeric_limits<std::uint64_t>::max();
      saturated_ = true;
      return;
    }
    value_ += value;
  }

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool saturated() const noexcept { return saturated_; }
  constexpr void reset() noexcept {
    value_ = 0;
    saturated_ = false;
  }

 private:
  std::uint64_t value_{0};
  bool saturated_{false};
};

/// Scale p numerator / p denominator into basis points (0..10000) using
/// integer arithmetic only, so results are bit-identical on every platform.
/// Returns false when p denominator is zero (callers must treat that as
/// "undefined ratio", never as 0% or 100%).
[[nodiscard]] constexpr bool ratio_basis_points(std::uint64_t numerator, std::uint64_t denominator,
                                                std::uint32_t& out) noexcept {
  constexpr std::uint64_t kScale = 10000;
  if (denominator == 0) {
    return false;
  }
  if (numerator == 0) {
    out = 0;
    return true;
  }
  // Clamp before multiplying so the product cannot overflow.
  if (numerator >= denominator) {
    out = 10000;
    return true;
  }
  const std::uint64_t scaled = numerator * kScale;
  const std::uint64_t bp = scaled / denominator;
  out = static_cast<std::uint32_t>(bp > kScale ? kScale : bp);
  return true;
}

}  // namespace loss_observatory::checked
