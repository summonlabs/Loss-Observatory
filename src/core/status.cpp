#include "loss_observatory/core/status.hpp"

#include <string>

namespace loss_observatory {

std::string_view to_string(StatusCode code) noexcept {
  switch (code) {
    case StatusCode::Ok:
      return "ok";
    case StatusCode::InvalidArgument:
      return "invalid-argument";
    case StatusCode::OutOfRange:
      return "out-of-range";
    case StatusCode::NotFound:
      return "not-found";
    case StatusCode::AlreadyExists:
      return "already-exists";
    case StatusCode::Conflict:
      return "conflict";
    case StatusCode::Stale:
      return "stale";
    case StatusCode::Fenced:
      return "fenced";
    case StatusCode::Duplicate:
      return "duplicate";
    case StatusCode::CapacityExceeded:
      return "capacity-exceeded";
    case StatusCode::Overflow:
      return "overflow";
    case StatusCode::Corrupt:
      return "corrupt";
    case StatusCode::Unsupported:
      return "unsupported";
    case StatusCode::Cancelled:
      return "cancelled";
    case StatusCode::ShuttingDown:
      return "shutting-down";
    case StatusCode::Busy:
      return "busy";
    case StatusCode::Internal:
      return "internal";
  }
  return "unknown";
}

std::string_view to_string(UpsertOutcome outcome) noexcept {
  switch (outcome) {
    case UpsertOutcome::Inserted:
      return "inserted";
    case UpsertOutcome::Unchanged:
      return "unchanged";
    case UpsertOutcome::Replaced:
      return "replaced";
    case UpsertOutcome::Conflicted:
      return "conflicted";
  }
  return "conflicted";
}

std::string Status::to_string() const {
  std::string result(loss_observatory::to_string(code_));
  if (!message_.empty()) {
    result.append(": ");
    result.append(message_);
  }
  return result;
}

Status make_status(StatusCode code, std::string_view message) {
  return Status{code, std::string(message)};
}

}  // namespace loss_observatory
