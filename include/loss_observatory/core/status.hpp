#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace loss_observatory {

/// Maximum length retained for a status message. Status messages are
/// diagnostic, not payloads; they are truncated rather than allowed to grow
/// without bound from externally supplied text.
inline constexpr std::size_t kMaxStatusMessageBytes = 512;

enum class StatusCode : std::uint16_t {
  Ok = 0,
  InvalidArgument,
  OutOfRange,
  NotFound,
  AlreadyExists,
  Conflict,
  Stale,
  Fenced,
  Duplicate,
  CapacityExceeded,
  Overflow,
  Corrupt,
  Unsupported,
  Cancelled,
  ShuttingDown,
  Busy,
  Internal,
};

[[nodiscard]] std::string_view to_string(StatusCode code) noexcept;

/// Outcome of declaring an entity into a registry. Registries never overwrite
/// silently: an id that already exists with different content is a Conflict,
/// which the caller must resolve explicitly.
enum class UpsertOutcome : std::uint8_t {
  Inserted = 0,
  Unchanged,
  Replaced,
  Conflicted,
};

[[nodiscard]] std::string_view to_string(UpsertOutcome outcome) noexcept;

/// A recoverable outcome. Failure is expressed as a value, never as an
/// exception: the runtime must classify bad input, not unwind from it.
class Status {
 public:
  Status() noexcept = default;

  Status(StatusCode code, std::string message) : code_(code), message_(std::move(message)) {
    if (message_.size() > kMaxStatusMessageBytes) {
      message_.resize(kMaxStatusMessageBytes);
    }
  }

  explicit Status(StatusCode code) : code_(code) {}

  [[nodiscard]] StatusCode code() const noexcept { return code_; }
  [[nodiscard]] bool ok() const noexcept { return code_ == StatusCode::Ok; }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
  [[nodiscard]] const std::string& message() const noexcept { return message_; }

  /// "invalid-argument: message"
  [[nodiscard]] std::string to_string() const;

 private:
  StatusCode code_{StatusCode::Ok};
  std::string message_{};
};

[[nodiscard]] inline Status ok_status() noexcept { return Status{}; }

[[nodiscard]] Status make_status(StatusCode code, std::string_view message);

/// Result<T>: either a value or a Status. The error channel is a value so that
/// every failure path in the runtime is explicit and inspectable.
template <class T>
class Result {
 public:
  Result(T value) : storage_(std::move(value)) {}
  Result(Status status) : storage_(std::move(status)) {}

  [[nodiscard]] bool ok() const noexcept { return storage_.index() == 0; }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

  /// Precondition: ok(). Throws std::logic_error on contract violation only;
  /// no other function in this runtime throws.
  [[nodiscard]] T& value() &;
  [[nodiscard]] const T& value() const&;
  [[nodiscard]] T&& value() &&;

  [[nodiscard]] T value_or(T fallback) const {
    return ok() ? std::get<0>(storage_) : std::move(fallback);
  }

  /// Precondition: !ok().
  [[nodiscard]] const Status& status() const&;
  [[nodiscard]] StatusCode code() const noexcept;

 private:
  std::variant<T, Status> storage_;
};

/// Result<void> specialisation: success carries no value.
template <>
class Result<void> {
 public:
  Result() = default;
  Result(Status status) : status_(std::move(status)) {}

  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
  [[nodiscard]] const Status& status() const noexcept { return status_; }
  [[nodiscard]] StatusCode code() const noexcept { return status_.code(); }
  void value() const noexcept {}

 private:
  Status status_{};
};

template <class T>
T& Result<T>::value() & {
  if (!ok()) {
    throw std::logic_error("Result::value() called on a failed result: " + status().to_string());
  }
  return std::get<0>(storage_);
}

template <class T>
const T& Result<T>::value() const& {
  if (!ok()) {
    throw std::logic_error("Result::value() called on a failed result: " + status().to_string());
  }
  return std::get<0>(storage_);
}

template <class T>
T&& Result<T>::value() && {
  if (!ok()) {
    throw std::logic_error("Result::value() called on a failed result: " + status().to_string());
  }
  return std::move(std::get<0>(storage_));
}

template <class T>
const Status& Result<T>::status() const& {
  static const Status kOk{};
  return ok() ? kOk : std::get<1>(storage_);
}

template <class T>
StatusCode Result<T>::code() const noexcept {
  return ok() ? StatusCode::Ok : std::get<1>(storage_).code();
}

}  // namespace loss_observatory
