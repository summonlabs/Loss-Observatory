#include <algorithm>
#include <string>
#include <vector>

#include "fixtures.hpp"
#include "loss_observatory/localize.hpp"
#include "loss_observatory/testkit/testkit.hpp"

using namespace loss_observatory;

namespace {

const Timestamp kNow = Timestamp::from_unix_seconds(3000);

/// Declares a three-hop path with one flow and returns the identities.
struct Topology {
  ManualClock clock{kNow};
  EngineConfig config{};
  std::unique_ptr<ObservatoryEngine> engine{};
  lofixture::Network network{};
};

std::unique_ptr<Topology> make_topology(std::size_t hop_count) {
  auto topology = std::make_unique<Topology>();
  topology->clock.set(kNow);
  topology->config = lofixture::default_config("localize");
  topology->engine = lofixture::make_engine(topology->clock, topology->config);
  auto network = lofixture::declare_linear_network(*topology->engine, hop_count, kNow, "loc");
  if (!network.ok()) {
    throw lotest::Failure{"failed to declare topology: " + network.status().to_string()};
  }
  topology->network = network.value();
  return topology;
}

LossObservation observation(const lofixture::Network& network, SubjectRef subject, Granularity granularity,
                            std::uint64_t lost, std::uint64_t offered, std::uint32_t ratio_bp) {
  LossObservation value{};
  value.id = MeasurementId::from_value(combine_hash(subject.raw_id(), lost + offered));
  value.evidence_id = MeasurementId::from_value(combine_hash(value.id.value(), 3));
  value.source = network.source;
  value.epoch = network.epoch;
  value.generation = network.generation;
  value.subject = subject;
  value.granularity = granularity;
  value.method = MeasurementMethod::SequenceGap;
  value.semantics = LossSemantics::RatioLoss;
  value.validity = ObservationValidity::Valid;
  value.observed_at.value = kNow - Duration::from_seconds(1);
  value.received_at.value = kNow - Duration::from_seconds(1);
  value.lost = lost;
  value.offered = offered;
  value.offered_known = true;
  value.ratio_defined = true;
  value.ratio_bp = ratio_bp;
  return value;
}

LocalizationResult localize(const Topology& topology, std::vector<LossObservation> observations,
                            Granularity requested = Granularity::Queue) {
  LocalizationRequest request{};
  request.flow = topology.network.flow;
  request.at = kNow;
  request.requested_max_granularity = requested;
  Localizer localizer{topology.engine->topology(), LocalizePolicy{}};
  const FreshnessContext context{};
  return localizer.localize(request, observations, kNow, LossPolicy{}, context);
}

}  // namespace

LO_TEST(localize, flow_scoped_evidence_never_names_a_hop) {
  auto topology = make_topology(3);
  const std::vector<LossObservation> observations{
      observation(topology->network, SubjectRef::flow(topology->network.flow), Granularity::Flow, 50, 1000,
                  500)};
  const LocalizationResult result = localize(*topology, observations);
  LO_CHECK(result.aggregate_class == LossClass::ConfirmedLoss);
  LO_CHECK(result.achieved_granularity == Granularity::Flow);
  LO_CHECK(result.evidence_granularity == Granularity::Flow);
  LO_CHECK(result.ambiguous);
  LO_REQUIRE(result.segments.size() == 1ULL);
  LO_CHECK(result.segments[0].kind == SegmentKind::FlowWide);
  LO_CHECK(result.segments[0].granularity == Granularity::Flow);
  for (const LocalizedSegment& segment : result.segments) {
    LO_CHECK(!is_finer_than(segment.granularity, Granularity::Flow));
  }
  LO_CHECK(testkit::localization_within_evidence_granularity(result, observations));
}

LO_TEST(localize, hop_evidence_produces_hop_scoped_segments) {
  auto topology = make_topology(3);
  const std::vector<LossObservation> observations{
      observation(topology->network, SubjectRef::hop(topology->network.hops[0].id), Granularity::Hop, 0,
                  1000, 0),
      observation(topology->network, SubjectRef::hop(topology->network.hops[1].id), Granularity::Hop, 50,
                  1000, 500)};
  const LocalizationResult result = localize(*topology, observations);
  LO_CHECK(result.achieved_granularity == Granularity::Hop);
  LO_CHECK(result.hops_with_evidence >= 2ULL);
  LO_CHECK_EQ(result.hops_in_path, 3ULL);

  bool saw_loss_segment = false;
  bool saw_no_loss_segment = false;
  bool saw_unobserved = false;
  for (const LocalizedSegment& segment : result.segments) {
    if (segment.kind == SegmentKind::UnobservedSpan) {
      saw_unobserved = true;
      LO_CHECK(segment.klass == LossClass::IncompleteEvidence);
      continue;
    }
    if (segment.klass == LossClass::ConfirmedLoss) {
      saw_loss_segment = true;
      LO_CHECK_EQ(segment.first_hop, 1U);
    }
    if (segment.klass == LossClass::NoLossObserved) {
      saw_no_loss_segment = true;
      LO_CHECK_EQ(segment.first_hop, 0U);
    }
  }
  LO_CHECK(saw_loss_segment);
  LO_CHECK(saw_no_loss_segment);
  LO_CHECK(saw_unobserved);
  LO_CHECK(result.ambiguous);
  LO_CHECK(testkit::localization_within_evidence_granularity(result, observations));
}

LO_TEST(localize, unobserved_hops_are_never_reported_as_clean) {
  auto topology = make_topology(4);
  const std::vector<LossObservation> observations{
      observation(topology->network, SubjectRef::hop(topology->network.hops[0].id), Granularity::Hop, 0,
                  1000, 0)};
  const LocalizationResult result = localize(*topology, observations);
  std::size_t unobserved_hops = 0;
  for (const LocalizedSegment& segment : result.segments) {
    if (segment.kind != SegmentKind::UnobservedSpan) {
      continue;
    }
    LO_CHECK(segment.klass != LossClass::NoLossObserved);
    unobserved_hops += static_cast<std::size_t>(segment.last_hop - segment.first_hop + 1);
  }
  LO_CHECK_EQ(unobserved_hops, 3ULL);
  bool found_reason = false;
  for (const AmbiguityNote& note : result.ambiguity) {
    if (note.reason == ReasonCode::HopEvidenceAbsent) {
      found_reason = true;
    }
  }
  LO_CHECK(found_reason);
}

LO_TEST(localize, an_undeclared_path_downgrades_the_result_to_the_flow) {
  ManualClock clock{kNow};
  EngineConfig config = lofixture::default_config("localize-nopath");
  auto engine = lofixture::make_engine(clock, config);
  const FlowId flow = FlowId::from_canonical_text("flow/nopath");
  const SourceId source = SourceId::from_canonical_text("src/nopath");
  SourceDescriptor descriptor{};
  descriptor.id = source;
  descriptor.name = "s";
  descriptor.kind = SourceKind::CounterTelemetry;
  descriptor.authority = SourceAuthority::Primary;
  (void)engine->sources().register_source(descriptor);
  auto epoch = engine->sources().activate_incarnation(source, "boot", kNow);
  LO_REQUIRE(epoch.ok());

  LossObservation value{};
  value.id = MeasurementId::from_value(11);
  value.evidence_id = MeasurementId::from_value(12);
  value.source = source;
  value.epoch = epoch.value();
  value.subject = SubjectRef::flow(flow);
  value.granularity = Granularity::Hop;
  value.method = MeasurementMethod::CounterDelta;
  value.semantics = LossSemantics::DirectLoss;
  value.validity = ObservationValidity::Valid;
  value.observed_at.value = kNow - Duration::from_seconds(1);
  value.received_at.value = kNow - Duration::from_seconds(1);
  value.lost = 9;

  LocalizationRequest request{};
  request.flow = flow;
  request.at = kNow;
  Localizer localizer{engine->topology(), LocalizePolicy{}};
  const FreshnessContext context{};
  const LocalizationResult result =
      localizer.localize(request, std::span<const LossObservation>{&value, 1}, kNow, LossPolicy{}, context);
  LO_CHECK(!result.path_known);
  LO_CHECK(!is_finer_than(result.achieved_granularity, Granularity::Flow));
  LO_REQUIRE(!result.segments.empty());
  for (const LocalizedSegment& segment : result.segments) {
    LO_CHECK(!is_finer_than(segment.granularity, Granularity::Flow));
  }
  bool found_reason = false;
  for (const ReasonCode reason : result.reasons) {
    if (reason == ReasonCode::PathUnknown) {
      found_reason = true;
    }
  }
  LO_CHECK(found_reason);
}

LO_TEST(localize, a_caller_cap_is_honoured_and_reported) {
  auto topology = make_topology(3);
  const std::vector<LossObservation> observations{
      observation(topology->network, SubjectRef::hop(topology->network.hops[1].id), Granularity::Hop, 50,
                  1000, 500)};
  const LocalizationResult result = localize(*topology, observations, Granularity::Path);
  LO_CHECK(!is_finer_than(result.achieved_granularity, Granularity::Path));
  LO_REQUIRE(!result.segments.empty());
  for (const LocalizedSegment& segment : result.segments) {
    LO_CHECK(!is_finer_than(segment.granularity, Granularity::Path));
  }
  bool found_reason = false;
  for (const ReasonCode reason : result.reasons) {
    if (reason == ReasonCode::LocalizationClampedToEvidenceGranularity) {
      found_reason = true;
    }
  }
  LO_CHECK(found_reason);
}

LO_TEST(localize, no_admissible_evidence_localizes_nothing) {
  auto topology = make_topology(3);
  const std::vector<LossObservation> observations{
      observation(topology->network, SubjectRef::flow(topology->network.flow), Granularity::Flow, 50, 1000,
                  500)};
  LossObservation stale = observations[0];
  stale.observed_at.value = kNow - Duration::from_seconds(600);
  LocalizationRequest request{};
  request.flow = topology->network.flow;
  request.at = kNow;
  Localizer localizer{topology->engine->topology(), LocalizePolicy{}};
  const FreshnessContext context{};
  LossPolicy policy{};
  policy.freshness.max_age = Duration::from_seconds(5);
  const LocalizationResult result =
      localizer.localize(request, std::span<const LossObservation>{&stale, 1}, kNow, policy, context);
  LO_CHECK(result.achieved_granularity == Granularity::Unknown);
  LO_CHECK(result.aggregate_class == LossClass::StaleEvidence);
  LO_CHECK(result.ambiguous);
  LO_CHECK(testkit::localization_within_evidence_granularity(result, std::span<const LossObservation>{}));
}

LO_TEST(localize, segment_evidence_is_assigned_exactly_once) {
  auto topology = make_topology(2);
  const std::vector<LossObservation> observations{
      observation(topology->network, SubjectRef::path(topology->network.path), Granularity::Path, 40, 1000,
                  400)};
  const LocalizationResult result = localize(*topology, observations);
  std::size_t total_observations = 0;
  for (const LocalizedSegment& segment : result.segments) {
    total_observations += segment.observation_count;
  }
  LO_CHECK_EQ(total_observations, 1ULL);
}

LO_TEST(localize, result_text_is_stable) {
  auto topology = make_topology(3);
  const std::vector<LossObservation> observations{
      observation(topology->network, SubjectRef::hop(topology->network.hops[1].id), Granularity::Hop, 50,
                  1000, 500)};
  const LocalizationResult first = localize(*topology, observations);
  const LocalizationResult second = localize(*topology, observations);
  LO_CHECK_EQ(first.to_string(), second.to_string());
  LO_CHECK_EQ(render_localization(first), render_localization(second));
}
