#include "loss_observatory/evidence/derive.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>

#include "loss_observatory/core/checked.hpp"

namespace loss_observatory {
namespace {

[[nodiscard]] MeasurementId observation_id(const EvidenceItem& item, std::string_view basis) {
  const std::uint64_t mixed = combine_hash(item.header.id.value(), fnv1a64(basis));
  return MeasurementId::from_value(mixed == 0 ? 1 : mixed);
}

[[nodiscard]] LossObservation base_observation(const EvidenceItem& item, std::string_view basis,
                                               LossSemantics semantics) {
  LossObservation observation{};
  observation.id = observation_id(item, basis);
  observation.evidence_id = item.header.id;
  observation.source = item.header.source;
  observation.epoch = item.header.epoch;
  observation.generation = item.header.generation;
  observation.topology_revision = item.header.topology_revision;
  observation.source_sequence = item.header.source_sequence;
  observation.observed_at = item.header.observed_at;
  observation.received_at = item.header.received_at;
  observation.subject = item.header.subject;
  observation.granularity = item.header.granularity;
  observation.method = item.header.method;
  observation.semantics = semantics;
  observation.synthetic = semantics_of(item.header.method).synthetic;
  observation.recovered_from_persistence = item.header.recovered_from_persistence;
  return observation;
}

struct DeclaredTopology {
  std::set<FlowId> flows{};
  std::set<PathId> paths{};
  std::set<HopId> hops{};
  std::set<LinkId> links{};
  std::set<QueueId> queues{};
  bool complete{false};
};

[[nodiscard]] DeclaredTopology collect_declared(const TopologyRegistry& topology) {
  DeclaredTopology declared{};
  for (const FlowBinding& flow : topology.flows()) {
    declared.flows.insert(flow.id);
  }
  for (const Path& path : topology.paths()) {
    declared.paths.insert(path.id);
    for (const Hop& hop : path.hops) {
      declared.hops.insert(hop.id);
    }
  }
  for (const Link& link : topology.links()) {
    declared.links.insert(link.id);
  }
  for (const QueueEntity& queue : topology.queues()) {
    declared.queues.insert(queue.id);
  }
  declared.complete = true;
  return declared;
}

[[nodiscard]] bool subject_is_declared(const DeclaredTopology& declared, const SubjectRef& subject) {
  switch (subject.kind()) {
    case SubjectKind::Flow:
      return declared.flows.count(FlowId::from_value(subject.raw_id())) != 0;
    case SubjectKind::Path:
      return declared.paths.count(PathId::from_value(subject.raw_id())) != 0;
    case SubjectKind::Hop:
      return declared.hops.count(HopId::from_value(subject.raw_id())) != 0;
    case SubjectKind::Link:
      return declared.links.count(LinkId::from_value(subject.raw_id())) != 0;
    case SubjectKind::Queue:
      return declared.queues.count(QueueId::from_value(subject.raw_id())) != 0;
    case SubjectKind::Source:
    case SubjectKind::Unknown:
      return true;
  }
  return false;
}

[[nodiscard]] std::string subject_detail(const SubjectRef& subject) {
  return "subject " + subject.to_string() + " is not declared in the topology";
}

void apply_counter_delta(LossObservation& observation, const CounterDelta& delta, const EvidenceItem& to_item,
                         const CounterPolicy& policy) {
  observation.lost = 0;
  observation.offered = 0;
  observation.offered_known = false;
  observation.ratio_defined = false;
  observation.ratio_bp = 0;
  observation.detail = delta.to_string();

  switch (delta.state) {
    case CounterDeltaState::Valid:
      observation.validity = ObservationValidity::Valid;
      observation.discontinuity = DiscontinuityKind::None;
      break;
    case CounterDeltaState::WrapDetected:
      observation.discontinuity = DiscontinuityKind::CounterWrap;
      observation.validity = policy.accept_wrapped_delta ? ObservationValidity::Valid
                                                         : ObservationValidity::Discontinuity;
      break;
    case CounterDeltaState::ResetDetected:
      observation.validity = ObservationValidity::Discontinuity;
      observation.discontinuity = DiscontinuityKind::CounterReset;
      break;
    case CounterDeltaState::DiscontinuityUnresolved:
      observation.validity = ObservationValidity::Discontinuity;
      observation.discontinuity = DiscontinuityKind::DiscontinuityUnresolved;
      break;
    case CounterDeltaState::OutOfOrder:
      observation.validity = ObservationValidity::Discontinuity;
      observation.discontinuity = DiscontinuityKind::OutOfOrder;
      break;
    case CounterDeltaState::EpochChanged:
      observation.validity = ObservationValidity::Discontinuity;
      observation.discontinuity = DiscontinuityKind::EpochChange;
      break;
    case CounterDeltaState::GenerationChanged:
      observation.validity = ObservationValidity::Discontinuity;
      observation.discontinuity = DiscontinuityKind::GenerationChange;
      break;
    case CounterDeltaState::CounterChanged:
      observation.validity = ObservationValidity::Discontinuity;
      observation.discontinuity = DiscontinuityKind::DiscontinuityUnresolved;
      break;
    case CounterDeltaState::ImplausibleDelta:
      observation.validity = ObservationValidity::Implausible;
      observation.discontinuity = DiscontinuityKind::ImplausibleDelta;
      break;
    case CounterDeltaState::MissingBaseline:
      observation.validity = ObservationValidity::Insufficient;
      observation.discontinuity = DiscontinuityKind::MissingBaseline;
      break;
  }

  if (observation.validity != ObservationValidity::Valid) {
    return;
  }

  const auto* sample = std::get_if<CounterSample>(&to_item.payload);
  const CounterScope scope = sample != nullptr ? sample->scope : CounterScope::Unknown;
  if (scope_reports_loss_directly(scope)) {
    observation.semantics = LossSemantics::DirectLoss;
    observation.lost = delta.delta;
    observation.offered = 0;
    observation.offered_known = false;
    observation.ratio_defined = false;
    return;
  }
  // A throughput counter on its own carries no expectation to compare against.
  // It is recorded as a valid observation that simply is not loss evidence.
  observation.semantics = LossSemantics::NonLoss;
  observation.lost = 0;
  observation.offered = delta.delta;
  observation.offered_known = true;
  observation.ratio_defined = false;
}

void apply_probe(LossObservation& observation, const ProbeReport& report, const DerivationContext& context,
                 const EvidenceItem& item) {
  observation.detail = describe(report);
  const MethodSemantics& semantics = semantics_of(item.header.method);
  if (!semantics.implemented) {
    observation.validity = ObservationValidity::UnsupportedMethod;
    observation.semantics = LossSemantics::RatioLoss;
    return;
  }
  if (semantics.requires_synchronized_clocks && !context.clock_synchronized) {
    observation.validity = ObservationValidity::UnsupportedPrecondition;
    observation.semantics = LossSemantics::RatioLoss;
    return;
  }
  observation.semantics = LossSemantics::RatioLoss;
  if (report.received > report.sent) {
    observation.validity = ObservationValidity::Implausible;
    observation.discontinuity = DiscontinuityKind::None;
    return;
  }
  const std::uint64_t offered = report.sent;
  const std::uint64_t lost = offered - report.received;
  observation.offered = offered;
  observation.offered_known = true;
  observation.lost = lost;
  if (offered == 0) {
    observation.validity = ObservationValidity::UndefinedRatio;
    observation.ratio_defined = false;
    return;
  }
  std::uint32_t ratio = 0;
  if (!checked::ratio_basis_points(lost, offered, ratio)) {
    observation.validity = ObservationValidity::UndefinedRatio;
    observation.ratio_defined = false;
    return;
  }
  observation.ratio_bp = ratio;
  observation.ratio_defined = true;
}

void apply_sequence(LossObservation& observation, const SequenceReport& report) {
  observation.detail = describe(report);
  observation.semantics = LossSemantics::RatioLoss;
  if (report.sequence_restart) {
    observation.validity = ObservationValidity::Discontinuity;
    observation.discontinuity = DiscontinuityKind::SequenceRestart;
    return;
  }
  if (report.highest_sequence < report.lowest_sequence) {
    observation.validity = ObservationValidity::Implausible;
    return;
  }
  std::uint64_t span = 0;
  if (!checked::sub(report.highest_sequence, report.lowest_sequence, span) ||
      !checked::add(span, 1, span)) {
    observation.validity = ObservationValidity::Implausible;
    return;
  }
  if (report.received_count > span) {
    observation.validity = ObservationValidity::Implausible;
    return;
  }
  const std::uint64_t lost = span - report.received_count;
  observation.offered = span;
  observation.offered_known = true;
  observation.lost = lost;
  if (span == 0) {
    observation.validity = ObservationValidity::UndefinedRatio;
    return;
  }
  std::uint32_t ratio = 0;
  if (!checked::ratio_basis_points(lost, span, ratio)) {
    observation.validity = ObservationValidity::UndefinedRatio;
    return;
  }
  observation.ratio_bp = ratio;
  observation.ratio_defined = true;
}

struct EndpointGroupKey {
  FlowId flow{};
  GenerationId generation{};
  EpochId epoch{};

  friend bool operator<(const EndpointGroupKey& lhs, const EndpointGroupKey& rhs) {
    return std::tie(lhs.flow, lhs.generation, lhs.epoch) < std::tie(rhs.flow, rhs.generation, rhs.epoch);
  }
};

}  // namespace

std::string_view to_string(LossSemantics semantics) noexcept {
  switch (semantics) {
    case LossSemantics::DirectLoss:
      return "direct-loss";
    case LossSemantics::RatioLoss:
      return "ratio-loss";
    case LossSemantics::NonLoss:
      return "non-loss";
  }
  return "non-loss";
}

std::string_view to_string(ObservationValidity validity) noexcept {
  switch (validity) {
    case ObservationValidity::Valid:
      return "valid";
    case ObservationValidity::Discontinuity:
      return "discontinuity";
    case ObservationValidity::Implausible:
      return "implausible";
    case ObservationValidity::Insufficient:
      return "insufficient";
    case ObservationValidity::UndefinedRatio:
      return "undefined-ratio";
    case ObservationValidity::UnsupportedMethod:
      return "unsupported-method";
    case ObservationValidity::UnsupportedPrecondition:
      return "unsupported-precondition";
  }
  return "insufficient";
}

std::string_view to_string(DiscontinuityKind kind) noexcept {
  switch (kind) {
    case DiscontinuityKind::None:
      return "none";
    case DiscontinuityKind::CounterWrap:
      return "counter-wrap";
    case DiscontinuityKind::CounterReset:
      return "counter-reset";
    case DiscontinuityKind::DiscontinuityUnresolved:
      return "discontinuity-unresolved";
    case DiscontinuityKind::ImplausibleDelta:
      return "implausible-delta";
    case DiscontinuityKind::SequenceRestart:
      return "sequence-restart";
    case DiscontinuityKind::EpochChange:
      return "epoch-change";
    case DiscontinuityKind::GenerationChange:
      return "generation-change";
    case DiscontinuityKind::TopologyRevisionChange:
      return "topology-revision-change";
    case DiscontinuityKind::OutOfOrder:
      return "out-of-order";
    case DiscontinuityKind::MissingBaseline:
      return "missing-baseline";
  }
  return "none";
}

bool is_discontinuity(DiscontinuityKind kind) noexcept { return kind != DiscontinuityKind::None; }

std::string LossObservation::to_string() const {
  std::string result = subject.to_string();
  result.append(" method=");
  result.append(loss_observatory::to_string(method));
  result.append(" semantics=");
  result.append(loss_observatory::to_string(semantics));
  result.append(" validity=");
  result.append(loss_observatory::to_string(validity));
  result.append(" discontinuity=");
  result.append(loss_observatory::to_string(discontinuity));
  result.append(" granularity=");
  result.append(loss_observatory::to_string(granularity));
  result.append(" source=");
  result.append(source.to_string());
  result.append(" lost=");
  result.append(std::to_string(lost));
  result.append(" offered=");
  result.append(std::to_string(offered));
  result.append(" ratio_bp=");
  result.append(ratio_defined ? std::to_string(ratio_bp) : std::string("undefined"));
  return result;
}

std::string_view to_string(RejectionReason reason) noexcept {
  switch (reason) {
    case RejectionReason::None:
      return "none";
    case RejectionReason::Malformed:
      return "malformed";
    case RejectionReason::PayloadTooLarge:
      return "payload-too-large";
    case RejectionReason::NoteTooLong:
      return "note-too-long";
    case RejectionReason::UnknownSource:
      return "unknown-source";
    case RejectionReason::UnknownEpoch:
      return "unknown-epoch";
    case RejectionReason::StaleEpoch:
      return "stale-epoch";
    case RejectionReason::ReplayedSequence:
      return "replayed-sequence";
    case RejectionReason::ReorderedSequence:
      return "reordered-sequence";
    case RejectionReason::DuplicateMeasurement:
      return "duplicate-measurement";
    case RejectionReason::UnsupportedMethod:
      return "unsupported-method";
    case RejectionReason::GranularityExceedsMethod:
      return "granularity-exceeds-method";
    case RejectionReason::GranularityUnknown:
      return "granularity-unknown";
    case RejectionReason::TopologyUnknown:
      return "topology-unknown";
    case RejectionReason::GenerationUnknown:
      return "generation-unknown";
    case RejectionReason::StoreCapacity:
      return "store-capacity";
    case RejectionReason::Cancelled:
      return "cancelled";
  }
  return "none";
}

DerivationResult derive_observations(std::vector<EvidenceItem> items, const DerivationContext& context,
                                     const DerivePolicy& policy) {
  DerivationResult result{};
  items = canonical_order(std::move(items));

  DeclaredTopology declared{};
  const bool check_topology = policy.require_declared_topology && context.topology != nullptr;
  if (check_topology) {
    declared = collect_declared(*context.topology);
  }

  std::vector<EvidenceItem> counter_items;
  counter_items.reserve(items.size());
  for (const EvidenceItem& item : items) {
    if (check_topology && !subject_is_declared(declared, item.header.subject)) {
      RejectionNote note{};
      note.id = item.header.id;
      note.source = item.header.source;
      note.reason = RejectionReason::TopologyUnknown;
      note.detail = subject_detail(item.header.subject);
      result.rejections.push_back(std::move(note));
      continue;
    }
    switch (kind_of(item.payload)) {
      case EvidenceKind::CounterSample:
        counter_items.push_back(item);
        break;
      case EvidenceKind::ProbeReport:
        ++result.probe_reports_examined;
        break;
      case EvidenceKind::SequenceReport:
        ++result.sequence_reports_examined;
        break;
      case EvidenceKind::EndpointReport:
        ++result.endpoint_reports_examined;
        break;
      case EvidenceKind::Unknown:
        break;
    }

    if (result.observations.size() >= policy.max_observations) {
      result.bounds.add(BoundKind::ResultSet, static_cast<std::uint64_t>(policy.max_observations),
                        static_cast<std::uint64_t>(result.observations.size() + 1), "derivation");
      continue;
    }

    if (const auto* probe = std::get_if<ProbeReport>(&item.payload)) {
      LossObservation observation = base_observation(item, "probe", LossSemantics::RatioLoss);
      apply_probe(observation, *probe, context, item);
      result.observations.push_back(std::move(observation));
    } else if (const auto* sequence = std::get_if<SequenceReport>(&item.payload)) {
      LossObservation observation = base_observation(item, "sequence", LossSemantics::RatioLoss);
      apply_sequence(observation, *sequence);
      result.observations.push_back(std::move(observation));
    }
  }

  // ---- counters ----------------------------------------------------------
  const std::vector<std::vector<EvidenceItem>> series =
      group_counter_series(counter_items, policy.counter.max_series, result.bounds);
  result.counter_series_examined = series.size();
  for (const std::vector<EvidenceItem>& group : series) {
    std::vector<EvidenceItem> trimmed(group.begin(),
                                      group.begin() + static_cast<std::ptrdiff_t>(
                                                           std::min(group.size(), policy.counter.max_points_per_series)));
    if (trimmed.size() < group.size()) {
      result.bounds.add(BoundKind::EvidencePerSubject,
                        static_cast<std::uint64_t>(policy.counter.max_points_per_series),
                        static_cast<std::uint64_t>(group.size()), "counter-series-points");
    }
    const std::vector<CounterDelta> deltas = derive_counter_deltas(trimmed, policy.counter);
    for (const CounterDelta& delta : deltas) {
      if (result.observations.size() >= policy.max_observations) {
        result.bounds.add(BoundKind::ResultSet, static_cast<std::uint64_t>(policy.max_observations),
                          static_cast<std::uint64_t>(result.observations.size() + 1), "derivation");
        break;
      }
      const EvidenceItem* to_item = nullptr;
      const EvidenceItem* from_item = nullptr;
      for (const EvidenceItem& candidate : trimmed) {
        if (candidate.header.id == delta.to_id) {
          to_item = &candidate;
        } else if (candidate.header.id == delta.from_id) {
          from_item = &candidate;
        }
      }
      if (to_item == nullptr) {
        continue;
      }
      LossObservation observation = base_observation(*to_item, "counter-delta", LossSemantics::DirectLoss);
      observation.id = delta.id;
      // Both endpoints of the interval decide whether it is current: a delta
      // measured against a baseline that came back from disk is partly
      // historical and must not be presented as wholly current.
      if (from_item != nullptr && from_item->header.recovered_from_persistence) {
        observation.recovered_from_persistence = true;
      }
      apply_counter_delta(observation, delta, *to_item, policy.counter);
      result.observations.push_back(std::move(observation));
    }
  }

  // ---- endpoint comparisons ---------------------------------------------
  std::map<EndpointGroupKey, std::vector<EvidenceItem>> endpoint_groups;
  for (const EvidenceItem& item : items) {
    if (!std::holds_alternative<EndpointReport>(item.payload)) {
      continue;
    }
    if (check_topology && !subject_is_declared(declared, item.header.subject)) {
      continue;
    }
    EndpointGroupKey key{};
    if (const auto flow = item.header.subject.as_flow(); flow.ok()) {
      key.flow = flow.value();
    }
    key.generation = item.header.generation;
    key.epoch = item.header.epoch;
    endpoint_groups[key].push_back(item);
  }

  for (auto& entry : endpoint_groups) {
    std::vector<EvidenceItem>& group = entry.second;
    std::sort(group.begin(), group.end(), [](const EvidenceItem& lhs, const EvidenceItem& rhs) {
      return canonical_less(lhs, rhs);
    });
    std::vector<bool> consumed(group.size(), false);
    for (std::size_t i = 0; i < group.size(); ++i) {
      if (consumed[i]) {
        continue;
      }
      const auto* sender = std::get_if<EndpointReport>(&group[i].payload);
      if (sender == nullptr || sender->role != EndpointRole::Sender) {
        continue;
      }
      std::size_t best = group.size();
      Duration best_distance{};
      for (std::size_t j = 0; j < group.size(); ++j) {
        if (j == i || consumed[j]) {
          continue;
        }
        const auto* candidate = std::get_if<EndpointReport>(&group[j].payload);
        if (candidate == nullptr || candidate->role != EndpointRole::Receiver) {
          continue;
        }
        if (candidate->node == sender->node) {
          continue;
        }
        const Duration distance =
            (group[j].header.observed_at.value - group[i].header.observed_at.value).abs();
        if (distance > policy.endpoint_pair_window) {
          continue;
        }
        if (best == group.size() || distance < best_distance) {
          best = j;
          best_distance = distance;
        }
      }
      if (best == group.size()) {
        LossObservation observation =
            base_observation(group[i], "endpoint-unpaired", LossSemantics::RatioLoss);
        observation.validity = ObservationValidity::Insufficient;
        observation.detail = "sender report has no receiver report inside the pairing window";
        result.observations.push_back(std::move(observation));
        consumed[i] = true;
        continue;
      }
      consumed[i] = true;
      consumed[best] = true;
      const auto* receiver = std::get_if<EndpointReport>(&group[best].payload);
      if (receiver == nullptr) {
        continue;
      }
      LossObservation observation =
          base_observation(group[i], "endpoint-pair", LossSemantics::RatioLoss);
      observation.id = MeasurementId::from_value(
          combine_hash(group[i].header.id.value(), group[best].header.id.value()));
      // A comparison is only as current as the older of the two reports.
      observation.recovered_from_persistence =
          group[i].header.recovered_from_persistence || group[best].header.recovered_from_persistence;
      observation.detail = "sender=" + group[i].header.id.to_string() + " receiver=" +
                           group[best].header.id.to_string();
      if (sender->counter_reset || receiver->counter_reset) {
        observation.validity = ObservationValidity::Discontinuity;
        observation.discontinuity = DiscontinuityKind::CounterReset;
        result.observations.push_back(std::move(observation));
        continue;
      }
      if (receiver->count > sender->count) {
        observation.validity = ObservationValidity::Implausible;
        result.observations.push_back(std::move(observation));
        continue;
      }
      const std::uint64_t offered = sender->count;
      const std::uint64_t lost = offered - receiver->count;
      observation.offered = offered;
      observation.offered_known = true;
      observation.lost = lost;
      if (offered == 0) {
        observation.validity = ObservationValidity::UndefinedRatio;
        result.observations.push_back(std::move(observation));
        continue;
      }
      std::uint32_t ratio = 0;
      if (!checked::ratio_basis_points(lost, offered, ratio)) {
        observation.validity = ObservationValidity::UndefinedRatio;
        result.observations.push_back(std::move(observation));
        continue;
      }
      observation.ratio_bp = ratio;
      observation.ratio_defined = true;
      result.observations.push_back(std::move(observation));
    }
    for (std::size_t i = 0; i < group.size(); ++i) {
      if (consumed[i]) {
        continue;
      }
      LossObservation observation =
          base_observation(group[i], "endpoint-unpaired", LossSemantics::RatioLoss);
      observation.validity = ObservationValidity::Insufficient;
      observation.detail = "endpoint report has no counterpart inside the pairing window";
      result.observations.push_back(std::move(observation));
      consumed[i] = true;
    }
  }

  std::sort(result.observations.begin(), result.observations.end(),
            [](const LossObservation& lhs, const LossObservation& rhs) {
              if (lhs.observed_at != rhs.observed_at) {
                return lhs.observed_at < rhs.observed_at;
              }
              if (lhs.source != rhs.source) {
                return lhs.source < rhs.source;
              }
              if (lhs.subject != rhs.subject) {
                return lhs.subject < rhs.subject;
              }
              return lhs.id < rhs.id;
            });
  std::sort(result.rejections.begin(), result.rejections.end(),
            [](const RejectionNote& lhs, const RejectionNote& rhs) {
              if (lhs.source != rhs.source) {
                return lhs.source < rhs.source;
              }
              return lhs.id < rhs.id;
            });
  return result;
}

}  // namespace loss_observatory
