#pragma once

#include <cstdint>
#include <string_view>

#include "loss_observatory/core/status.hpp"

namespace loss_observatory {

/// How precisely a piece of evidence can place loss.
///
/// The numeric ordering is significant and is the ordering used by every
/// "never exceed the evidence" check in this runtime:
///
///   Unknown < Flow < Path < Hop < Link < Queue
///
/// A larger value means a *finer* claim. Localization may only produce a
/// granularity no finer than the coarsest granularity that its supporting
/// evidence actually supports. Evidence that is only flow-scoped can never
/// name a queue, no matter how suggestive the numbers look.
enum class Granularity : std::uint8_t {
  Unknown = 0,
  Flow = 1,
  Path = 2,
  Hop = 3,
  Link = 4,
  Queue = 5,
};

[[nodiscard]] std::string_view to_string(Granularity granularity) noexcept;
[[nodiscard]] Result<Granularity> parse_granularity(std::string_view text);

/// True when p lhs is a strictly finer claim than p rhs.
[[nodiscard]] constexpr bool is_finer_than(Granularity lhs, Granularity rhs) noexcept {
  return static_cast<std::uint8_t>(lhs) > static_cast<std::uint8_t>(rhs);
}

/// The finer of the two; Unknown loses to anything except Unknown.
[[nodiscard]] constexpr Granularity finer_of(Granularity lhs, Granularity rhs) noexcept {
  return static_cast<std::uint8_t>(lhs) >= static_cast<std::uint8_t>(rhs) ? lhs : rhs;
}

/// The coarser of the two. Used to clamp a claim to what evidence supports.
[[nodiscard]] constexpr Granularity coarser_of(Granularity lhs, Granularity rhs) noexcept {
  return static_cast<std::uint8_t>(lhs) <= static_cast<std::uint8_t>(rhs) ? lhs : rhs;
}

}  // namespace loss_observatory
