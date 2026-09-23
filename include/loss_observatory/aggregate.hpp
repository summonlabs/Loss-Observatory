#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "loss_observatory/classify.hpp"

namespace loss_observatory {

struct AggregateLimits {
  /// Maximum number of time buckets retained. Older buckets are evicted and
  /// counted; eviction is always reported.
  std::size_t max_windows{256};
  std::size_t max_sources_per_window{64};
  std::size_t max_evidence_ids_per_window{32};
  Duration window_width = Duration::from_seconds(1);
  /// Absolute bound on retained buckets across a single observation series.
  std::size_t max_observations_per_window{1000000};
};

struct AggregateBucket {
  WindowId id{};
  Timestamp start{};
  Timestamp end{};
  std::uint64_t lost{0};
  std::uint64_t offered{0};
  bool offered_known{false};
  bool ratio_defined{false};
  std::uint32_t ratio_bp{0};
  std::uint64_t observation_count{0};
  std::uint64_t direct_loss_observations{0};
  bool saturated{false};
  LossClass worst_class{LossClass::Unknown};
  std::vector<SourceId> sources{};
  bool sources_truncated{false};
  std::vector<MeasurementId> evidence_ids{};
  bool evidence_ids_truncated{false};

  [[nodiscard]] std::string to_string() const;
};

/// Fixed-width, bounded time-bucket aggregator.
///
/// Sums use checked arithmetic with explicit saturation reporting: an overflow
/// in a long run of loss must never silently wrap into a small number.
class BoundedAggregator {
 public:
  explicit BoundedAggregator(AggregateLimits limits = {}) : limits_(limits) {}

  void add(const LossObservation& observation, LossClass klass, Timestamp observed_at);

  [[nodiscard]] std::vector<AggregateBucket> buckets() const;
  [[nodiscard]] std::vector<AggregateBucket> buckets_for(SourceId source) const;
  [[nodiscard]] std::uint64_t evicted_buckets() const;
  [[nodiscard]] std::vector<BoundNote> bound_notes() const;
  [[nodiscard]] std::size_t window_count() const;

  void clear();
  void set_limits(AggregateLimits limits);

 private:
  [[nodiscard]] std::int64_t window_index(Timestamp at) const;
  [[nodiscard]] WindowId window_id(std::int64_t index) const;

  mutable std::mutex mutex_{};
  AggregateLimits limits_{};
  std::map<std::int64_t, AggregateBucket> buckets_{};
  std::uint64_t evicted_{0};
  std::vector<BoundNote> bounds_{};
};

}  // namespace loss_observatory
