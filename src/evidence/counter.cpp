#include "loss_observatory/evidence/counter.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <tuple>

namespace loss_observatory {
namespace {

struct SeriesKey {
  SourceId source{};
  EpochId epoch{};
  CounterId counter{};
  CounterScope scope{CounterScope::Unknown};
  NodeId node{};
  PortId port{};
  QueueId queue{};
  HopId hop{};
  LinkId link{};

  friend bool operator<(const SeriesKey& lhs, const SeriesKey& rhs) {
    return std::tie(lhs.source, lhs.epoch, lhs.counter, lhs.scope, lhs.node, lhs.port, lhs.queue, lhs.hop,
                    lhs.link) < std::tie(rhs.source, rhs.epoch, rhs.counter, rhs.scope, rhs.node, rhs.port,
                                         rhs.queue, rhs.hop, rhs.link);
  }
};

[[nodiscard]] SeriesKey key_of(const EvidenceItem& item) {
  const auto* sample = std::get_if<CounterSample>(&item.payload);
  SeriesKey key{};
  key.source = item.header.source;
  key.epoch = item.header.epoch;
  if (sample != nullptr) {
    key.counter = sample->counter;
    key.scope = sample->scope;
    key.node = sample->node;
    key.port = sample->port;
    key.queue = sample->queue;
    key.hop = sample->hop;
    key.link = sample->link;
  }
  return key;
}

[[nodiscard]] MeasurementId derive_delta_id(const EvidenceItem& from, const EvidenceItem& to) {
  std::uint64_t mixed = combine_hash(from.header.id.value(), to.header.id.value());
  mixed = combine_hash(mixed, static_cast<std::uint64_t>(CounterDeltaState::Valid));
  return MeasurementId::from_value(mixed == 0 ? 1 : mixed);
}

}  // namespace

bool counter_binding_differs(const CounterSample& lhs, const CounterSample& rhs) noexcept {
  return !(lhs.counter == rhs.counter && lhs.scope == rhs.scope && lhs.node == rhs.node &&
           lhs.port == rhs.port && lhs.queue == rhs.queue && lhs.hop == rhs.hop && lhs.link == rhs.link);
}

std::string_view to_string(CounterDeltaState state) noexcept {
  switch (state) {
    case CounterDeltaState::Valid:
      return "valid";
    case CounterDeltaState::WrapDetected:
      return "wrap-detected";
    case CounterDeltaState::ResetDetected:
      return "reset-detected";
    case CounterDeltaState::DiscontinuityUnresolved:
      return "discontinuity-unresolved";
    case CounterDeltaState::OutOfOrder:
      return "out-of-order";
    case CounterDeltaState::EpochChanged:
      return "epoch-changed";
    case CounterDeltaState::GenerationChanged:
      return "generation-changed";
    case CounterDeltaState::CounterChanged:
      return "counter-changed";
    case CounterDeltaState::ImplausibleDelta:
      return "implausible-delta";
    case CounterDeltaState::MissingBaseline:
      return "missing-baseline";
  }
  return "discontinuity-unresolved";
}

bool CounterDelta::is_discontinuity() const noexcept {
  switch (state) {
    case CounterDeltaState::Valid:
      return false;
    case CounterDeltaState::MissingBaseline:
      return false;
    default:
      return true;
  }
}

std::string CounterDelta::to_string() const {
  std::string result("delta=");
  result.append(std::to_string(raw_previous));
  result.append("->");
  result.append(std::to_string(raw_current));
  result.append(" state=");
  result.append(loss_observatory::to_string(state));
  result.append(" delta=");
  result.append(std::to_string(delta));
  result.append(" elapsed=");
  result.append(elapsed.to_string());
  result.append(" width=");
  result.append(std::to_string(width_bits));
  if (!detail.empty()) {
    result.append(" detail=");
    result.append(detail);
  }
  return result;
}

std::vector<std::vector<EvidenceItem>> group_counter_series(std::span<const EvidenceItem> items,
                                                           std::size_t max_series, BoundNotes& bounds) {
  std::map<SeriesKey, std::vector<EvidenceItem>> groups;
  bool truncated = false;
  for (const EvidenceItem& item : items) {
    if (!std::holds_alternative<CounterSample>(item.payload)) {
      continue;
    }
    const SeriesKey key = key_of(item);
    auto it = groups.find(key);
    if (it == groups.end()) {
      if (groups.size() >= max_series) {
        truncated = true;
        continue;
      }
      it = groups.emplace(key, std::vector<EvidenceItem>{}).first;
    }
    it->second.push_back(item);
  }
  if (truncated) {
    bounds.add(BoundKind::EvidencePerSubject, static_cast<std::uint64_t>(max_series),
               static_cast<std::uint64_t>(groups.size()), "counter-series");
  }

  std::vector<std::vector<EvidenceItem>> result;
  result.reserve(groups.size());
  for (auto& entry : groups) {
    std::vector<EvidenceItem>& series = entry.second;
    std::sort(series.begin(), series.end(), [](const EvidenceItem& lhs, const EvidenceItem& rhs) {
      if (lhs.header.source_sequence != rhs.header.source_sequence) {
        return lhs.header.source_sequence < rhs.header.source_sequence;
      }
      return lhs.header.id < rhs.header.id;
    });
    result.push_back(std::move(series));
  }
  return result;
}

std::vector<CounterDelta> derive_counter_deltas(std::span<const EvidenceItem> series,
                                                const CounterPolicy& policy) {
  std::vector<CounterDelta> deltas;
  if (series.empty()) {
    return deltas;
  }
  if (series.size() == 1) {
    const EvidenceItem& only = series.front();
    CounterDelta delta{};
    delta.id = derive_delta_id(only, only);
    delta.from_id = only.header.id;
    delta.to_id = only.header.id;
    delta.from_sequence = only.header.source_sequence;
    delta.to_sequence = only.header.source_sequence;
    if (const auto* sample = std::get_if<CounterSample>(&only.payload)) {
      delta.raw_previous = sample->value;
      delta.raw_current = sample->value;
      delta.width_bits = sample->width_bits;
    }
    delta.state = CounterDeltaState::MissingBaseline;
    delta.detail = "a single reading cannot produce a delta";
    deltas.push_back(std::move(delta));
    return deltas;
  }

  for (std::size_t i = 0; i + 1 < series.size(); ++i) {
    const EvidenceItem& previous = series[i];
    const EvidenceItem& current = series[i + 1];
    const auto* previous_sample = std::get_if<CounterSample>(&previous.payload);
    const auto* current_sample = std::get_if<CounterSample>(&current.payload);
    if (previous_sample == nullptr || current_sample == nullptr) {
      continue;
    }

    CounterDelta delta{};
    delta.id = derive_delta_id(previous, current);
    delta.from_id = previous.header.id;
    delta.to_id = current.header.id;
    delta.from_sequence = previous.header.source_sequence;
    delta.to_sequence = current.header.source_sequence;
    delta.raw_previous = previous_sample->value;
    delta.raw_current = current_sample->value;
    delta.width_bits = current_sample->width_bits != 0 ? current_sample->width_bits : previous_sample->width_bits;
    delta.elapsed = current.header.observed_at.value - previous.header.observed_at.value;

    if (previous.header.source_sequence >= current.header.source_sequence) {
      delta.state = CounterDeltaState::OutOfOrder;
      delta.detail = "source sequence did not advance between the two readings";
      deltas.push_back(std::move(delta));
      continue;
    }
    if (delta.elapsed.is_negative()) {
      delta.state = CounterDeltaState::OutOfOrder;
      delta.detail = "second reading is dated before the first";
      deltas.push_back(std::move(delta));
      continue;
    }
    if (previous.header.epoch != current.header.epoch) {
      delta.state = CounterDeltaState::EpochChanged;
      delta.detail = "readings belong to different source epochs";
      deltas.push_back(std::move(delta));
      continue;
    }
    if (previous.header.generation != current.header.generation) {
      delta.state = CounterDeltaState::GenerationChanged;
      delta.detail = "readings belong to different generations";
      deltas.push_back(std::move(delta));
      continue;
    }
    if (counter_binding_differs(*previous_sample, *current_sample)) {
      delta.state = CounterDeltaState::CounterChanged;
      delta.detail = "counter identity, scope, or binding changed between readings";
      deltas.push_back(std::move(delta));
      continue;
    }

    const std::uint64_t previous_value = previous_sample->value;
    const std::uint64_t current_value = current_sample->value;

    if (current_value >= previous_value) {
      const std::uint64_t raw_delta = current_value - previous_value;
      if (raw_delta > policy.implausible_delta_threshold) {
        delta.state = CounterDeltaState::ImplausibleDelta;
        delta.detail = "monotonic delta exceeds the configured plausibility threshold";
        deltas.push_back(std::move(delta));
        continue;
      }
      delta.delta = raw_delta;
      delta.state = CounterDeltaState::Valid;
      deltas.push_back(std::move(delta));
      continue;
    }

    // The counter decreased. Decide between wrap, reset, and unresolved.
    const std::uint8_t width = delta.width_bits;
    if (!policy.allow_wrap || !counter_width_supported(width)) {
      if (!counter_width_supported(width)) {
        delta.state = CounterDeltaState::DiscontinuityUnresolved;
        delta.detail = "counter decreased and no supported width was declared, so a wrap cannot be proven";
      } else {
        delta.state = CounterDeltaState::ResetDetected;
        delta.detail = "counter decreased and wrap handling is disabled by policy";
      }
      deltas.push_back(std::move(delta));
      continue;
    }

    if (width == 64) {
      // Modular arithmetic over the full 64-bit range is well defined.
      const std::uint64_t wrapped = current_value - previous_value;
      delta.delta = wrapped;
      delta.state = wrapped > policy.implausible_delta_threshold ? CounterDeltaState::ImplausibleDelta
                                                                 : CounterDeltaState::WrapDetected;
      delta.detail = "counter decreased on a 64-bit counter: interpreted as a modular wrap";
      deltas.push_back(std::move(delta));
      continue;
    }

    const std::uint64_t modulus = counter_modulus(width);
    const std::uint64_t band = modulus / (policy.wrap_band_divisor == 0 ? 1 : policy.wrap_band_divisor);
    if (previous_value >= modulus - band) {
      const std::uint64_t wrapped = (modulus - previous_value) + current_value;
      if (wrapped > policy.implausible_delta_threshold) {
        delta.state = CounterDeltaState::ImplausibleDelta;
        delta.detail = "wrapped delta exceeds the configured plausibility threshold";
      } else {
        delta.delta = wrapped;
        delta.state = CounterDeltaState::WrapDetected;
        delta.detail = "counter decreased near the top of its declared range: interpreted as a wrap";
      }
      deltas.push_back(std::move(delta));
      continue;
    }

    delta.state = CounterDeltaState::ResetDetected;
    delta.detail = "counter decreased far from the top of its declared range: interpreted as a reset";
    deltas.push_back(std::move(delta));
  }
  return deltas;
}

}  // namespace loss_observatory
