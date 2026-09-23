#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "loss_observatory/classify.hpp"

namespace loss_observatory {

/// Lifecycle of a recorded loss episode.
enum class EpisodeState : std::uint8_t {
  /// Still receiving loss observations in this process epoch.
  Open = 0,
  /// Closed because absence of loss was observed.
  ClosedByAbsence,
  /// Closed because no further observation arrived within the idle window.
  ClosedByIdle,
  /// Closed because the runtime restarted. Historical only.
  ClosedByRestart,
  /// Closed because the operator sealed it.
  ClosedExplicitly,
};

[[nodiscard]] std::string_view to_string(EpisodeState state) noexcept;
[[nodiscard]] bool is_closed(EpisodeState state) noexcept;

struct EpisodeLimits {
  std::size_t max_episodes{4096};
  std::size_t max_evidence_ids_per_episode{64};
  std::size_t max_sources_per_episode{16};
  std::size_t max_query_results{256};
  /// An open episode with no observation for this long is closed as idle.
  Duration idle_timeout = Duration::from_seconds(120);
};

/// A bounded, historical record of a period during which loss was asserted.
///
/// Episodes are history. Nothing in the runtime may extend an episode across a
/// restart, and no episode may be presented as current loss after recovery.
struct LossEpisode {
  EpisodeId id{};
  FlowId flow{};
  PathId path{};
  GenerationId generation{};
  EpisodeState state{EpisodeState::Open};
  LossClass peak_class{LossClass::Unknown};
  Granularity granularity{Granularity::Unknown};
  Timestamp started_at{};
  Timestamp last_observed_at{};
  Timestamp closed_at{};
  std::uint64_t lost_total{0};
  std::uint64_t offered_total{0};
  bool ratio_defined{false};
  std::uint32_t peak_ratio_bp{0};
  std::uint32_t last_ratio_bp{0};
  bool totals_saturated{false};
  std::size_t observation_count{0};
  std::vector<SourceId> sources{};
  std::vector<MeasurementId> evidence_ids{};
  bool evidence_ids_truncated{false};
  std::string state_detail{};

  [[nodiscard]] bool open() const noexcept { return state == EpisodeState::Open; }
  [[nodiscard]] Duration duration() const noexcept { return closed_at - started_at; }
  [[nodiscard]] std::string to_string() const;
};

struct EpisodeQuery {
  std::optional<FlowId> flow{};
  std::optional<PathId> path{};
  Timestamp from{};
  Timestamp to{};
  bool include_open{true};
  bool include_closed{true};
  std::size_t max_results{256};
};

struct EpisodeQueryResult {
  std::vector<LossEpisode> episodes{};
  bool truncated{false};
  std::uint64_t total_matching{0};
  std::uint64_t evicted_episodes{0};
  std::vector<BoundNote> bounds{};
  std::string summary{};

  [[nodiscard]] std::string to_string() const;
};

/// Tracks episodes across classifications. Thread-safe; the engine may feed it
/// from worker threads.
class EpisodeTracker {
 public:
  explicit EpisodeTracker(EpisodeLimits limits = {}) : limits_(limits) {}

  /// Feeds one classification. p generation and p path identify the binding
  /// the classification belongs to. Returns the affected episode id.
  EpisodeId observe(const Classification& classification, FlowId flow, PathId path, GenerationId generation,
                    Timestamp now);

  /// Closes episodes whose last observation is older than the idle timeout.
  std::size_t close_idle(Timestamp now);

  /// Seals every open episode. Used at restart and at shutdown so no episode is
  /// ever left dangling and none can be resumed by a later process.
  std::size_t seal_all(EpisodeState state, Timestamp at, std::string detail);

  [[nodiscard]] Result<LossEpisode> find(EpisodeId id) const;
  [[nodiscard]] std::vector<LossEpisode> episodes() const;
  [[nodiscard]] std::vector<LossEpisode> open_episodes() const;
  [[nodiscard]] EpisodeQueryResult query(const EpisodeQuery& request) const;
  [[nodiscard]] std::size_t episode_count() const;
  [[nodiscard]] std::uint64_t evicted_total() const;

  /// Restores a closed episode from persisted history. Open episodes are
  /// restored as ClosedByRestart; they can never come back as open.
  void restore(LossEpisode episode);

  [[nodiscard]] const EpisodeLimits& limits() const noexcept { return limits_; }
  void set_limits(EpisodeLimits limits) noexcept { limits_ = limits; }

 private:
  void evict_locked();

  mutable std::mutex mutex_{};
  EpisodeLimits limits_{};
  std::map<EpisodeId, LossEpisode> episodes_{};
  std::map<std::pair<FlowId, GenerationId>, EpisodeId> open_by_flow_{};
  std::uint64_t evicted_{0};
};

/// Deterministic episode identity: the same flow, generation, and start instant
/// always produce the same id, so a replayed history does not duplicate.
[[nodiscard]] EpisodeId make_episode_id(FlowId flow, GenerationId generation, Timestamp start);

}  // namespace loss_observatory
