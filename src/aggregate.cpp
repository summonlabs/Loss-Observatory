#include "loss_observatory/aggregate.hpp"

#include <algorithm>
#include <limits>
#include <mutex>
#include <string>

#include "loss_observatory/core/checked.hpp"

namespace loss_observatory {
namespace {

[[nodiscard]] int class_severity(LossClass klass) noexcept {
  switch (klass) {
    case LossClass::Unknown:
      return 0;
    case LossClass::NoLossObserved:
      return 1;
    case LossClass::AbsentEvidence:
      return 2;
    case LossClass::IncompleteEvidence:
      return 3;
    case LossClass::StaleEvidence:
      return 4;
    case LossClass::Discontinuity:
      return 5;
    case LossClass::UnsupportedMethod:
      return 6;
    case LossClass::ImplausibleEvidence:
      return 7;
    case LossClass::ConflictingEvidence:
      return 8;
    case LossClass::SuspectedLoss:
      return 9;
    case LossClass::ConfirmedLoss:
      return 10;
  }
  return 0;
}

}  // namespace

std::string AggregateBucket::to_string() const {
  std::string result("aggregate window=");
  result.append(id.to_string());
  result.append(" start=");
  result.append(start.to_string());
  result.append(" end=");
  result.append(end.to_string());
  result.append(" lost=");
  result.append(std::to_string(lost));
  result.append(" offered=");
  result.append(std::to_string(offered));
  result.append(" ratio_bp=");
  result.append(ratio_defined ? std::to_string(ratio_bp) : std::string("undefined"));
  result.append(" observations=");
  result.append(std::to_string(observation_count));
  result.append(" worst=");
  result.append(loss_observatory::to_string(worst_class));
  result.append(" sources=");
  result.append(std::to_string(sources.size()));
  if (sources_truncated) {
    result.append(" sources_truncated=true");
  }
  if (evidence_ids_truncated) {
    result.append(" evidence_truncated=true");
  }
  if (saturated) {
    result.append(" saturated=true");
  }
  return result;
}

std::int64_t BoundedAggregator::window_index(Timestamp at) const {
  std::int64_t width = limits_.window_width.nanos();
  if (width <= 0) {
    width = 1;
  }
  const std::int64_t nanos = at.unix_nanos();
  if (nanos >= 0) {
    return nanos / width;
  }
  return -(((-nanos) + width - 1) / width);
}

WindowId BoundedAggregator::window_id(std::int64_t index) const {
  const std::uint64_t mixed = mix64(static_cast<std::uint64_t>(index) ^ 0x10A9C3ULL);
  return WindowId::from_value(mixed == 0 ? 1 : mixed);
}

void BoundedAggregator::add(const LossObservation& observation, LossClass klass, Timestamp observed_at) {
  std::lock_guard<std::mutex> guard(mutex_);
  const std::int64_t index = window_index(observed_at);
  auto it = buckets_.find(index);
  if (it == buckets_.end()) {
    std::int64_t width = limits_.window_width.nanos();
    if (width <= 0) {
      width = 1;
    }
    AggregateBucket bucket{};
    bucket.id = window_id(index);
    bucket.start = Timestamp::from_unix_nanos(index * width);
    bucket.end = Timestamp::from_unix_nanos(index * width + width);
    it = buckets_.emplace(index, std::move(bucket)).first;
  }
  AggregateBucket& bucket = it->second;

  const std::uint64_t lost_before = bucket.lost;
  const std::uint64_t offered_before = bucket.offered;
  if (std::numeric_limits<std::uint64_t>::max() - bucket.lost < observation.lost) {
    bucket.lost = std::numeric_limits<std::uint64_t>::max();
    bucket.saturated = true;
    bounds_.push_back(BoundNote{BoundKind::AggregationWindows, std::numeric_limits<std::uint64_t>::max(),
                                lost_before, "aggregate-lost"});
  } else {
    bucket.lost += observation.lost;
  }
  if (std::numeric_limits<std::uint64_t>::max() - bucket.offered < observation.offered) {
    bucket.offered = std::numeric_limits<std::uint64_t>::max();
    bucket.saturated = true;
    bounds_.push_back(BoundNote{BoundKind::AggregationWindows, std::numeric_limits<std::uint64_t>::max(),
                                offered_before, "aggregate-offered"});
  } else {
    bucket.offered += observation.offered;
  }
  bucket.offered_known = bucket.offered_known || observation.offered_known;
  if (bucket.observation_count >= limits_.max_observations_per_window) {
    bucket.saturated = true;
  } else {
    ++bucket.observation_count;
  }
  if (observation.semantics == LossSemantics::DirectLoss) {
    ++bucket.direct_loss_observations;
  }
  if (class_severity(klass) > class_severity(bucket.worst_class)) {
    bucket.worst_class = klass;
  }
  if (std::find(bucket.sources.begin(), bucket.sources.end(), observation.source) ==
      bucket.sources.end()) {
    if (bucket.sources.size() < limits_.max_sources_per_window) {
      bucket.sources.push_back(observation.source);
    } else {
      bucket.sources_truncated = true;
      bounds_.push_back(BoundNote{BoundKind::AggregationSources,
                                  static_cast<std::uint64_t>(limits_.max_sources_per_window),
                                  static_cast<std::uint64_t>(bucket.sources.size() + 1),
                                  bucket.id.to_string()});
    }
  }
  if (bucket.evidence_ids.size() < limits_.max_evidence_ids_per_window) {
    bucket.evidence_ids.push_back(observation.id);
  } else {
    bucket.evidence_ids_truncated = true;
  }
  if (bucket.offered > 0) {
    std::uint32_t ratio = 0;
    if (checked::ratio_basis_points(bucket.lost, bucket.offered, ratio)) {
      bucket.ratio_bp = ratio;
      bucket.ratio_defined = true;
    }
  }

  while (buckets_.size() > limits_.max_windows) {
    buckets_.erase(buckets_.begin());
    ++evicted_;
    bounds_.push_back(BoundNote{BoundKind::AggregationWindows,
                                static_cast<std::uint64_t>(limits_.max_windows),
                                static_cast<std::uint64_t>(buckets_.size() + 1), "aggregation-window"});
  }
}

std::vector<AggregateBucket> BoundedAggregator::buckets() const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<AggregateBucket> result;
  result.reserve(buckets_.size());
  for (const auto& entry : buckets_) {
    result.push_back(entry.second);
  }
  return result;
}

std::vector<AggregateBucket> BoundedAggregator::buckets_for(SourceId source) const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<AggregateBucket> result;
  for (const auto& entry : buckets_) {
    if (std::find(entry.second.sources.begin(), entry.second.sources.end(), source) !=
        entry.second.sources.end()) {
      result.push_back(entry.second);
    }
  }
  return result;
}

std::uint64_t BoundedAggregator::evicted_buckets() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return evicted_;
}

std::vector<BoundNote> BoundedAggregator::bound_notes() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return bounds_;
}

std::size_t BoundedAggregator::window_count() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return buckets_.size();
}

void BoundedAggregator::clear() {
  std::lock_guard<std::mutex> guard(mutex_);
  buckets_.clear();
  bounds_.clear();
}

void BoundedAggregator::set_limits(AggregateLimits limits) {
  std::lock_guard<std::mutex> guard(mutex_);
  limits_ = limits;
}

}  // namespace loss_observatory
