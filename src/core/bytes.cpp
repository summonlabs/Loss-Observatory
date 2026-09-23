#include "loss_observatory/core/bytes.hpp"

#include <array>
#include <string>

namespace loss_observatory {
namespace {

constexpr std::array<std::uint32_t, 256> make_crc_table() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t crc = i;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0x82F63B78U & (0U - (crc & 1U)));
    }
    table[i] = crc;
  }
  return table;
}

constexpr auto kCrcTable = make_crc_table();

constexpr char kHexDigits[] = "0123456789abcdef";

int hex_value(char ch) {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'a' && ch <= 'f') {
    return ch - 'a' + 10;
  }
  if (ch >= 'A' && ch <= 'F') {
    return ch - 'A' + 10;
  }
  return -1;
}

}  // namespace

bool ByteWriter::reserve(std::size_t count) noexcept {
  if (overflowed_) {
    return false;
  }
  if (count > buffer_.size() - offset_) {
    overflowed_ = true;
    return false;
  }
  return true;
}

ByteWriter& ByteWriter::u8(std::uint8_t value) noexcept {
  if (reserve(1)) {
    buffer_[offset_] = value;
    ++offset_;
  }
  return *this;
}

ByteWriter& ByteWriter::u16(std::uint16_t value) noexcept {
  u8(static_cast<std::uint8_t>(value & 0xFFU));
  u8(static_cast<std::uint8_t>((value >> 8) & 0xFFU));
  return *this;
}

ByteWriter& ByteWriter::u32(std::uint32_t value) noexcept {
  u16(static_cast<std::uint16_t>(value & 0xFFFFU));
  u16(static_cast<std::uint16_t>((value >> 16) & 0xFFFFU));
  return *this;
}

ByteWriter& ByteWriter::u64(std::uint64_t value) noexcept {
  u32(static_cast<std::uint32_t>(value & 0xFFFFFFFFULL));
  u32(static_cast<std::uint32_t>((value >> 32) & 0xFFFFFFFFULL));
  return *this;
}

ByteWriter& ByteWriter::i64(std::int64_t value) noexcept {
  u64(static_cast<std::uint64_t>(value));
  return *this;
}

ByteWriter& ByteWriter::raw(ByteSpan bytes) noexcept {
  if (reserve(bytes.size())) {
    for (std::size_t i = 0; i < bytes.size(); ++i) {
      buffer_[offset_ + i] = bytes[i];
    }
    offset_ += bytes.size();
  }
  return *this;
}

ByteWriter& ByteWriter::length_prefixed(ByteSpan bytes) noexcept {
  if (bytes.size() > 0xFFFFFFFFULL) {
    overflowed_ = true;
    return *this;
  }
  u32(static_cast<std::uint32_t>(bytes.size()));
  raw(bytes);
  return *this;
}

ByteWriter& ByteWriter::text(std::string_view value) noexcept {
  const auto* data = reinterpret_cast<const std::uint8_t*>(value.data());
  length_prefixed(ByteSpan{data, value.size()});
  return *this;
}

bool ByteReader::take(std::size_t count, ByteSpan& out) noexcept {
  if (count > remaining()) {
    return false;
  }
  out = buffer_.subspan(offset_, count);
  offset_ += count;
  return true;
}

bool ByteReader::u8(std::uint8_t& out) noexcept {
  ByteSpan slice{};
  if (!take(1, slice)) {
    return false;
  }
  out = slice[0];
  return true;
}

bool ByteReader::u16(std::uint16_t& out) noexcept {
  std::uint8_t low = 0;
  std::uint8_t high = 0;
  if (!u8(low) || !u8(high)) {
    return false;
  }
  out = static_cast<std::uint16_t>(static_cast<std::uint16_t>(low) |
                                   static_cast<std::uint16_t>(static_cast<std::uint16_t>(high) << 8));
  return true;
}

bool ByteReader::u32(std::uint32_t& out) noexcept {
  std::uint16_t low = 0;
  std::uint16_t high = 0;
  if (!u16(low) || !u16(high)) {
    return false;
  }
  out = static_cast<std::uint32_t>(low) | (static_cast<std::uint32_t>(high) << 16);
  return true;
}

bool ByteReader::u64(std::uint64_t& out) noexcept {
  std::uint32_t low = 0;
  std::uint32_t high = 0;
  if (!u32(low) || !u32(high)) {
    return false;
  }
  out = static_cast<std::uint64_t>(low) | (static_cast<std::uint64_t>(high) << 32);
  return true;
}

bool ByteReader::i64(std::int64_t& out) noexcept {
  std::uint64_t raw_value = 0;
  if (!u64(raw_value)) {
    return false;
  }
  out = static_cast<std::int64_t>(raw_value);
  return true;
}

bool ByteReader::raw(std::size_t count, ByteSpan& out) noexcept { return take(count, out); }

bool ByteReader::skip(std::size_t count) noexcept {
  ByteSpan slice{};
  return take(count, slice);
}

bool ByteReader::length_prefixed(ByteSpan& out) noexcept {
  std::uint32_t length = 0;
  if (!u32(length)) {
    return false;
  }
  return take(length, out);
}

bool ByteReader::text(std::string& out, std::size_t max_bytes) noexcept {
  ByteSpan slice{};
  if (!length_prefixed(slice)) {
    return false;
  }
  if (slice.size() > max_bytes) {
    return false;
  }
  out.assign(reinterpret_cast<const char*>(slice.data()), slice.size());
  return true;
}

std::uint32_t crc32c_extend(std::uint32_t seed, ByteSpan bytes) noexcept {
  std::uint32_t crc = ~seed;
  for (const std::uint8_t byte : bytes) {
    crc = kCrcTable[(crc ^ byte) & 0xFFU] ^ (crc >> 8);
  }
  return ~crc;
}

std::uint32_t crc32c(ByteSpan bytes) noexcept { return crc32c_extend(0, bytes); }

std::string to_hex(ByteSpan bytes) {
  std::string result;
  result.reserve(bytes.size() * 2);
  for (const std::uint8_t byte : bytes) {
    result.push_back(kHexDigits[(byte >> 4) & 0x0FU]);
    result.push_back(kHexDigits[byte & 0x0FU]);
  }
  return result;
}

Result<std::vector<std::uint8_t>> from_hex(std::string_view text) {
  if (text.size() % 2 != 0) {
    return make_status(StatusCode::InvalidArgument, "hex text must have an even number of digits");
  }
  std::vector<std::uint8_t> result;
  result.reserve(text.size() / 2);
  for (std::size_t i = 0; i < text.size(); i += 2) {
    const int high = hex_value(text[i]);
    const int low = hex_value(text[i + 1]);
    if (high < 0 || low < 0) {
      return make_status(StatusCode::InvalidArgument, "hex text contains a non-hex digit");
    }
    result.push_back(static_cast<std::uint8_t>((high << 4) | low));
  }
  return result;
}

}  // namespace loss_observatory
