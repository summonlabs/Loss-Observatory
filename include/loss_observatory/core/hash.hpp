#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>

#include "loss_observatory/core/status.hpp"

namespace loss_observatory {

/// FNV-1a over bytes. Chosen for stability across platforms and processes:
/// identity values derived here must be reproducible from the same canonical
/// text everywhere, otherwise persisted references would not survive restart.
[[nodiscard]] constexpr std::uint64_t fnv1a64(std::string_view text) noexcept {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const char ch : text) {
    hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
    hash *= 1099511628211ULL;
  }
  return hash;
}

[[nodiscard]] constexpr std::uint64_t fnv1a64_bytes(const std::uint8_t* data, std::size_t size) noexcept {
  std::uint64_t hash = 1469598103934665603ULL;
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= static_cast<std::uint64_t>(data[i]);
    hash *= 1099511628211ULL;
  }
  return hash;
}

/// Splitmix64 finaliser: avalanche without a lookup table.
[[nodiscard]] constexpr std::uint64_t mix64(std::uint64_t value) noexcept {
  value += 0x9E3779B97F4A7C15ULL;
  value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31);
}

[[nodiscard]] constexpr std::uint64_t combine_hash(std::uint64_t seed, std::uint64_t value) noexcept {
  return mix64(seed ^ mix64(value));
}

/// 16 lowercase hex digits. Wide enough that accidental collisions in a single
/// deployment are not a practical concern; identity collisions are additionally
/// detected by the registry, which reports them rather than merging silently.
[[nodiscard]] std::string hex_u64(std::uint64_t value);

/// Parses exactly 16 lowercase/uppercase hex digits. Rejects anything else,
/// including shorter strings, so a typo can never alias a different identity.
[[nodiscard]] Result<std::uint64_t> parse_hex_u64(std::string_view text);

/// Strongly typed opaque identity.
///
/// p Tag is an incomplete tag type declared per domain concept, which makes
/// every identity a distinct type: a HopId cannot be passed where a LinkId is
/// expected, and no conversion between identity kinds exists.
template <class Tag>
class Id {
 public:
  using tag_type = Tag;
  using value_type = std::uint64_t;

  constexpr Id() noexcept = default;

  [[nodiscard]] static constexpr Id from_value(std::uint64_t value) noexcept { return Id{value}; }

  /// Deterministic identity derived from canonical text. Used when a caller
  /// supplies a symbolic name instead of a numeric identity.
  [[nodiscard]] static constexpr Id from_canonical_text(std::string_view text) noexcept {
    return Id{fnv1a64(text)};
  }

  [[nodiscard]] static Result<Id> parse(std::string_view text) {
    auto parsed = parse_hex_u64(text);
    if (!parsed.ok()) {
      return parsed.status();
    }
    return Id{parsed.value()};
  }

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_nil() const noexcept { return value_ == 0; }
  [[nodiscard]] std::string to_string() const { return hex_u64(value_); }

  friend constexpr bool operator==(Id lhs, Id rhs) noexcept { return lhs.value_ == rhs.value_; }
  friend constexpr bool operator!=(Id lhs, Id rhs) noexcept { return lhs.value_ != rhs.value_; }
  friend constexpr bool operator<(Id lhs, Id rhs) noexcept { return lhs.value_ < rhs.value_; }
  friend constexpr bool operator>(Id lhs, Id rhs) noexcept { return lhs.value_ > rhs.value_; }
  friend constexpr bool operator<=(Id lhs, Id rhs) noexcept { return lhs.value_ <= rhs.value_; }
  friend constexpr bool operator>=(Id lhs, Id rhs) noexcept { return lhs.value_ >= rhs.value_; }

 private:
  explicit constexpr Id(std::uint64_t value) noexcept : value_(value) {}

  std::uint64_t value_{0};
};

}  // namespace loss_observatory

namespace std {

template <class Tag>
struct hash<loss_observatory::Id<Tag>> {
  [[nodiscard]] std::size_t operator()(const loss_observatory::Id<Tag>& id) const noexcept {
    return static_cast<std::size_t>(loss_observatory::mix64(id.value()));
  }
};

}  // namespace std
