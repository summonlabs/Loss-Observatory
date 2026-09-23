#include <limits>
#include <string>
#include <vector>

#include "fixtures.hpp"
#include "loss_observatory/evidence/counter.hpp"
#include "loss_observatory/evidence/derive.hpp"

using namespace loss_observatory;

namespace {

const SourceId kSource = SourceId::from_canonical_text("src");
const EpochId kEpoch = EpochId::from_canonical_text("epoch/1");
const GenerationId kGeneration = GenerationId::from_canonical_text("gen/1");
const SubjectRef kSubject = SubjectRef::queue(QueueId::from_canonical_text("q1"));
const CounterId kCounter = CounterId::from_canonical_text("c1");
const Timestamp kStart = Timestamp::from_unix_seconds(1000);

EvidenceItem sample(std::uint64_t index, std::uint64_t value, std::uint8_t bits,
                    CounterScope scope = CounterScope::DroppedPackets, EpochId epoch = kEpoch,
                    GenerationId generation = kGeneration, Timestamp at = Timestamp{},
                    SubjectRef subject = kSubject) {
  EvidenceItem item{};
  item.header.id = MeasurementId::from_value(1000 + index);
  item.header.source = kSource;
  item.header.epoch = epoch;
  item.header.generation = generation;
  item.header.source_sequence = SequenceId::from_value(index);
  const Timestamp instant = at.is_zero() ? kStart + Duration::from_seconds(static_cast<std::int64_t>(index))
                                         : at;
  item.header.observed_at.value = instant;
  item.header.received_at.value = instant;
  item.header.method = MeasurementMethod::CounterDelta;
  item.header.subject = subject;
  item.header.granularity = Granularity::Queue;
  CounterSample sample_value{};
  sample_value.counter = kCounter;
  sample_value.scope = scope;
  sample_value.value = value;
  sample_value.width_bits = bits;
  sample_value.queue = QueueId::from_canonical_text("q1");
  item.payload = sample_value;
  return item;
}

std::vector<CounterDelta> deltas_of(std::vector<EvidenceItem> items, CounterPolicy policy = {}) {
  std::vector<EvidenceItem> ordered = canonical_order(std::move(items));
  return derive_counter_deltas(ordered, policy);
}

DerivationContext topology_free_context() {
  DerivationContext context{};
  context.topology = nullptr;
  context.generations = nullptr;
  context.sources = nullptr;
  context.clock_synchronized = false;
  return context;
}

DerivePolicy relaxed_policy() {
  DerivePolicy policy{};
  policy.require_declared_topology = false;
  return policy;
}

}  // namespace

LO_TEST(counter, monotonic_readings_produce_monotonic_deltas) {
  const std::vector<CounterDelta> deltas =
      deltas_of({sample(1, 100, 32), sample(2, 130, 32), sample(3, 130, 32)});
  LO_REQUIRE(deltas.size() == 2ULL);
  LO_CHECK(deltas[0].state == CounterDeltaState::Valid);
  LO_CHECK_EQ(deltas[0].delta, 30ULL);
  LO_CHECK(deltas[1].state == CounterDeltaState::Valid);
  LO_CHECK_EQ(deltas[1].delta, 0ULL);
  LO_CHECK_EQ(deltas[0].elapsed.nanos(), 1000000000LL);
}

LO_TEST(counter, a_single_reading_reports_a_missing_baseline) {
  const std::vector<CounterDelta> deltas = deltas_of({sample(1, 100, 32)});
  LO_REQUIRE(deltas.size() == 1ULL);
  LO_CHECK(deltas[0].state == CounterDeltaState::MissingBaseline);
  LO_CHECK(!deltas[0].is_discontinuity());
}

LO_TEST(counter, reset_is_a_discontinuity_not_loss) {
  // The counter drops from a value far below the top of its 32-bit range.
  const std::vector<CounterDelta> deltas = deltas_of({sample(1, 100000, 32), sample(2, 12, 32)});
  LO_REQUIRE(deltas.size() == 1ULL);
  LO_CHECK(deltas[0].state == CounterDeltaState::ResetDetected);
  LO_CHECK(deltas[0].is_discontinuity());
  LO_CHECK_EQ(deltas[0].delta, 0ULL);

  const DerivationResult derived =
      derive_observations({sample(1, 100000, 32), sample(2, 12, 32)}, topology_free_context(),
                          relaxed_policy());
  LO_REQUIRE(derived.observations.size() == 1ULL);
  LO_CHECK(derived.observations[0].validity == ObservationValidity::Discontinuity);
  LO_CHECK(derived.observations[0].discontinuity == DiscontinuityKind::CounterReset);
  LO_CHECK_EQ(derived.observations[0].lost, 0ULL);
}

LO_TEST(counter, wrap_near_the_top_of_the_range_is_detected_and_reported) {
  const std::uint64_t near_top = 0xFFFFFFFFULL - 2;
  const std::vector<CounterDelta> deltas = deltas_of({sample(1, near_top, 32), sample(2, 3, 32)});
  LO_REQUIRE(deltas.size() == 1ULL);
  LO_CHECK(deltas[0].state == CounterDeltaState::WrapDetected);
  LO_CHECK_EQ(deltas[0].delta, 6ULL);

  // Policy decides whether the recovered delta is usable. The default is to
  // treat the wrap as a discontinuity and require a fresh baseline.
  const DerivationResult conservative =
      derive_observations({sample(1, near_top, 32), sample(2, 3, 32)}, topology_free_context(),
                          relaxed_policy());
  LO_REQUIRE(conservative.observations.size() == 1ULL);
  LO_CHECK(conservative.observations[0].validity == ObservationValidity::Discontinuity);
  LO_CHECK_EQ(conservative.observations[0].lost, 0ULL);

  DerivePolicy accepting = relaxed_policy();
  accepting.counter.accept_wrapped_delta = true;
  const DerivationResult permissive =
      derive_observations({sample(1, near_top, 32), sample(2, 3, 32)}, topology_free_context(), accepting);
  LO_REQUIRE(permissive.observations.size() == 1ULL);
  LO_CHECK(permissive.observations[0].validity == ObservationValidity::Valid);
  LO_CHECK_EQ(permissive.observations[0].lost, 6ULL);
}

LO_TEST(counter, decrease_without_a_declared_width_is_unresolved) {
  const std::vector<CounterDelta> deltas = deltas_of({sample(1, 100000, 0), sample(2, 5, 0)});
  LO_REQUIRE(deltas.size() == 1ULL);
  LO_CHECK(deltas[0].state == CounterDeltaState::DiscontinuityUnresolved);
  LO_CHECK(deltas[0].is_discontinuity());
}

LO_TEST(counter, wrap_handling_can_be_disabled_by_policy) {
  CounterPolicy policy{};
  policy.allow_wrap = false;
  const std::uint64_t near_top = 0xFFFFFFFFULL - 1;
  const std::vector<CounterDelta> deltas = deltas_of({sample(1, near_top, 32), sample(2, 2, 32)}, policy);
  LO_REQUIRE(deltas.size() == 1ULL);
  LO_CHECK(deltas[0].state == CounterDeltaState::ResetDetected);
}

LO_TEST(counter, implausible_jumps_are_flagged_not_reported_as_loss) {
  CounterPolicy policy{};
  policy.implausible_delta_threshold = 1000;
  const std::vector<CounterDelta> deltas = deltas_of({sample(1, 0, 64), sample(2, 5000, 64)}, policy);
  LO_REQUIRE(deltas.size() == 1ULL);
  LO_CHECK(deltas[0].state == CounterDeltaState::ImplausibleDelta);

  DerivePolicy derive_policy = relaxed_policy();
  derive_policy.counter.implausible_delta_threshold = 1000;
  const DerivationResult derived =
      derive_observations({sample(1, 0, 64), sample(2, 5000, 64)}, topology_free_context(), derive_policy);
  LO_REQUIRE(derived.observations.size() == 1ULL);
  LO_CHECK(derived.observations[0].validity == ObservationValidity::Implausible);
  LO_CHECK_EQ(derived.observations[0].lost, 0ULL);
}

LO_TEST(counter, out_of_order_and_epoch_and_generation_changes_are_discontinuities) {
  EvidenceItem out_of_order = sample(2, 200, 32, CounterScope::DroppedPackets, kEpoch, kGeneration,
                                     Timestamp::from_unix_seconds(900));
  const std::vector<CounterDelta> stale =
      deltas_of({sample(1, 100, 32, CounterScope::DroppedPackets, kEpoch, kGeneration,
                        Timestamp::from_unix_seconds(1000)),
                 out_of_order});
  LO_REQUIRE(stale.size() == 1ULL);
  LO_CHECK(stale[0].state == CounterDeltaState::OutOfOrder);

  const EpochId other_epoch = EpochId::from_canonical_text("epoch/2");
  const std::vector<CounterDelta> epoch_changed =
      deltas_of({sample(1, 100, 32), sample(2, 200, 32, CounterScope::DroppedPackets, other_epoch)});
  LO_REQUIRE(epoch_changed.size() == 1ULL);
  LO_CHECK(epoch_changed[0].state == CounterDeltaState::EpochChanged);

  const GenerationId other_generation = GenerationId::from_canonical_text("gen/2");
  const std::vector<CounterDelta> generation_changed =
      deltas_of({sample(1, 100, 32), sample(2, 200, 32, CounterScope::DroppedPackets, kEpoch, other_generation)});
  LO_REQUIRE(generation_changed.size() == 1ULL);
  LO_CHECK(generation_changed[0].state == CounterDeltaState::GenerationChanged);
}

LO_TEST(counter, bindings_are_grouped_independently) {
  EvidenceItem first = sample(1, 100, 32);
  EvidenceItem second = sample(2, 300, 32);
  std::get<CounterSample>(second.payload).counter = CounterId::from_canonical_text("c2");
  EvidenceItem third = sample(3, 400, 32);
  BoundNotes notes{};
  const std::vector<std::vector<EvidenceItem>> groups =
      group_counter_series(std::vector<EvidenceItem>{first, second, third}, 16, notes);
  LO_CHECK_EQ(groups.size(), 2ULL);
}

LO_TEST(counter, drop_counters_are_direct_loss_and_throughput_counters_are_not) {
  const DerivationResult drops =
      derive_observations({sample(1, 10, 32), sample(2, 25, 32)}, topology_free_context(), relaxed_policy());
  LO_REQUIRE(drops.observations.size() == 1ULL);
  LO_CHECK(drops.observations[0].semantics == LossSemantics::DirectLoss);
  LO_CHECK(drops.observations[0].validity == ObservationValidity::Valid);
  LO_CHECK_EQ(drops.observations[0].lost, 15ULL);
  LO_CHECK(!drops.observations[0].offered_known);
  LO_CHECK(!drops.observations[0].ratio_defined);

  const DerivationResult throughput = derive_observations(
      {sample(1, 10, 32, CounterScope::ReceivedPackets), sample(2, 25, 32, CounterScope::ReceivedPackets)},
      topology_free_context(), relaxed_policy());
  LO_REQUIRE(throughput.observations.size() == 1ULL);
  LO_CHECK(throughput.observations[0].semantics == LossSemantics::NonLoss);
  LO_CHECK_EQ(throughput.observations[0].lost, 0ULL);
  LO_CHECK_EQ(throughput.observations[0].offered, 15ULL);
  LO_CHECK(!throughput.observations[0].ratio_defined);
}

LO_TEST(counter, series_point_bound_is_reported) {
  std::vector<EvidenceItem> items;
  for (std::uint64_t i = 1; i <= 20; ++i) {
    items.push_back(sample(i, i * 10, 32));
  }
  DerivePolicy policy = relaxed_policy();
  policy.counter.max_points_per_series = 5;
  const DerivationResult derived = derive_observations(items, topology_free_context(), policy);
  LO_CHECK(!derived.bounds.empty());
  LO_CHECK(derived.bounds.truncated() || !derived.bounds.notes().empty());
}

LO_TEST(probe, round_trip_reports_a_ratio_and_rejects_impossible_counts) {
  EvidenceItem item{};
  item.header.id = MeasurementId::from_value(77);
  item.header.source = kSource;
  item.header.epoch = kEpoch;
  item.header.generation = kGeneration;
  item.header.source_sequence = SequenceId::from_value(1);
  item.header.observed_at.value = kStart;
  item.header.received_at.value = kStart;
  item.header.method = MeasurementMethod::ProbeRoundTrip;
  item.header.subject = SubjectRef::path(PathId::from_canonical_text("p1"));
  item.header.granularity = Granularity::Path;
  ProbeReport report{};
  report.probe = ProbeId::from_canonical_text("probe/1");
  report.sent = 1000;
  report.received = 990;
  report.timed_out = 10;
  item.payload = report;

  const DerivationResult derived = derive_observations({item}, topology_free_context(), relaxed_policy());
  LO_REQUIRE(derived.observations.size() == 1ULL);
  LO_CHECK(derived.observations[0].validity == ObservationValidity::Valid);
  LO_CHECK_EQ(derived.observations[0].lost, 10ULL);
  LO_CHECK_EQ(derived.observations[0].offered, 1000ULL);
  LO_CHECK_EQ(derived.observations[0].ratio_bp, 100U);

  ProbeReport impossible = report;
  impossible.received = 1001;
  item.payload = impossible;
  const DerivationResult implausible = derive_observations({item}, topology_free_context(), relaxed_policy());
  LO_REQUIRE(implausible.observations.size() == 1ULL);
  LO_CHECK(implausible.observations[0].validity == ObservationValidity::Implausible);

  ProbeReport empty = report;
  empty.sent = 0;
  empty.received = 0;
  item.payload = empty;
  const DerivationResult undefined_ratio =
      derive_observations({item}, topology_free_context(), relaxed_policy());
  LO_REQUIRE(undefined_ratio.observations.size() == 1ULL);
  LO_CHECK(undefined_ratio.observations[0].validity == ObservationValidity::UndefinedRatio);
  LO_CHECK(!undefined_ratio.observations[0].ratio_defined);
}

LO_TEST(probe, one_way_probes_are_unsupported_without_a_synchronised_clock) {
  EvidenceItem item{};
  item.header.id = MeasurementId::from_value(78);
  item.header.source = kSource;
  item.header.epoch = kEpoch;
  item.header.generation = kGeneration;
  item.header.source_sequence = SequenceId::from_value(1);
  item.header.observed_at.value = kStart;
  item.header.received_at.value = kStart;
  item.header.method = MeasurementMethod::ProbeOneWay;
  item.header.subject = SubjectRef::link(LinkId::from_canonical_text("l1"));
  item.header.granularity = Granularity::Link;
  ProbeReport report{};
  report.probe = ProbeId::from_canonical_text("probe/2");
  report.sent = 10;
  report.received = 9;
  report.one_way = true;
  item.payload = report;

  const DerivationResult derived = derive_observations({item}, topology_free_context(), relaxed_policy());
  LO_REQUIRE(derived.observations.size() == 1ULL);
  LO_CHECK(derived.observations[0].validity == ObservationValidity::UnsupportedMethod);
}

LO_TEST(sequence, gaps_are_computed_from_the_span_and_restarts_are_discontinuities) {
  EvidenceItem item{};
  item.header.id = MeasurementId::from_value(90);
  item.header.source = kSource;
  item.header.epoch = kEpoch;
  item.header.generation = kGeneration;
  item.header.source_sequence = SequenceId::from_value(1);
  item.header.observed_at.value = kStart;
  item.header.received_at.value = kStart;
  item.header.method = MeasurementMethod::SequenceGap;
  item.header.subject = SubjectRef::flow(FlowId::from_canonical_text("f1"));
  item.header.granularity = Granularity::Flow;
  SequenceReport report{};
  report.lowest_sequence = 100;
  report.highest_sequence = 199;
  report.received_count = 95;
  item.payload = report;

  const DerivationResult derived = derive_observations({item}, topology_free_context(), relaxed_policy());
  LO_REQUIRE(derived.observations.size() == 1ULL);
  LO_CHECK_EQ(derived.observations[0].offered, 100ULL);
  LO_CHECK_EQ(derived.observations[0].lost, 5ULL);
  LO_CHECK_EQ(derived.observations[0].ratio_bp, 500U);

  SequenceReport restart = report;
  restart.sequence_restart = true;
  item.payload = restart;
  const DerivationResult restarted = derive_observations({item}, topology_free_context(), relaxed_policy());
  LO_REQUIRE(restarted.observations.size() == 1ULL);
  LO_CHECK(restarted.observations[0].validity == ObservationValidity::Discontinuity);
  LO_CHECK(restarted.observations[0].discontinuity == DiscontinuityKind::SequenceRestart);

  SequenceReport impossible = report;
  impossible.received_count = 101;
  item.payload = impossible;
  const DerivationResult implausible = derive_observations({item}, topology_free_context(), relaxed_policy());
  LO_REQUIRE(implausible.observations.size() == 1ULL);
  LO_CHECK(implausible.observations[0].validity == ObservationValidity::Implausible);

  SequenceReport overflow = report;
  overflow.lowest_sequence = 0;
  overflow.highest_sequence = std::numeric_limits<std::uint64_t>::max();
  overflow.received_count = 1;
  item.payload = overflow;
  const DerivationResult saturated = derive_observations({item}, topology_free_context(), relaxed_policy());
  LO_REQUIRE(saturated.observations.size() == 1ULL);
  LO_CHECK(saturated.observations[0].validity == ObservationValidity::Implausible);
}

LO_TEST(endpoint, paired_reports_are_compared_and_unpaired_reports_are_incomplete) {
  auto endpoint_item = [](std::uint64_t id, EndpointRole role, const char* node, std::uint64_t count,
                          Timestamp at) {
    EvidenceItem item{};
    item.header.id = MeasurementId::from_value(id);
    item.header.source = kSource;
    item.header.epoch = kEpoch;
    item.header.generation = kGeneration;
    item.header.source_sequence = SequenceId::from_value(id);
    item.header.observed_at.value = at;
    item.header.received_at.value = at;
    item.header.method = MeasurementMethod::EndpointComparison;
    item.header.subject = SubjectRef::flow(FlowId::from_canonical_text("f1"));
    item.header.granularity = Granularity::Flow;
    EndpointReport report{};
    report.role = role;
    report.node = NodeId::from_canonical_text(node);
    report.port = PortId::from_canonical_text("0");
    report.count = count;
    item.payload = report;
    return item;
  };

  const DerivationResult paired = derive_observations(
      {endpoint_item(1, EndpointRole::Sender, "n1", 1000, kStart),
       endpoint_item(2, EndpointRole::Receiver, "n2", 990, kStart + Duration::from_millis(10))},
      topology_free_context(), relaxed_policy());
  LO_REQUIRE(paired.observations.size() == 1ULL);
  LO_CHECK(paired.observations[0].validity == ObservationValidity::Valid);
  LO_CHECK_EQ(paired.observations[0].lost, 10ULL);
  LO_CHECK_EQ(paired.observations[0].offered, 1000ULL);

  const DerivationResult unpaired = derive_observations(
      {endpoint_item(3, EndpointRole::Sender, "n1", 1000, kStart),
       endpoint_item(4, EndpointRole::Receiver, "n2", 990, kStart + Duration::from_seconds(30))},
      topology_free_context(), relaxed_policy());
  LO_REQUIRE(unpaired.observations.size() == 2ULL);
  for (const LossObservation& observation : unpaired.observations) {
    LO_CHECK(observation.validity == ObservationValidity::Insufficient);
  }
}

LO_TEST(derive, canonical_order_is_independent_of_input_order) {
  std::vector<EvidenceItem> forward{sample(1, 100, 32), sample(2, 130, 32), sample(3, 160, 32)};
  std::vector<EvidenceItem> reverse{forward[2], forward[1], forward[0]};
  const DerivationResult a = derive_observations(forward, topology_free_context(), relaxed_policy());
  const DerivationResult b = derive_observations(reverse, topology_free_context(), relaxed_policy());
  LO_REQUIRE(a.observations.size() == b.observations.size());
  for (std::size_t i = 0; i < a.observations.size(); ++i) {
    LO_CHECK(a.observations[i].id == b.observations[i].id);
    LO_CHECK_EQ(a.observations[i].lost, b.observations[i].lost);
    LO_CHECK(a.observations[i].validity == b.observations[i].validity);
  }
}
