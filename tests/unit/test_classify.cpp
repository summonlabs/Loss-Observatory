#include <algorithm>
#include <string>
#include <vector>

#include "fixtures.hpp"
#include "loss_observatory/classify.hpp"
#include "loss_observatory/testkit/testkit.hpp"

using namespace loss_observatory;

namespace {

const Timestamp kNow = Timestamp::from_unix_seconds(2000);

LossObservation observation(SourceId source, LossSemantics semantics, ObservationValidity validity,
                            std::uint64_t lost, std::uint64_t offered, std::uint32_t ratio_bp,
                            bool ratio_defined, Timestamp at, Granularity granularity = Granularity::Flow,
                            MeasurementMethod method = MeasurementMethod::SequenceGap,
                            DiscontinuityKind discontinuity = DiscontinuityKind::None) {
  LossObservation value{};
  value.id = MeasurementId::from_value(combine_hash(source.value(), lost + offered + ratio_bp));
  value.evidence_id = MeasurementId::from_value(combine_hash(value.id.value(), 7));
  value.source = source;
  value.epoch = EpochId::from_canonical_text("epoch/1");
  value.generation = GenerationId::from_canonical_text("gen/1");
  value.subject = SubjectRef::flow(FlowId::from_canonical_text("flow/1"));
  value.granularity = granularity;
  value.method = method;
  value.semantics = semantics;
  value.validity = validity;
  value.discontinuity = discontinuity;
  value.observed_at.value = at;
  value.received_at.value = at;
  value.lost = lost;
  value.offered = offered;
  value.offered_known = offered > 0;
  value.ratio_defined = ratio_defined;
  value.ratio_bp = ratio_bp;
  return value;
}

ClassificationInput input_with(std::vector<LossObservation> observations) {
  ClassificationInput input{};
  input.subject = SubjectRef::flow(FlowId::from_canonical_text("flow/1"));
  input.observations = std::move(observations);
  input.evaluated_at = kNow;
  return input;
}

Classification classify(std::vector<LossObservation> observations, LossPolicy policy = {}) {
  FreshnessContext context{};
  const Classifier classifier(context, policy);
  return classifier.classify(input_with(std::move(observations)));
}

const SourceId kSourceA = SourceId::from_canonical_text("src-a");
const SourceId kSourceB = SourceId::from_canonical_text("src-b");

}  // namespace

LO_TEST(classification, absent_evidence_is_not_loss) {
  const Classification result = classify({});
  LO_CHECK(result.klass == LossClass::AbsentEvidence);
  LO_CHECK(!asserts_loss(result.klass));
  LO_CHECK(!asserts_absence(result.klass));
  LO_CHECK_EQ(result.evidence_considered, 0ULL);
  LO_CHECK_EQ(result.freshness.fresh, 0ULL);
  LO_CHECK(std::find(result.reasons.begin(), result.reasons.end(), ReasonCode::NoEvidenceSubmitted) !=
           result.reasons.end());
  LO_CHECK(testkit::reasons_are_renderable(result.reasons));
}

LO_TEST(classification, stale_evidence_is_never_current_loss) {
  LossPolicy policy{};
  policy.freshness.max_age = Duration::from_seconds(5);
  const Classification result = classify(
      {observation(kSourceA, LossSemantics::RatioLoss, ObservationValidity::Valid, 500, 1000, 5000, true,
                   kNow - Duration::from_seconds(600))},
      policy);
  LO_CHECK(result.klass == LossClass::StaleEvidence);
  LO_CHECK(!asserts_loss(result.klass));
  LO_CHECK_EQ(result.freshness.fresh, 0ULL);
  LO_CHECK_EQ(result.freshness.stale, 1ULL);
  LO_CHECK_EQ(result.lost_total, 0ULL);
  LO_CHECK(testkit::stale_evidence_cannot_prove_current_loss(result));
}

LO_TEST(classification, recovered_evidence_alone_is_stale_evidence) {
  LossObservation recovered = observation(kSourceA, LossSemantics::RatioLoss, ObservationValidity::Valid,
                                          500, 1000, 5000, true, kNow - Duration::from_seconds(1));
  recovered.recovered_from_persistence = true;
  const Classification result = classify({recovered});
  LO_CHECK(result.klass == LossClass::StaleEvidence);
  LO_CHECK(!asserts_loss(result.klass));
  LO_CHECK_EQ(result.freshness.not_current, 1ULL);
}

LO_TEST(classification, zero_ratio_with_coverage_is_no_loss_observed) {
  const Classification result = classify({observation(kSourceA, LossSemantics::RatioLoss,
                                                      ObservationValidity::Valid, 0, 1000, 0, true,
                                                      kNow - Duration::from_seconds(1))});
  LO_CHECK(result.klass == LossClass::NoLossObserved);
  LO_CHECK(asserts_absence(result.klass));
  LO_CHECK_EQ(result.offered_total, 1000ULL);
  LO_CHECK(result.ratio_defined);
  LO_CHECK_EQ(result.ratio_bp, 0U);
}

LO_TEST(classification, ratio_above_threshold_confirms_loss) {
  const Classification result = classify({observation(kSourceA, LossSemantics::RatioLoss,
                                                      ObservationValidity::Valid, 20, 1000, 200, true,
                                                      kNow - Duration::from_seconds(1))});
  LO_CHECK(result.klass == LossClass::ConfirmedLoss);
  LO_CHECK(asserts_loss(result.klass));
  LO_CHECK_EQ(result.lost_total, 20ULL);
  LO_CHECK(result.attribution.score > 0);
  LO_CHECK(result.attribution.band != ConfidenceBand::None);
}

LO_TEST(classification, direct_drop_counter_confirms_loss_and_static_counter_does_not) {
  const Classification advanced =
      classify({observation(kSourceA, LossSemantics::DirectLoss, ObservationValidity::Valid, 7, 0, 0, false,
                            kNow - Duration::from_seconds(1), Granularity::Hop,
                            MeasurementMethod::CounterDelta)});
  LO_CHECK(advanced.klass == LossClass::ConfirmedLoss);
  LO_CHECK_EQ(advanced.lost_total, 7ULL);
  LO_CHECK(!advanced.ratio_defined);

  const Classification static_counter =
      classify({observation(kSourceA, LossSemantics::DirectLoss, ObservationValidity::Valid, 0, 0, 0, false,
                            kNow - Duration::from_seconds(1), Granularity::Hop,
                            MeasurementMethod::CounterDelta)});
  LO_CHECK(static_counter.klass == LossClass::NoLossObserved);
}

LO_TEST(classification, low_coverage_positive_ratio_is_only_suspected) {
  LossPolicy policy{};
  policy.min_offered_for_confirmation = 1000;
  const Classification result = classify({observation(kSourceA, LossSemantics::RatioLoss,
                                                      ObservationValidity::Valid, 1, 10, 1000, true,
                                                      kNow - Duration::from_seconds(1))},
                                         policy);
  LO_CHECK(result.klass == LossClass::SuspectedLoss);
  LO_CHECK(asserts_loss(result.klass));
  LO_CHECK(std::find(result.reasons.begin(), result.reasons.end(),
                     ReasonCode::CoverageBelowConfirmationThreshold) != result.reasons.end());
}

LO_TEST(classification, discontinuity_only_never_becomes_loss) {
  const Classification result =
      classify({observation(kSourceA, LossSemantics::DirectLoss, ObservationValidity::Discontinuity, 0, 0, 0,
                            false, kNow - Duration::from_seconds(1), Granularity::Hop,
                            MeasurementMethod::CounterDelta, DiscontinuityKind::CounterReset)});
  LO_CHECK(result.klass == LossClass::Discontinuity);
  LO_CHECK(!asserts_loss(result.klass));
  LO_CHECK_EQ(result.lost_total, 0ULL);
  LO_CHECK(std::find(result.reasons.begin(), result.reasons.end(), ReasonCode::CounterResetObserved) !=
           result.reasons.end());
}

LO_TEST(classification, a_discontinuity_does_not_erase_real_loss_but_is_reported) {
  const Classification result = classify(
      {observation(kSourceA, LossSemantics::DirectLoss, ObservationValidity::Valid, 5, 0, 0, false,
                   kNow - Duration::from_seconds(2), Granularity::Hop, MeasurementMethod::CounterDelta),
       observation(kSourceA, LossSemantics::DirectLoss, ObservationValidity::Discontinuity, 0, 0, 0, false,
                   kNow - Duration::from_seconds(1), Granularity::Hop, MeasurementMethod::CounterDelta,
                   DiscontinuityKind::CounterWrap)});
  LO_CHECK(result.klass == LossClass::ConfirmedLoss);
  LO_CHECK(std::find(result.reasons.begin(), result.reasons.end(), ReasonCode::DiscontinuityObserved) !=
           result.reasons.end());
  LO_CHECK(std::find(result.reasons.begin(), result.reasons.end(), ReasonCode::CounterWrapObserved) !=
           result.reasons.end());
}

LO_TEST(classification, unsupported_and_implausible_evidence_stay_distinct) {
  const Classification unsupported =
      classify({observation(kSourceA, LossSemantics::RatioLoss, ObservationValidity::UnsupportedMethod, 0, 0,
                            0, false, kNow - Duration::from_seconds(1), Granularity::Link,
                            MeasurementMethod::ProbeOneWay)});
  LO_CHECK(unsupported.klass == LossClass::UnsupportedMethod);

  const Classification implausible =
      classify({observation(kSourceA, LossSemantics::RatioLoss, ObservationValidity::Implausible, 0, 0, 0,
                            false, kNow - Duration::from_seconds(1), Granularity::Flow,
                            MeasurementMethod::SequenceGap)});
  LO_CHECK(implausible.klass == LossClass::ImplausibleEvidence);
  LO_CHECK(!asserts_loss(implausible.klass));
}

LO_TEST(classification, throughput_evidence_alone_is_incomplete) {
  const Classification result =
      classify({observation(kSourceA, LossSemantics::NonLoss, ObservationValidity::Valid, 0, 5000, 0, false,
                            kNow - Duration::from_seconds(1))});
  LO_CHECK(result.klass == LossClass::IncompleteEvidence);
  LO_CHECK(!asserts_loss(result.klass));
  LO_CHECK(!asserts_absence(result.klass));
}

LO_TEST(classification, conflicting_sources_yield_conflicting_evidence) {
  const Classification result = classify(
      {observation(kSourceA, LossSemantics::DirectLoss, ObservationValidity::Valid, 50, 0, 0, false,
                   kNow - Duration::from_seconds(1), Granularity::Hop, MeasurementMethod::CounterDelta),
       observation(kSourceB, LossSemantics::RatioLoss, ObservationValidity::Valid, 0, 1000, 0, true,
                   kNow - Duration::from_seconds(1))});
  LO_CHECK(result.klass == LossClass::ConflictingEvidence);
  LO_CHECK(!asserts_loss(result.klass));
  LO_CHECK_EQ(result.claims.size(), 2ULL);
}

LO_TEST(classification, attribution_terms_are_deterministic_and_explainable) {
  const std::vector<LossObservation> observations = {
      observation(kSourceA, LossSemantics::RatioLoss, ObservationValidity::Valid, 30, 3000, 100, true,
                  kNow - Duration::from_seconds(1))};
  const Classification first = classify(observations);
  const Classification second = classify(observations);
  LO_CHECK(first.klass == second.klass);
  LO_CHECK_EQ(first.attribution.score, second.attribution.score);
  LO_CHECK(first.attribution.band == second.attribution.band);
  LO_CHECK_EQ(first.summary, second.summary);
  std::int32_t sum = 0;
  for (const ConfidenceTerm& term : first.attribution.terms) {
    sum += term.points;
  }
  LO_CHECK(sum >= 0);
  LO_CHECK(testkit::reasons_are_renderable(first.reasons));
}

LO_TEST(classification, evidence_identifier_list_is_bounded_and_flagged) {
  LossPolicy policy{};
  policy.max_evidence_ids = 3;
  std::vector<LossObservation> observations;
  for (std::uint64_t i = 0; i < 10; ++i) {
    LossObservation value = observation(kSourceA, LossSemantics::RatioLoss, ObservationValidity::Valid, 1,
                                        1000, 10, true, kNow - Duration::from_seconds(1));
    value.id = MeasurementId::from_value(5000 + i);
    value.evidence_id = MeasurementId::from_value(9000 + i);
    observations.push_back(value);
  }
  const Classification result = classify(std::move(observations), policy);
  LO_CHECK_EQ(result.evidence_ids.size(), 3ULL);
  LO_CHECK(result.evidence_ids_truncated);
}

LO_TEST(classification, summaries_are_stable_across_runs) {
  const Classification a = classify({observation(kSourceA, LossSemantics::RatioLoss,
                                                 ObservationValidity::Valid, 5, 1000, 50, true,
                                                 kNow - Duration::from_seconds(1))});
  const Classification b = classify({observation(kSourceA, LossSemantics::RatioLoss,
                                                 ObservationValidity::Valid, 5, 1000, 50, true,
                                                 kNow - Duration::from_seconds(1))});
  LO_CHECK_EQ(a.to_string(), b.to_string());
}
