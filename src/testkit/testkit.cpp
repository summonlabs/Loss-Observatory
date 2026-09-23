#include "loss_observatory/testkit/testkit.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "loss_observatory/report.hpp"

namespace loss_observatory::testkit {
namespace {

[[nodiscard]] std::string key(const char* prefix, std::uint64_t seed, std::size_t index) {
  return std::string(prefix) + "/" + std::to_string(seed) + "/" + std::to_string(index);
}

[[nodiscard]] MeasurementId measurement_id(std::uint64_t seed, std::size_t index) {
  std::uint64_t mixed = combine_hash(seed, index);
  return MeasurementId::from_value(mixed == 0 ? 1 : mixed);
}

}  // namespace

std::uint64_t DeterministicRng::next_u64() noexcept {
  state_ += 0x9E3779B97F4A7C15ULL;
  std::uint64_t value = state_;
  value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31);
}

std::uint32_t DeterministicRng::next_u32() noexcept {
  return static_cast<std::uint32_t>(next_u64() >> 32);
}

std::uint64_t DeterministicRng::next_below(std::uint64_t bound) noexcept {
  if (bound == 0) {
    return 0;
  }
  return next_u64() % bound;
}

bool DeterministicRng::next_bool(std::uint32_t percent_true) noexcept {
  if (percent_true == 0) {
    return false;
  }
  if (percent_true >= 100) {
    return true;
  }
  return next_below(100) < percent_true;
}

Result<Scenario> install_synthetic_scenario(ObservatoryEngine& engine, const ScenarioConfig& config) {
  Scenario scenario{};
  scenario.description = "synthetic scenario seed=" + std::to_string(config.seed);
  scenario.flow = FlowId::from_canonical_text("flow/" + std::to_string(config.seed));
  scenario.path = PathId::from_canonical_text("path/" + std::to_string(config.seed));
  scenario.generation = GenerationId::from_canonical_text("gen/" + std::to_string(config.seed));
  scenario.source = SourceId::from_canonical_text(config.source_name);

  std::vector<Hop> hops;
  hops.reserve(config.hop_count);
  for (std::size_t index = 0; index < config.hop_count; ++index) {
    Hop hop{};
    hop.id = HopId::from_canonical_text(key("hop", config.seed, index));
    hop.index = static_cast<std::uint32_t>(index);
    hop.node = NodeId::from_canonical_text(key("node", config.seed, index));
    hop.ingress_port = PortId::from_canonical_text(key("in-port", config.seed, index));
    hop.egress_port = PortId::from_canonical_text(key("out-port", config.seed, index));
    hop.ingress_link = LinkId::from_canonical_text(key("in-link", config.seed, index));
    hop.egress_link = LinkId::from_canonical_text(key("out-link", config.seed, index));
    hops.push_back(hop);
    scenario.hops.push_back(hop.id);
  }
  Path path{};
  path.id = scenario.path;
  path.kind = PathKind::Synthetic;
  path.hops = hops;
  const Result<UpsertResult> path_result = engine.topology().upsert_path(path, true);
  if (!path_result.ok()) {
    return path_result.status();
  }
  const Result<Path> stored_path = engine.topology().find_path(scenario.path);
  if (!stored_path.ok()) {
    return stored_path.status();
  }

  FlowBinding flow{};
  flow.id = scenario.flow;
  flow.source = Endpoint{hops.front().node, hops.front().ingress_port};
  flow.destination = Endpoint{hops.back().node, hops.back().egress_port};
  flow.protocol = ProtocolId::from_canonical_text("proto/tcp");
  flow.path = scenario.path;
  flow.generation = scenario.generation;
  flow.binding_revision = stored_path.value().revision;
  const Result<UpsertResult> flow_result = engine.topology().upsert_flow(flow, true);
  if (!flow_result.ok()) {
    return flow_result.status();
  }

  SourceDescriptor descriptor{};
  descriptor.id = scenario.source;
  descriptor.name = config.source_name;
  descriptor.kind = config.kind;
  descriptor.authority = config.authority;
  const Result<UpsertOutcome> source_result = engine.sources().register_source(descriptor, true);
  if (!source_result.ok()) {
    return source_result.status();
  }
  const Result<EpochId> epoch =
      engine.sources().activate_incarnation(scenario.source, "synthetic-incarnation-1", config.start);
  if (!epoch.ok()) {
    return epoch.status();
  }
  scenario.epoch = epoch.value();
  return scenario;
}

std::vector<EvidenceItem> make_counter_series(const ScenarioConfig& config, CounterScope scope,
                                              std::uint64_t start_value,
                                              std::vector<std::uint64_t> increments) {
  std::vector<EvidenceItem> items;
  const SourceId source = SourceId::from_canonical_text(config.source_name);
  const EpochId epoch = EpochId::from_canonical_text("epoch/" + std::to_string(config.seed));
  const QueueId queue = QueueId::from_canonical_text("queue/" + std::to_string(config.seed));
  const CounterId counter =
      CounterId::from_canonical_text("counter/" + std::to_string(config.seed) + "/" +
                                     std::string(loss_observatory::to_string(scope)));

  std::uint64_t value = start_value;
  std::uint64_t sequence = 0;
  Timestamp instant = config.start;
  for (std::size_t index = 0; index <= increments.size(); ++index) {
    EvidenceItem item{};
    item.header.id = measurement_id(config.seed, index);
    item.header.source = source;
    item.header.epoch = epoch;
    item.header.generation = GenerationId::from_canonical_text("gen/" + std::to_string(config.seed));
    item.header.topology_revision = RevisionId::from_canonical_text("rev/" + std::to_string(config.seed));
    item.header.source_sequence = SequenceId::from_value(++sequence);
    item.header.observed_at.value = instant;
    item.header.received_at.value = instant;
    item.header.method = MeasurementMethod::CounterDelta;
    item.header.subject = SubjectRef::queue(queue);
    item.header.granularity = Granularity::Queue;
    CounterSample sample{};
    sample.counter = counter;
    sample.scope = scope;
    sample.value = value;
    sample.width_bits = config.counter_width_bits;
    sample.queue = queue;
    item.payload = sample;
    items.push_back(std::move(item));
    if (index < increments.size()) {
      value += increments[index];
      instant = instant + config.sample_interval;
    }
  }
  return items;
}

Result<Scenario> make_drop_counter_scenario(ObservatoryEngine& engine, const ScenarioConfig& config) {
  Result<Scenario> installed = install_synthetic_scenario(engine, config);
  if (!installed.ok()) {
    return installed.status();
  }
  Scenario scenario = installed.value();

  DeterministicRng rng(config.seed);
  const std::uint64_t interval_packets = 1000;
  const std::uint64_t drops_per_interval =
      (interval_packets * static_cast<std::uint64_t>(config.loss_ratio_bp)) / 10000;

  const std::size_t reset_at =
      config.inject_counter_reset && config.sample_count > 4 ? config.sample_count / 2 : config.sample_count;
  const std::size_t wrap_at =
      config.inject_counter_wrap && config.sample_count > 4 ? config.sample_count / 3 : config.sample_count;
  const std::size_t generation_at =
      config.inject_generation_change && config.sample_count > 4 ? config.sample_count / 2 : config.sample_count;

  const std::uint64_t modulus = config.counter_width_bits == 0 || config.counter_width_bits >= 64
                                    ? 0
                                    : (1ULL << config.counter_width_bits);

  const CounterId counter = CounterId::from_canonical_text("drops/" + std::to_string(config.seed));

  std::uint64_t value = 1000;
  Timestamp instant = config.start;
  std::uint64_t sequence = 0;
  std::uint64_t injected_drops = 0;

  for (std::size_t index = 0; index < config.sample_count; ++index) {
    EvidenceItem item{};
    item.header.id = measurement_id(config.seed, index);
    item.header.source = scenario.source;
    item.header.epoch = scenario.epoch;
    item.header.generation = scenario.generation;
    if (index >= generation_at) {
      item.header.generation =
          GenerationId::from_canonical_text("gen/" + std::to_string(config.seed) + "/next");
    }
    const Result<Path> path = engine.topology().find_path(scenario.path);
    if (path.ok()) {
      item.header.topology_revision = path.value().revision;
    }
    item.header.source_sequence = SequenceId::from_value(++sequence);
    item.header.observed_at.value = instant;
    item.header.received_at.value = instant;
    item.header.method = MeasurementMethod::CounterDelta;
    item.header.subject = SubjectRef::flow(scenario.flow);
    item.header.granularity = Granularity::Flow;
    CounterSample sample{};
    sample.counter = counter;
    sample.scope = CounterScope::DroppedPackets;
    sample.value = value;
    sample.width_bits = config.counter_width_bits;
    item.payload = sample;
    const Result<IngestOutcome> outcome = engine.ingest(item);
    if (!outcome.ok()) {
      return outcome.status();
    }
    scenario.items.push_back(std::move(item));

    if (index + 1 >= config.sample_count) {
      break;
    }
    if (index == reset_at) {
      // A restart of the counter: the next reading is far below the previous
      // one and nowhere near the top of the declared range.
      value = 5;
    } else if (index == wrap_at && modulus != 0) {
      value = modulus - 3;
    } else {
      const std::uint64_t jitter = rng.next_below(3);
      value += drops_per_interval + jitter;
      injected_drops += drops_per_interval + jitter;
      if (modulus != 0 && value >= modulus) {
        value %= modulus;
      }
    }
    instant = instant + config.sample_interval;
  }
  scenario.injected_drops = injected_drops;
  scenario.description += " drops=" + std::to_string(injected_drops);
  return scenario;
}

bool stale_evidence_cannot_prove_current_loss(const Classification& classification) {
  if (!asserts_loss(classification.klass)) {
    return true;
  }
  return classification.freshness.fresh > 0 && classification.evidence_admissible > 0;
}

bool discontinuities_are_explicit(const Classification& classification,
                                  std::span<const LossObservation> observations) {
  bool any_discontinuity = false;
  bool any_valid_loss_capable = false;
  for (const LossObservation& observation : observations) {
    if (observation.validity == ObservationValidity::Discontinuity) {
      any_discontinuity = true;
    }
    if (observation.validity == ObservationValidity::Valid &&
        (observation.semantics == LossSemantics::DirectLoss ||
         observation.semantics == LossSemantics::RatioLoss)) {
      any_valid_loss_capable = true;
    }
  }
  if (!any_discontinuity) {
    return true;
  }
  if (any_valid_loss_capable) {
    return true;
  }
  return classification.klass == LossClass::Discontinuity ||
         classification.klass == LossClass::StaleEvidence ||
         classification.klass == LossClass::AbsentEvidence;
}

bool disagreement_is_retained(const Classification& classification,
                              const ConflictResolution& resolution) {
  if (!resolution.conflicted) {
    return true;
  }
  if (!resolution.resolved) {
    return classification.klass == LossClass::ConflictingEvidence;
  }
  // Every non-authoritative claim must still be present in the output.
  return !resolution.retained_lower_authority.empty() &&
         resolution.claims.size() == resolution.retained_lower_authority.size() + 1 &&
         classification.klass != LossClass::ConflictingEvidence;
}

bool localization_within_evidence_granularity(const LocalizationResult& result,
                                              std::span<const LossObservation> observations) {
  (void)observations;
  if (result.achieved_granularity != Granularity::Unknown &&
      result.evidence_granularity != Granularity::Unknown &&
      is_finer_than(result.achieved_granularity, result.evidence_granularity)) {
    return false;
  }
  for (const LocalizedSegment& segment : result.segments) {
    if (segment.granularity == Granularity::Unknown) {
      continue;
    }
    if (result.evidence_granularity != Granularity::Unknown &&
        is_finer_than(segment.granularity, result.evidence_granularity)) {
      return false;
    }
    if (!result.path_known && is_finer_than(segment.granularity, Granularity::Flow)) {
      return false;
    }
  }
  return true;
}

bool restart_does_not_revive(const EpisodeQueryResult& history,
                             std::span<const LossObservation> observations,
                             const FreshnessPolicy& policy, const FreshnessContext& context,
                             Timestamp now) {
  for (const LossEpisode& episode : history.episodes) {
    if (episode.open()) {
      return false;
    }
  }
  for (const LossObservation& observation : observations) {
    if (!observation.recovered_from_persistence) {
      continue;
    }
    if (assess_freshness(observation, now, policy, context).admissible) {
      return false;
    }
  }
  return true;
}

bool reasons_are_renderable(std::span<const ReasonCode> reasons) {
  for (const ReasonCode reason : reasons) {
    const std::string_view text = loss_observatory::to_string(reason);
    if (text.empty() || text == "none") {
      return false;
    }
  }
  return true;
}

}  // namespace loss_observatory::testkit
