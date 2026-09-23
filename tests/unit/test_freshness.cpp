#include <string>
#include <vector>

#include "fixtures.hpp"
#include "loss_observatory/freshness.hpp"

using namespace loss_observatory;

namespace {

LossObservation observation_at(Timestamp at, bool recovered = false) {
  LossObservation observation{};
  observation.id = MeasurementId::from_value(1);
  observation.evidence_id = MeasurementId::from_value(2);
  observation.source = SourceId::from_canonical_text("src");
  observation.epoch = EpochId::from_canonical_text("epoch/1");
  observation.generation = GenerationId::from_canonical_text("gen/1");
  observation.subject = SubjectRef::flow(FlowId::from_canonical_text("flow/1"));
  observation.granularity = Granularity::Flow;
  observation.method = MeasurementMethod::SequenceGap;
  observation.semantics = LossSemantics::RatioLoss;
  observation.validity = ObservationValidity::Valid;
  observation.observed_at.value = at;
  observation.received_at.value = at;
  observation.lost = 10;
  observation.offered = 1000;
  observation.ratio_defined = true;
  observation.ratio_bp = 100;
  observation.recovered_from_persistence = recovered;
  return observation;
}

SourceRegistry& registry_with_source() {
  static SourceRegistry registry{};
  static bool initialised = false;
  if (!initialised) {
    SourceDescriptor descriptor{};
    descriptor.id = SourceId::from_canonical_text("src");
    descriptor.name = "leaf-a";
    descriptor.kind = SourceKind::CounterTelemetry;
    descriptor.authority = SourceAuthority::Primary;
    (void)registry.register_source(descriptor);
    initialised = true;
  }
  return registry;
}

}  // namespace

LO_TEST(freshness, young_evidence_is_fresh_and_admissible) {
  const Timestamp now = Timestamp::from_unix_seconds(1000);
  const FreshnessPolicy policy{};
  FreshnessContext context{};
  const FreshnessAssessment assessment =
      assess_freshness(observation_at(now - Duration::from_seconds(5)), now, policy, context);
  LO_CHECK(assessment.state == Freshness::Fresh);
  LO_CHECK(assessment.admissible);
  LO_CHECK(assessment.reason == StaleReason::None);
}

LO_TEST(freshness, aged_evidence_is_stale_and_inadmissible) {
  const Timestamp now = Timestamp::from_unix_seconds(1000);
  FreshnessPolicy policy{};
  policy.max_age = Duration::from_seconds(10);
  FreshnessContext context{};
  const FreshnessAssessment assessment =
      assess_freshness(observation_at(now - Duration::from_seconds(11)), now, policy, context);
  LO_CHECK(assessment.state == Freshness::Stale);
  LO_CHECK(assessment.reason == StaleReason::AgeExceeded);
  LO_CHECK(!assessment.admissible);
  LO_CHECK_EQ(assessment.age.nanos(), 11000000000LL);
}

LO_TEST(freshness, recovered_evidence_is_never_admissible_however_young) {
  const Timestamp now = Timestamp::from_unix_seconds(1000);
  const FreshnessPolicy policy{};
  FreshnessContext context{};
  const FreshnessAssessment assessment =
      assess_freshness(observation_at(now - Duration::from_millis(1), true), now, policy, context);
  LO_CHECK(assessment.state == Freshness::NotCurrent);
  LO_CHECK(assessment.reason == StaleReason::RecoveredFromPersistence);
  LO_CHECK(!assessment.admissible);
}

LO_TEST(freshness, future_dating_is_tolerated_only_inside_the_skew_window) {
  const Timestamp now = Timestamp::from_unix_seconds(1000);
  FreshnessPolicy policy{};
  policy.future_tolerance = Duration::from_seconds(5);
  FreshnessContext context{};

  const FreshnessAssessment inside =
      assess_freshness(observation_at(now + Duration::from_seconds(3)), now, policy, context);
  LO_CHECK(inside.state == Freshness::Fresh);
  LO_CHECK(inside.admissible);

  const FreshnessAssessment beyond =
      assess_freshness(observation_at(now + Duration::from_seconds(30)), now, policy, context);
  LO_CHECK(beyond.state == Freshness::FutureDated);
  LO_CHECK(beyond.reason == StaleReason::FutureDated);
  LO_CHECK(!beyond.admissible);
}

LO_TEST(freshness, undated_and_receive_before_observe_are_rejected) {
  const Timestamp now = Timestamp::from_unix_seconds(1000);
  const FreshnessPolicy policy{};
  FreshnessContext context{};

  LossObservation undated = observation_at(now);
  undated.observed_at.value = Timestamp{};
  const FreshnessAssessment missing = assess_freshness(undated, now, policy, context);
  LO_CHECK(missing.state == Freshness::Undated);
  LO_CHECK(!missing.admissible);

  LossObservation skewed = observation_at(now);
  skewed.received_at.value = now - Duration::from_seconds(60);
  const FreshnessAssessment inconsistent = assess_freshness(skewed, now, policy, context);
  LO_CHECK(inconsistent.state == Freshness::Stale);
  LO_CHECK(inconsistent.reason == StaleReason::ReceivedBeforeObserved);
  LO_CHECK(!inconsistent.admissible);
}

LO_TEST(freshness, retired_source_epoch_is_stale) {
  SourceRegistry& registry = registry_with_source();
  auto first = registry.activate_incarnation(SourceId::from_canonical_text("src"), "boot-1",
                                             Timestamp::from_unix_seconds(10));
  auto second = registry.activate_incarnation(SourceId::from_canonical_text("src"), "boot-2",
                                              Timestamp::from_unix_seconds(20));
  LO_REQUIRE(first.ok());
  LO_REQUIRE(second.ok());

  const Timestamp now = Timestamp::from_unix_seconds(1000);
  const FreshnessPolicy policy{};
  FreshnessContext context{};
  context.sources = &registry;

  LossObservation retired = observation_at(now - Duration::from_seconds(1));
  retired.epoch = first.value();
  const FreshnessAssessment assessment = assess_freshness(retired, now, policy, context);
  LO_CHECK(assessment.state == Freshness::Stale);
  LO_CHECK(assessment.reason == StaleReason::EpochRetired);
  LO_CHECK(!assessment.admissible);

  LossObservation live = observation_at(now - Duration::from_seconds(1));
  live.epoch = second.value();
  LO_CHECK(assess_freshness(live, now, policy, context).admissible);
}

LO_TEST(freshness, unknown_source_and_unknown_epoch_are_distinguished) {
  SourceRegistry registry{};
  SourceDescriptor descriptor{};
  descriptor.id = SourceId::from_canonical_text("known");
  descriptor.name = "leaf";
  descriptor.kind = SourceKind::CounterTelemetry;
  descriptor.authority = SourceAuthority::Primary;
  (void)registry.register_source(descriptor);
  auto epoch = registry.activate_incarnation(descriptor.id, "boot-1", Timestamp::from_unix_seconds(5));
  LO_REQUIRE(epoch.ok());

  const Timestamp now = Timestamp::from_unix_seconds(1000);
  const FreshnessPolicy policy{};
  FreshnessContext context{};
  context.sources = &registry;

  LossObservation unknown_source = observation_at(now - Duration::from_seconds(1));
  unknown_source.source = SourceId::from_canonical_text("ghost");
  LO_CHECK(assess_freshness(unknown_source, now, policy, context).state == Freshness::Unknown);

  LossObservation unknown_epoch = observation_at(now - Duration::from_seconds(1));
  unknown_epoch.source = descriptor.id;
  unknown_epoch.epoch = EpochId::from_canonical_text("never-activated");
  const FreshnessAssessment assessment = assess_freshness(unknown_epoch, now, policy, context);
  LO_CHECK(assessment.state == Freshness::Stale);
  LO_CHECK(assessment.reason == StaleReason::EpochUnknown);
}

LO_TEST(freshness, superseded_generation_is_stale) {
  GenerationRegistry generations{};
  const FlowId flow = FlowId::from_canonical_text("flow/1");
  const Timestamp now = Timestamp::from_unix_seconds(1000);
  const GenerationId first = GenerationId::from_canonical_text("gen/1");
  const GenerationId second = GenerationId::from_canonical_text("gen/2");
  generations.observe_flow(flow, first, now - Duration::from_seconds(10));
  generations.observe_flow(flow, second, now - Duration::from_seconds(5));

  FreshnessPolicy policy{};
  FreshnessContext context{};
  context.generations = &generations;

  LossObservation before = observation_at(now - Duration::from_seconds(8));
  before.generation = first;
  LO_CHECK(assess_freshness(before, now, policy, context).admissible);

  LossObservation old = observation_at(now - Duration::from_seconds(1));
  old.generation = first;
  const FreshnessAssessment assessment = assess_freshness(old, now, policy, context);
  LO_CHECK(assessment.state == Freshness::Stale);
  LO_CHECK(assessment.reason == StaleReason::GenerationSuperseded);

  LossObservation current = observation_at(now - Duration::from_seconds(1));
  current.generation = second;
  LO_CHECK(assess_freshness(current, now, policy, context).admissible);
}

LO_TEST(freshness, topology_revision_change_is_stale) {
  TopologyRegistry topology{};
  const FlowId flow = FlowId::from_canonical_text("flow/1");
  Path path{};
  path.id = PathId::from_canonical_text("path/1");
  Hop hop{};
  hop.id = HopId::from_canonical_text("hop/1");
  hop.index = 0;
  path.hops.push_back(hop);
  (void)topology.upsert_path(path);
  auto stored_path = topology.find_path(path.id);
  LO_REQUIRE(stored_path.ok());

  FlowBinding binding{};
  binding.id = flow;
  binding.path = path.id;
  binding.generation = GenerationId::from_canonical_text("gen/1");
  binding.binding_revision = stored_path.value().revision;
  (void)topology.upsert_flow(binding);

  const Timestamp now = Timestamp::from_unix_seconds(1000);
  FreshnessPolicy policy{};
  FreshnessContext context{};
  context.topology = &topology;

  LossObservation matching = observation_at(now - Duration::from_seconds(1));
  matching.subject = SubjectRef::flow(flow);
  matching.topology_revision = stored_path.value().revision;
  LO_CHECK(assess_freshness(matching, now, policy, context).admissible);

  // Re-declare the path with a different shape: the revision moves on and the
  // old observation can no longer describe the current path.
  Path changed = stored_path.value();
  changed.hops.front().egress_port = PortId::from_canonical_text("9");
  (void)topology.upsert_path(changed, true);

  LossObservation obsolete = observation_at(now - Duration::from_seconds(1));
  obsolete.subject = SubjectRef::flow(flow);
  obsolete.topology_revision = stored_path.value().revision;
  const FreshnessAssessment assessment = assess_freshness(obsolete, now, policy, context);
  LO_CHECK(assessment.state == Freshness::Stale);
  LO_CHECK(assessment.reason == StaleReason::TopologyRevisionChanged);
}

LO_TEST(freshness, summary_counts_every_state) {
  std::vector<FreshnessAssessment> assessments;
  FreshnessAssessment fresh{};
  fresh.state = Freshness::Fresh;
  FreshnessAssessment stale{};
  stale.state = Freshness::Stale;
  FreshnessAssessment recovered{};
  recovered.state = Freshness::NotCurrent;
  assessments.push_back(fresh);
  assessments.push_back(stale);
  assessments.push_back(recovered);
  const FreshnessSummary summary = summarize(assessments);
  LO_CHECK_EQ(summary.total, 3ULL);
  LO_CHECK_EQ(summary.fresh, 1ULL);
  LO_CHECK_EQ(summary.stale, 1ULL);
  LO_CHECK_EQ(summary.not_current, 1ULL);
  LO_CHECK(summary.has_admissible());
}
