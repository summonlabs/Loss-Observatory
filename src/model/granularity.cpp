#include "loss_observatory/model/granularity.hpp"

namespace loss_observatory {

std::string_view to_string(Granularity granularity) noexcept {
  switch (granularity) {
    case Granularity::Unknown:
      return "unknown";
    case Granularity::Flow:
      return "flow";
    case Granularity::Path:
      return "path";
    case Granularity::Hop:
      return "hop";
    case Granularity::Link:
      return "link";
    case Granularity::Queue:
      return "queue";
  }
  return "unknown";
}

Result<Granularity> parse_granularity(std::string_view text) {
  if (text == "unknown") {
    return Granularity::Unknown;
  }
  if (text == "flow") {
    return Granularity::Flow;
  }
  if (text == "path") {
    return Granularity::Path;
  }
  if (text == "hop") {
    return Granularity::Hop;
  }
  if (text == "link") {
    return Granularity::Link;
  }
  if (text == "queue") {
    return Granularity::Queue;
  }
  return make_status(StatusCode::InvalidArgument, "unknown granularity");
}

}  // namespace loss_observatory
