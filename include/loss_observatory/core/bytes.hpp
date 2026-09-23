#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "loss_observatory/core/status.hpp"

namespace loss_observatory {

/// Byte-level encoding helpers for persistence and exports.
///
/// Every write is bounds checked and every read is bounds checked. A short or
/// oversized buffer produces a failing Status instead of reading past the end:
/// persistence decoding must treat a truncated or hostile file as data, not as
/// a memory-safety problem.

using ByteSpan = std::span<const std::uint8_t>;
using MutableByteSpan = std::span<std::uint8_t>;

/// Chainable, bounds-checked writer.
///
/// Writes never throw and never run past the end of the buffer: the first
/// write that does not fit latches an overflow flag which ok() reports. This
/// design is deliberate: a truncated record must be detectable, so failures are
/// recorded rather than propagated through every call site.
class ByteWriter {
 public:
  explicit ByteWriter(MutableByteSpan buffer) noexcept : buffer_(buffer) {}

  ByteWriter& u8(std::uint8_t value) noexcept;
  ByteWriter& u16(std::uint16_t value) noexcept;
  ByteWriter& u32(std::uint32_t value) noexcept;
  ByteWriter& u64(std::uint64_t value) noexcept;
  ByteWriter& i64(std::int64_t value) noexcept;
  ByteWriter& raw(ByteSpan bytes) noexcept;

  /// Length-prefixed byte string. Length is u32, so a payload above 4 GiB is
  /// rejected instead of truncated.
  ByteWriter& length_prefixed(ByteSpan bytes) noexcept;
  ByteWriter& text(std::string_view value) noexcept;

  [[nodiscard]] std::size_t size() const noexcept { return offset_; }
  [[nodiscard]] bool overflowed() const noexcept { return overflowed_; }
  [[nodiscard]] bool ok() const noexcept { return !overflowed_; }

 private:
  [[nodiscard]] bool reserve(std::size_t count) noexcept;

  MutableByteSpan buffer_;
  std::size_t offset_{0};
  bool overflowed_{false};
};

class ByteReader {
 public:
  explicit ByteReader(ByteSpan buffer) noexcept : buffer_(buffer) {}

  [[nodiscard]] bool u8(std::uint8_t& out) noexcept;
  [[nodiscard]] bool u16(std::uint16_t& out) noexcept;
  [[nodiscard]] bool u32(std::uint32_t& out) noexcept;
  [[nodiscard]] bool u64(std::uint64_t& out) noexcept;
  [[nodiscard]] bool i64(std::int64_t& out) noexcept;
  [[nodiscard]] bool raw(std::size_t count, ByteSpan& out) noexcept;
  [[nodiscard]] bool skip(std::size_t count) noexcept;

  /// Reads a u32 length prefix and returns exactly that many bytes.
  [[nodiscard]] bool length_prefixed(ByteSpan& out) noexcept;
  [[nodiscard]] bool text(std::string& out, std::size_t max_bytes) noexcept;

  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return buffer_.size() - offset_; }
  [[nodiscard]] bool at_end() const noexcept { return offset_ == buffer_.size(); }

 private:
  [[nodiscard]] bool take(std::size_t count, ByteSpan& out) noexcept;

  ByteSpan buffer_;
  std::size_t offset_{0};
};

/// CRC-32C (Castagnoli), used for persisted-record integrity. A torn or
/// bit-rotted record must be detectable, so recovery can truncate instead of
/// interpreting damaged bytes as evidence.
[[nodiscard]] std::uint32_t crc32c(ByteSpan bytes) noexcept;
[[nodiscard]] std::uint32_t crc32c_extend(std::uint32_t seed, ByteSpan bytes) noexcept;

[[nodiscard]] std::string to_hex(ByteSpan bytes);
[[nodiscard]] Result<std::vector<std::uint8_t>> from_hex(std::string_view text);

}  // namespace loss_observatory
