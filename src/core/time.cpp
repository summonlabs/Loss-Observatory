#include "loss_observatory/core/time.hpp"

#include <chrono>
#include <cstdio>
#include <string>

namespace loss_observatory {
namespace {

struct CivilDate {
  std::int64_t year{1970};
  unsigned month{1};
  unsigned day{1};
};

/// Howard Hinnant's civil-from-days, restricted to the range this runtime needs
/// and free of any locale or timezone database dependency: formatting must be
/// identical in every environment.
CivilDate civil_from_days(std::int64_t days) {
  const std::int64_t z = days + 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const auto doe = static_cast<std::uint64_t>(z - era * 146097);
  const auto yoe = static_cast<unsigned>((doe - doe / 1460 + doe / 36524 - doe / 146096) / 365);
  const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
  const std::uint64_t doy = doe - (365ULL * yoe + yoe / 4 - yoe / 100);
  const std::uint64_t mp = (5 * doy + 2) / 153;
  const auto d = static_cast<unsigned>(doy - (153 * mp + 2) / 5 + 1);
  const auto m = static_cast<unsigned>(mp < 10 ? mp + 3 : mp - 9);
  return CivilDate{y + (m <= 2 ? 1 : 0), m, d};
}

std::int64_t days_from_civil(std::int64_t year, unsigned month, unsigned day) {
  const std::int64_t y = year - (month <= 2 ? 1 : 0);
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const auto yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

constexpr std::int64_t kNanosPerSecond = 1000000000LL;

bool all_digits(std::string_view text) {
  if (text.empty()) {
    return false;
  }
  for (const char ch : text) {
    if (ch < '0' || ch > '9') {
      return false;
    }
  }
  return true;
}

bool parse_fixed_digits(std::string_view text, std::size_t offset, std::size_t count, std::uint64_t& out) {
  if (offset + count > text.size()) {
    return false;
  }
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const char ch = text[offset + i];
    if (ch < '0' || ch > '9') {
      return false;
    }
    value = value * 10 + static_cast<std::uint64_t>(ch - '0');
  }
  out = value;
  return true;
}

}  // namespace

std::string Timestamp::to_string() const {
  std::int64_t seconds = nanos_ / kNanosPerSecond;
  std::int64_t fraction = nanos_ % kNanosPerSecond;
  if (fraction < 0) {
    fraction += kNanosPerSecond;
    --seconds;
  }
  const std::int64_t days = seconds >= 0 ? seconds / 86400 : -(((-seconds) + 86399) / 86400);
  const std::int64_t seconds_of_day = seconds - days * 86400;
  const CivilDate date = civil_from_days(days);

  char buffer[64] = {};
  const auto hour = static_cast<unsigned>(seconds_of_day / 3600);
  const auto minute = static_cast<unsigned>((seconds_of_day % 3600) / 60);
  const auto second = static_cast<unsigned>(seconds_of_day % 60);
  const int written = std::snprintf(buffer, sizeof(buffer), "%04lld-%02u-%02uT%02u:%02u:%02u.%09lldZ",
                                    static_cast<long long>(date.year), date.month, date.day, hour, minute,
                                    second, static_cast<long long>(fraction));
  if (written <= 0) {
    return "1970-01-01T00:00:00.000000000Z";
  }
  return std::string(buffer, static_cast<std::size_t>(written));
}

Result<Timestamp> Timestamp::parse_iso8601(std::string_view text) {
  // Accepted: YYYY-MM-DDTHH:MM:SS[.fraction][Z]
  if (text.size() < 19) {
    return make_status(StatusCode::InvalidArgument, "timestamp too short for ISO-8601");
  }
  std::uint64_t year = 0;
  std::uint64_t month = 0;
  std::uint64_t day = 0;
  std::uint64_t hour = 0;
  std::uint64_t minute = 0;
  std::uint64_t second = 0;
  if (!parse_fixed_digits(text, 0, 4, year) || text[4] != '-' || !parse_fixed_digits(text, 5, 2, month) ||
      text[7] != '-' || !parse_fixed_digits(text, 8, 2, day) ||
      (text[10] != 'T' && text[10] != 't' && text[10] != ' ') || !parse_fixed_digits(text, 11, 2, hour) ||
      text[13] != ':' || !parse_fixed_digits(text, 14, 2, minute) || text[16] != ':' ||
      !parse_fixed_digits(text, 17, 2, second)) {
    return make_status(StatusCode::InvalidArgument, "timestamp does not match YYYY-MM-DDTHH:MM:SS");
  }
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) {
    return make_status(StatusCode::OutOfRange, "timestamp component out of range");
  }

  std::size_t cursor = 19;
  std::int64_t fraction = 0;
  if (cursor < text.size() && text[cursor] == '.') {
    ++cursor;
    std::size_t digits = 0;
    std::int64_t scale = 100000000;
    while (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9') {
      if (digits < 9) {
        fraction += static_cast<std::int64_t>(text[cursor] - '0') * scale;
        scale /= 10;
      }
      ++digits;
      ++cursor;
    }
    if (digits == 0) {
      return make_status(StatusCode::InvalidArgument, "fractional seconds marker without digits");
    }
  }
  if (cursor < text.size() && (text[cursor] == 'Z' || text[cursor] == 'z')) {
    ++cursor;
  }
  if (cursor != text.size()) {
    return make_status(StatusCode::InvalidArgument, "trailing characters in timestamp");
  }

  const std::int64_t days = days_from_civil(static_cast<std::int64_t>(year), static_cast<unsigned>(month),
                                            static_cast<unsigned>(day));
  const std::int64_t seconds =
      days * 86400 + static_cast<std::int64_t>(hour) * 3600 + static_cast<std::int64_t>(minute) * 60 +
      static_cast<std::int64_t>(second);
  return Timestamp::from_unix_nanos(seconds * kNanosPerSecond + fraction);
}

std::string Duration::to_string() const {
  const bool negative = nanos_ < 0;
  const std::uint64_t magnitude = static_cast<std::uint64_t>(negative ? -nanos_ : nanos_);
  const std::uint64_t seconds = magnitude / 1000000000ULL;
  const std::uint64_t nanos = magnitude % 1000000000ULL;
  char buffer[64] = {};
  const int written = std::snprintf(buffer, sizeof(buffer), "%s%llu.%09llus", negative ? "-" : "",
                                    static_cast<unsigned long long>(seconds),
                                    static_cast<unsigned long long>(nanos));
  if (written <= 0) {
    return "0.000000000s";
  }
  return std::string(buffer, static_cast<std::size_t>(written));
}

Timestamp SystemClock::now() const noexcept {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  return Timestamp::from_unix_nanos(static_cast<std::int64_t>(nanos));
}

}  // namespace loss_observatory
