#include "loss_observatory/model/source.hpp"

#include <shared_mutex>
#include <string>

namespace loss_observatory {
namespace {
constexpr std::size_t kDefaultSequenceWindow = 256;
}  // namespace

std::string_view to_string(SourceKind kind) noexcept {
  switch (kind) {
    case SourceKind::Unknown:
      return "unknown";
    case SourceKind::CounterTelemetry:
      return "counter-telemetry";
    case SourceKind::ActiveProbe:
      return "active-probe";
    case SourceKind::SequenceTracker:
      return "sequence-tracker";
    case SourceKind::EndpointComparison:
      return "endpoint-comparison";
    case SourceKind::Synthetic:
      return "synthetic";
  }
  return "unknown";
}

std::string_view to_string(SourceAuthority authority) noexcept {
  switch (authority) {
    case SourceAuthority::None:
      return "none";
    case SourceAuthority::Advisory:
      return "advisory";
    case SourceAuthority::Secondary:
      return "secondary";
    case SourceAuthority::Primary:
      return "primary";
  }
  return "none";
}

Result<SourceKind> parse_source_kind(std::string_view text) {
  if (text == "unknown") {
    return SourceKind::Unknown;
  }
  if (text == "counter-telemetry" || text == "counter") {
    return SourceKind::CounterTelemetry;
  }
  if (text == "active-probe" || text == "probe") {
    return SourceKind::ActiveProbe;
  }
  if (text == "sequence-tracker" || text == "sequence") {
    return SourceKind::SequenceTracker;
  }
  if (text == "endpoint-comparison" || text == "endpoint") {
    return SourceKind::EndpointComparison;
  }
  if (text == "synthetic") {
    return SourceKind::Synthetic;
  }
  return make_status(StatusCode::InvalidArgument, "unknown source kind");
}

Result<SourceAuthority> parse_source_authority(std::string_view text) {
  if (text == "none") {
    return SourceAuthority::None;
  }
  if (text == "advisory") {
    return SourceAuthority::Advisory;
  }
  if (text == "secondary") {
    return SourceAuthority::Secondary;
  }
  if (text == "primary") {
    return SourceAuthority::Primary;
  }
  return make_status(StatusCode::InvalidArgument, "unknown source authority");
}

std::string SourceDescriptor::to_string() const {
  std::string result = "source id=";
  result.append(id.to_string());
  result.append(" name=");
  result.append(name);
  result.append(" kind=");
  result.append(loss_observatory::to_string(kind));
  result.append(" authority=");
  result.append(loss_observatory::to_string(authority));
  return result;
}

std::string SourceIncarnation::to_string() const {
  std::string result = "incarnation source=";
  result.append(source.to_string());
  result.append(" epoch=");
  result.append(epoch.to_string());
  result.append(" name=");
  result.append(incarnation);
  result.append(" at=");
  result.append(activated_at.to_string());
  // "active" is the key the script reader understands; a retired incarnation
  // exports as inactive so an export/import round trip preserves the state.
  result.append(retired ? " active=false" : " active=true");
  return result;
}

Result<void> SourceRegistry::activate_incarnation_with_epoch(SourceId source, EpochId epoch,
                                                             std::string incarnation_name,
                                                             Timestamp activated_at) {
  if (source.is_nil() || epoch.is_nil()) {
    return make_status(StatusCode::InvalidArgument, "incarnation identity is nil");
  }
  if (incarnation_name.size() > kMaxIncarnationNameBytes) {
    return make_status(StatusCode::InvalidArgument, "incarnation name exceeds the configured bound");
  }
  if (incarnation_name.find_first_of(" \t\r\n") != std::string::npos) {
    return make_status(StatusCode::InvalidArgument, "incarnation name must not contain whitespace");
  }
  std::unique_lock lock(mutex_);
  if (sources_.find(source) == sources_.end()) {
    return make_status(StatusCode::NotFound, "source is not registered");
  }
  std::vector<SourceIncarnation>& list = incarnations_[source];
  for (SourceIncarnation& existing : list) {
    existing.retired = true;
    if (existing.epoch == epoch) {
      existing.incarnation = std::move(incarnation_name);
      existing.activated_at = activated_at;
      existing.retired = false;
      return {};
    }
  }
  list.push_back(SourceIncarnation{source, epoch, std::move(incarnation_name), activated_at, false});
  return {};
}

std::string_view to_string(FenceOutcome outcome) noexcept {
  switch (outcome) {
    case FenceOutcome::Accepted:
      return "accepted";
    case FenceOutcome::Reordered:
      return "reordered";
    case FenceOutcome::Replayed:
      return "replayed";
    case FenceOutcome::StaleEpoch:
      return "stale-epoch";
    case FenceOutcome::UnknownSource:
      return "unknown-source";
    case FenceOutcome::NoActiveIncarnation:
      return "no-active-incarnation";
  }
  return "unknown-source";
}

Result<UpsertOutcome> SourceRegistry::register_source(SourceDescriptor descriptor, bool allow_replace) {
  if (descriptor.id.is_nil()) {
    return make_status(StatusCode::InvalidArgument, "source identity is nil");
  }
  if (descriptor.name.size() > kMaxSourceNameBytes) {
    return make_status(StatusCode::InvalidArgument, "source name exceeds the configured bound");
  }
  if (descriptor.name.find_first_of(" \t\r\n") != std::string::npos) {
    return make_status(StatusCode::InvalidArgument,
                       "source name must not contain whitespace; it is a single script token");
  }
  std::unique_lock lock(mutex_);
  const auto it = sources_.find(descriptor.id);
  if (it == sources_.end()) {
    if (sources_.size() >= max_sources_) {
      return make_status(StatusCode::CapacityExceeded, "source limit reached");
    }
    sources_.emplace(descriptor.id, descriptor);
    return UpsertOutcome::Inserted;
  }
  if (it->second == descriptor) {
    return UpsertOutcome::Unchanged;
  }
  if (!allow_replace) {
    return make_status(StatusCode::Conflict, "source identity already declared with different content");
  }
  it->second = descriptor;
  return UpsertOutcome::Replaced;
}

Result<EpochId> SourceRegistry::activate_incarnation(SourceId source, std::string incarnation_name,
                                                     Timestamp activated_at) {
  if (source.is_nil()) {
    return make_status(StatusCode::InvalidArgument, "source identity is nil");
  }
  if (incarnation_name.size() > kMaxIncarnationNameBytes) {
    return make_status(StatusCode::InvalidArgument, "incarnation name exceeds the configured bound");
  }
  std::unique_lock lock(mutex_);
  const auto source_it = sources_.find(source);
  if (source_it == sources_.end()) {
    return make_status(StatusCode::NotFound, "source is not registered");
  }
  std::vector<SourceIncarnation>& list = incarnations_[source];
  for (SourceIncarnation& existing : list) {
    existing.retired = true;
  }
  // Epoch identity is derived from the source, the activation ordinal, the
  // declared name, and the activation instant, so it is unique per activation
  // and reproducible from the same inputs.
  const std::uint64_t ordinal = static_cast<std::uint64_t>(list.size());
  std::uint64_t mixed = combine_hash(source.value(), ordinal);
  mixed = combine_hash(mixed, fnv1a64(incarnation_name));
  mixed = combine_hash(mixed, static_cast<std::uint64_t>(activated_at.unix_nanos()));
  const EpochId epoch = EpochId::from_value(mixed == 0 ? 1 : mixed);
  list.push_back(SourceIncarnation{source, epoch, std::move(incarnation_name), activated_at, false});
  return epoch;
}

Result<void> SourceRegistry::restore_incarnation(SourceIncarnation incarnation) {
  if (incarnation.source.is_nil() || incarnation.epoch.is_nil()) {
    return make_status(StatusCode::InvalidArgument, "restored incarnation has a nil identity");
  }
  if (incarnation.incarnation.size() > kMaxIncarnationNameBytes) {
    return make_status(StatusCode::InvalidArgument, "restored incarnation name exceeds the configured bound");
  }
  std::unique_lock lock(mutex_);
  if (sources_.find(incarnation.source) == sources_.end()) {
    return make_status(StatusCode::NotFound, "restored incarnation references an unregistered source");
  }
  std::vector<SourceIncarnation>& list = incarnations_[incarnation.source];
  for (const SourceIncarnation& existing : list) {
    if (existing.epoch == incarnation.epoch) {
      return {};
    }
  }
  // Restored incarnations are retired on arrival: they belong to a previous
  // process, so no live evidence may be attributed to them.
  incarnation.retired = true;
  list.push_back(std::move(incarnation));
  return {};
}

Result<void> SourceRegistry::retire_incarnation(SourceId source, EpochId epoch) {
  std::unique_lock lock(mutex_);
  const auto it = incarnations_.find(source);
  if (it == incarnations_.end()) {
    return make_status(StatusCode::NotFound, "source has no incarnations");
  }
  for (SourceIncarnation& incarnation : it->second) {
    if (incarnation.epoch == epoch) {
      incarnation.retired = true;
      return {};
    }
  }
  return make_status(StatusCode::NotFound, "epoch is not an incarnation of this source");
}

Result<SourceDescriptor> SourceRegistry::find_source(SourceId id) const {
  std::shared_lock lock(mutex_);
  const auto it = sources_.find(id);
  if (it == sources_.end()) {
    return make_status(StatusCode::NotFound, "source is not registered");
  }
  return it->second;
}

Result<SourceIncarnation> SourceRegistry::current_incarnation(SourceId id) const {
  std::shared_lock lock(mutex_);
  const auto it = incarnations_.find(id);
  if (it == incarnations_.end() || it->second.empty()) {
    return make_status(StatusCode::NotFound, "source has no active incarnation");
  }
  return it->second.back();
}

Result<SourceIncarnation> SourceRegistry::find_incarnation(SourceId id, EpochId epoch) const {
  std::shared_lock lock(mutex_);
  const auto it = incarnations_.find(id);
  if (it == incarnations_.end()) {
    return make_status(StatusCode::NotFound, "source has no incarnations");
  }
  for (const SourceIncarnation& incarnation : it->second) {
    if (incarnation.epoch == epoch) {
      return incarnation;
    }
  }
  return make_status(StatusCode::NotFound, "epoch is not an incarnation of this source");
}

Result<SourceAuthority> SourceRegistry::authority_of(SourceId id) const {
  std::shared_lock lock(mutex_);
  const auto it = sources_.find(id);
  if (it == sources_.end()) {
    return make_status(StatusCode::NotFound, "source is not registered");
  }
  return it->second.authority;
}

FenceDecision SourceRegistry::apply_fence(SequenceState& state, SequenceId sequence) const {
  FenceDecision decision{};
  decision.high_water = state.high_water;

  if (state.seen.empty()) {
    state.high_water = sequence;
    state.seen.push_back(sequence);
    decision.outcome = FenceOutcome::Accepted;
    decision.admissible = true;
    return decision;
  }

  for (const SequenceId seen : state.seen) {
    if (seen == sequence) {
      decision.outcome = FenceOutcome::Replayed;
      decision.admissible = false;
      decision.detail = "sequence was already observed for this source epoch";
      return decision;
    }
  }

  if (sequence > state.high_water) {
    state.high_water = sequence;
    state.seen.push_back(sequence);
    while (state.seen.size() > sequence_window_) {
      state.seen.pop_front();
    }
    decision.high_water = state.high_water;
    decision.outcome = FenceOutcome::Accepted;
    decision.admissible = true;
    return decision;
  }

  // Older than the high-water mark and not previously seen: retained as history
  // but flagged, so it cannot silently rewrite current state.
  state.seen.push_back(sequence);
  while (state.seen.size() > sequence_window_) {
    state.seen.pop_front();
  }
  decision.outcome = FenceOutcome::Reordered;
  decision.admissible = true;
  decision.detail = "sequence is behind the high-water mark for this source epoch";
  return decision;
}

Result<FenceDecision> SourceRegistry::observe_sequence(SourceId source, EpochId epoch,
                                                       SequenceId sequence) {
  std::unique_lock lock(mutex_);
  const auto source_it = sources_.find(source);
  if (source_it == sources_.end()) {
    FenceDecision decision{};
    decision.outcome = FenceOutcome::UnknownSource;
    decision.admissible = false;
    decision.detail = "source is not registered";
    return decision;
  }
  const auto incarnation_it = incarnations_.find(source);
  if (incarnation_it == incarnations_.end() || incarnation_it->second.empty()) {
    FenceDecision decision{};
    decision.outcome = FenceOutcome::NoActiveIncarnation;
    decision.admissible = false;
    decision.detail = "source has no active incarnation";
    return decision;
  }
  const SourceIncarnation& current = incarnation_it->second.back();
  const bool known_epoch = [&]() {
    for (const SourceIncarnation& incarnation : incarnation_it->second) {
      if (incarnation.epoch == epoch) {
        return true;
      }
    }
    return false;
  }();
  if (!known_epoch || current.epoch != epoch || current.retired) {
    FenceDecision decision{};
    decision.outcome = FenceOutcome::StaleEpoch;
    decision.admissible = false;
    decision.detail = "evidence cites an epoch that is not the active incarnation";
    return decision;
  }

  SequenceState& state = sequences_[std::make_pair(source, epoch.value())];
  return apply_fence(state, sequence);
}

std::vector<SourceDescriptor> SourceRegistry::sources() const {
  std::shared_lock lock(mutex_);
  std::vector<SourceDescriptor> result;
  result.reserve(sources_.size());
  for (const auto& entry : sources_) {
    result.push_back(entry.second);
  }
  return result;
}

std::vector<SourceIncarnation> SourceRegistry::incarnations() const {
  std::shared_lock lock(mutex_);
  std::vector<SourceIncarnation> result;
  for (const auto& entry : incarnations_) {
    for (const SourceIncarnation& incarnation : entry.second) {
      result.push_back(incarnation);
    }
  }
  return result;
}

std::size_t SourceRegistry::source_count() const {
  std::shared_lock lock(mutex_);
  return sources_.size();
}

void SourceRegistry::set_sequence_window(std::size_t window) {
  std::unique_lock lock(mutex_);
  sequence_window_ = window == 0 ? kDefaultSequenceWindow : window;
}

std::size_t SourceRegistry::sequence_window() const {
  std::shared_lock lock(mutex_);
  return sequence_window_;
}

}  // namespace loss_observatory
