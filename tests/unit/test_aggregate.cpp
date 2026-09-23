#include <string>
#include <vector>

#include "fixtures.hpp"
#include "loss_observatory/aggregate.hpp"

using namespace loss_observatory;

namespace {

const Timestamp kStart = Timestamp::from_unix_seconds(5000);

LossObservation observation(std::uint64_t id, SourceId source, std::uint64_t lost, std::uint64_t offered,
                            Timestamp at, LossSemantics semantics = LossSemantics::RatioLoss) {
  LossObservation value{};
  value.id = MeasurementId::from_value(id);
  value.evidence_id = MeasurementId::from_value(id + 100000);
  value.source = source;
  value.subject = SubjectRef::flow(FlowId::from_canonical_text("flow/agg"));
  value.granularity = Granularity::Flow;
  value.method = MeasurementMethod::SequenceGap;
  value.semantics = semantics;
  value.validity = ObservationValidity::Valid;
  value.observed_at.value = at;
  value.received_at.value = at;
  value.lost = lost;
  value.offered = offered;
  value.offered_known = offered > 0;
  value.ratio_defined = offered > 0;
  return value;
}

}  // namespace

LO_TEST(aggregate, observations_land_in_fixed_width_windows) {
  AggregateLimits limits{};
  limits.window_width = Duration::from_seconds(1);
  BoundedAggregator aggregator{limits};
  const SourceId source = SourceId::from_canonical_text("src");
  aggregator.add(observation(1, source, 10, 1000, kStart), LossClass::ConfirmedLoss, kStart);
  aggregator.add(observation(2, source, 5, 500, kStart + Duration::from_millis(200)),
                 LossClass::ConfirmedLoss, kStart + Duration::from_millis(200));
  aggregator.add(observation(3, source, 1, 100, kStart + Duration::from_millis(1500)),
                 LossClass::SuspectedLoss, kStart + Duration::from_millis(1500));

  const std::vector<AggregateBucket> buckets = aggregator.buckets();
  LO_REQUIRE(buckets.size() == 2ULL);
  LO_CHECK_EQ(buckets[0].lost, 15ULL);
  LO_CHECK_EQ(buckets[0].offered, 1500ULL);
  LO_CHECK_EQ(buckets[0].observation_count, 2ULL);
  LO_CHECK(buckets[0].worst_class == LossClass::ConfirmedLoss);
  LO_CHECK_EQ(buckets[1].lost, 1ULL);
  LO_CHECK(buckets[1].worst_class == LossClass::SuspectedLoss);
  LO_CHECK(buckets[0].start < buckets[1].start);
  LO_CHECK_EQ((buckets[0].end - buckets[0].start).nanos(), Duration::from_seconds(1).nanos());
}

LO_TEST(aggregate, ratio_is_computed_from_the_bucket_totals) {
  AggregateLimits limits{};
  BoundedAggregator aggregator{limits};
  const SourceId source = SourceId::from_canonical_text("src");
  aggregator.add(observation(1, source, 10, 1000, kStart), LossClass::ConfirmedLoss, kStart);
  aggregator.add(observation(2, source, 15, 1000, kStart), LossClass::ConfirmedLoss, kStart);
  const std::vector<AggregateBucket> buckets = aggregator.buckets();
  LO_REQUIRE(buckets.size() == 1ULL);
  LO_CHECK(buckets[0].ratio_defined);
  LO_CHECK_EQ(buckets[0].ratio_bp, 125U);
}

LO_TEST(aggregate, window_bound_is_reported) {
  AggregateLimits limits{};
  limits.max_windows = 2;
  limits.window_width = Duration::from_seconds(1);
  BoundedAggregator aggregator{limits};
  const SourceId source = SourceId::from_canonical_text("src");
  for (std::uint64_t i = 0; i < 5; ++i) {
    const Timestamp at = kStart + Duration::from_seconds(static_cast<std::int64_t>(i));
    aggregator.add(observation(i + 1, source, 1, 10, at), LossClass::ConfirmedLoss, at);
  }
  LO_CHECK_EQ(aggregator.window_count(), 2ULL);
  LO_CHECK_EQ(aggregator.evicted_buckets(), 3ULL);
  const std::vector<BoundNote> notes = aggregator.bound_notes();
  LO_CHECK(!notes.empty());
  bool found = false;
  for (const BoundNote& note : notes) {
    if (note.kind == BoundKind::AggregationWindows) {
      found = true;
    }
  }
  LO_CHECK(found);
}

LO_TEST(aggregate, source_bound_is_reported_per_window) {
  AggregateLimits limits{};
  limits.max_sources_per_window = 2;
  BoundedAggregator aggregator{limits};
  for (std::uint64_t i = 0; i < 5; ++i) {
    const SourceId source = SourceId::from_canonical_text("src/" + std::to_string(i));
    aggregator.add(observation(i + 1, source, 1, 10, kStart), LossClass::ConfirmedLoss, kStart);
  }
  const std::vector<AggregateBucket> buckets = aggregator.buckets();
  LO_REQUIRE(buckets.size() == 1ULL);
  LO_CHECK_EQ(buckets[0].sources.size(), 2ULL);
  LO_CHECK(buckets[0].sources_truncated);
  bool found = false;
  for (const BoundNote& note : aggregator.bound_notes()) {
    if (note.kind == BoundKind::AggregationSources) {
      found = true;
    }
  }
  LO_CHECK(found);
}

LO_TEST(aggregate, saturation_is_recorded_rather_than_wrapping) {
  AggregateLimits limits{};
  BoundedAggregator aggregator{limits};
  const SourceId source = SourceId::from_canonical_text("src");
  const std::uint64_t huge = std::numeric_limits<std::uint64_t>::max();
  aggregator.add(observation(1, source, huge, 0, kStart), LossClass::ConfirmedLoss, kStart);
  aggregator.add(observation(2, source, 10, 0, kStart), LossClass::ConfirmedLoss, kStart);
  const std::vector<AggregateBucket> buckets = aggregator.buckets();
  LO_REQUIRE(buckets.size() == 1ULL);
  LO_CHECK(buckets[0].saturated);
  LO_CHECK_EQ(buckets[0].lost, huge);
  LO_CHECK(!aggregator.bound_notes().empty());
}

LO_TEST(aggregate, evidence_identifier_list_is_bounded_and_flagged) {
  AggregateLimits limits{};
  limits.max_evidence_ids_per_window = 2;
  BoundedAggregator aggregator{limits};
  const SourceId source = SourceId::from_canonical_text("src");
  for (std::uint64_t i = 0; i < 5; ++i) {
    aggregator.add(observation(i + 1, source, 1, 10, kStart), LossClass::ConfirmedLoss, kStart);
  }
  const std::vector<AggregateBucket> buckets = aggregator.buckets();
  LO_REQUIRE(buckets.size() == 1ULL);
  LO_CHECK_EQ(buckets[0].evidence_ids.size(), 2ULL);
  LO_CHECK(buckets[0].evidence_ids_truncated);
}

LO_TEST(aggregate, empty_aggregator_reports_nothing) {
  BoundedAggregator aggregator{AggregateLimits{}};
  LO_CHECK_EQ(aggregator.buckets().size(), 0ULL);
  LO_CHECK_EQ(aggregator.window_count(), 0ULL);
  LO_CHECK(aggregator.bound_notes().empty());
  aggregator.clear();
  LO_CHECK_EQ(aggregator.window_count(), 0ULL);
}

LO_TEST(aggregate, pre_epoch_instants_bucket_correctly) {
  AggregateLimits limits{};
  limits.window_width = Duration::from_seconds(10);
  BoundedAggregator aggregator{limits};
  const SourceId source = SourceId::from_canonical_text("src");
  const Timestamp before = Timestamp::from_unix_seconds(-25);
  aggregator.add(observation(1, source, 1, 10, before), LossClass::ConfirmedLoss, before);
  const std::vector<AggregateBucket> buckets = aggregator.buckets();
  LO_REQUIRE(buckets.size() == 1ULL);
  LO_CHECK(buckets[0].start <= before);
  LO_CHECK(buckets[0].end > before);
}
