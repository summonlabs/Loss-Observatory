#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "loss_observatory/core/time.hpp"
#include "loss_observatory/evidence/derive.hpp"
#include "loss_observatory/model/generation.hpp"
#include "loss_observatory/model/source.hpp"
#include "loss_observatory/model/topology.hpp"

namespace loss_observatory {

/// Freshness is a property of the evidence at the moment it is evaluated, not a
/// property stored with the evidence. The same record can be fresh now and
/// stale later, and a record recovered from disk is never fresh until it is
/// re-observed by a live source in the current process epoch.
enum class Freshness : std::uint8_t {
  Fresh = 0,
  Stale,
  /// Recovered from persistence during this boot. Explicitly not current.
  NotCurrent,
  /// Observation time is in the future beyond the skew tolerance.
  FutureDated,
  /// No usable observation instant.
  Undated,
  /// The assessment could not reach a conclusion (for example the source is
  /// unknown to the registry). Never treated as fresh.
  Unknown,
};

[[nodiscard]] std::string_view to_string(Freshness state) noexcept;

enum class StaleReason : std::uint8_t {
  None = 0,
  AgeExceeded,
  EpochRetired,
  EpochUnknown,
  GenerationSuperseded,
  GenerationUnknown,
  TopologyRevisionChanged,
  RecoveredFromPersistence,
  SourceUnknown,
  SourceIncarnationUnknown,
  FutureDated,
  Undated,
  ReceivedBeforeObserved,
};

[[nodiscard]] std::string_view to_string(StaleReason reason) noexcept;

struct FreshnessAssessment {
  Freshness state{Freshness::Unknown};
  StaleReason reason{StaleReason::None};
  Duration age{};
  bool admissible{false};
  std::string detail{};

  [[nodiscard]] std::string to_string() const;
};

struct FreshnessPolicy {
  /// Evidence older than this cannot prove current loss.
  Duration max_age = Duration::from_seconds(30);
  /// Tolerance for observation instants slightly ahead of local time.
  Duration future_tolerance = Duration::from_seconds(5);
  /// Tolerance applied when comparing times from different sources.
  Duration clock_skew_tolerance = Duration::from_seconds(2);
  /// When false (the default), future-dated evidence is never admissible.
  bool allow_future_dated{false};
  /// When true, source epoch retirement is ignored for evidence that is
  /// otherwise age-fresh. Default false: a retired incarnation's evidence is
  /// stale by construction.
  bool ignore_retired_epoch{false};
  /// When true, a superseded generation is still usable for current reasoning.
  /// Default false.
  bool ignore_generation_supersession{false};
  /// When true, evidence recorded against an older topology revision is still
  /// admissible. Default false.
  bool ignore_topology_revision{false};
};

struct FreshnessContext {
  const SourceRegistry* sources{nullptr};
  const GenerationRegistry* generations{nullptr};
  const TopologyRegistry* topology{nullptr};
};

/// Deterministic freshness decision. p now is supplied by the caller (never
/// read from a hidden global clock), so the same inputs always give the same
/// answer.
[[nodiscard]] FreshnessAssessment assess_freshness(const LossObservation& observation, Timestamp now,
                                                   const FreshnessPolicy& policy,
                                                   const FreshnessContext& context);

struct FreshnessSummary {
  std::size_t total{0};
  std::size_t fresh{0};
  std::size_t stale{0};
  std::size_t not_current{0};
  std::size_t future_dated{0};
  std::size_t undated{0};
  std::size_t unknown{0};

  [[nodiscard]] bool has_admissible() const noexcept { return fresh > 0; }
  [[nodiscard]] std::string to_string() const;
};

[[nodiscard]] FreshnessSummary summarize(std::span<const FreshnessAssessment> assessments);

}  // namespace loss_observatory
