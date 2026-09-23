#include "loss_observatory/model/ids.hpp"

#include <string>

namespace loss_observatory {
namespace {
constexpr char kHexDigits[] = "0123456789abcdef";
}  // namespace

std::string hex_u64(std::uint64_t value) {
  std::string result(16, '0');
  for (int i = 15; i >= 0; --i) {
    result[static_cast<std::size_t>(i)] = kHexDigits[value & 0x0FU];
    value >>= 4;
  }
  return result;
}

Result<std::uint64_t> parse_hex_u64(std::string_view text) {
  if (text.size() != 16) {
    return make_status(StatusCode::InvalidArgument, "identity must be exactly 16 hexadecimal digits");
  }
  std::uint64_t value = 0;
  for (const char ch : text) {
    std::uint64_t digit = 0;
    if (ch >= '0' && ch <= '9') {
      digit = static_cast<std::uint64_t>(ch - '0');
    } else if (ch >= 'a' && ch <= 'f') {
      digit = static_cast<std::uint64_t>(ch - 'a' + 10);
    } else if (ch >= 'A' && ch <= 'F') {
      digit = static_cast<std::uint64_t>(ch - 'A' + 10);
    } else {
      return make_status(StatusCode::InvalidArgument, "identity contains a non-hexadecimal character");
    }
    value = (value << 4) | digit;
  }
  return value;
}

std::string_view to_string(SubjectKind kind) noexcept {
  switch (kind) {
    case SubjectKind::Unknown:
      return "unknown";
    case SubjectKind::Flow:
      return "flow";
    case SubjectKind::Path:
      return "path";
    case SubjectKind::Hop:
      return "hop";
    case SubjectKind::Link:
      return "link";
    case SubjectKind::Queue:
      return "queue";
    case SubjectKind::Source:
      return "source";
  }
  return "unknown";
}

Result<SubjectKind> parse_subject_kind(std::string_view text) {
  if (text == "flow") {
    return SubjectKind::Flow;
  }
  if (text == "path") {
    return SubjectKind::Path;
  }
  if (text == "hop") {
    return SubjectKind::Hop;
  }
  if (text == "link") {
    return SubjectKind::Link;
  }
  if (text == "queue") {
    return SubjectKind::Queue;
  }
  if (text == "source") {
    return SubjectKind::Source;
  }
  if (text == "unknown") {
    return SubjectKind::Unknown;
  }
  return make_status(StatusCode::InvalidArgument, "unknown subject kind");
}

Result<FlowId> SubjectRef::as_flow() const {
  if (kind_ != SubjectKind::Flow) {
    return make_status(StatusCode::InvalidArgument, "subject is not a flow");
  }
  return FlowId::from_value(id_);
}

Result<PathId> SubjectRef::as_path() const {
  if (kind_ != SubjectKind::Path) {
    return make_status(StatusCode::InvalidArgument, "subject is not a path");
  }
  return PathId::from_value(id_);
}

Result<HopId> SubjectRef::as_hop() const {
  if (kind_ != SubjectKind::Hop) {
    return make_status(StatusCode::InvalidArgument, "subject is not a hop");
  }
  return HopId::from_value(id_);
}

Result<LinkId> SubjectRef::as_link() const {
  if (kind_ != SubjectKind::Link) {
    return make_status(StatusCode::InvalidArgument, "subject is not a link");
  }
  return LinkId::from_value(id_);
}

Result<QueueId> SubjectRef::as_queue() const {
  if (kind_ != SubjectKind::Queue) {
    return make_status(StatusCode::InvalidArgument, "subject is not a queue");
  }
  return QueueId::from_value(id_);
}

Result<SourceId> SubjectRef::as_source() const {
  if (kind_ != SubjectKind::Source) {
    return make_status(StatusCode::InvalidArgument, "subject is not a source");
  }
  return SourceId::from_value(id_);
}

std::string SubjectRef::to_string() const {
  if (kind_ == SubjectKind::Unknown) {
    return "unknown";
  }
  std::string result(loss_observatory::to_string(kind_));
  result.push_back(':');
  result.append(hex_u64(id_));
  return result;
}

Result<std::uint64_t> canonical_identity(std::string_view text) {
  if (text.empty()) {
    return make_status(StatusCode::InvalidArgument, "identity text is empty");
  }
  // Exactly 16 hexadecimal digits is a literal identity. Anything else is
  // treated as a symbolic name and hashed deterministically, which makes
  // hand-written scenarios and exported reports refer to the same entities.
  if (text.size() == 16) {
    auto parsed = parse_hex_u64(text);
    if (parsed.ok()) {
      return parsed.value();
    }
  }
  return fnv1a64(text);
}

}  // namespace loss_observatory
