#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "loss_observatory/core/bounded.hpp"
#include "loss_observatory/evidence/derive.hpp"
#include "loss_observatory/freshness.hpp"

namespace loss_observatory {

/// The complete, closed set of conclusions this runtime is allowed to reach
/// about a subject. Every one of them is reachable from real inputs and every
/// one of them is distinguishable in output.
enum class LossClass : std::uint8_t {
  /// The inputs do not determine an answer.
  Unknown = 0,
  /// No evidence was supplied for this subject at all. This is emphatically
  /// not loss, and not an absence of loss either.
  AbsentEvidence,
  /// Fresh, admissible evidence of adequate coverage shows zero loss.
  NoLossObserved,
  /// Fresh, admissible evidence directly shows loss and the method is capable
  /// of supporting the claim.
  ConfirmedLoss,
  /// Something loss-shaped is present, but coverage or magnitude is not enough
  /// to confirm it.
  SuspectedLoss,
  /// The evidence shows a counter or sequence discontinuity. Explicitly not
  /// loss: a reset, wrap, or restart is not a drop.
  Discontinuity,
  /// Every piece of evidence for this subject is stale, recovered, or
  /// superseded. Not loss, and not an absence of loss.
  StaleEvidence,
  /// Admissible sources disagree beyond tolerance and nothing outranks them.
  ConflictingEvidence,
  /// The evidence that exists cannot cover the subject: partial hop coverage,
  /// missing baseline, unpaired endpoint report.
  IncompleteEvidence,
  /// The measurement method is declared but not implemented, or its
  /// preconditions are unmet.
  UnsupportedMethod,
  /// The evidence is internally impossible (received more than sent, and so
  /// on). Reported as evidence fault, never as loss.
  ImplausibleEvidence,
};

[[nodiscard]] std::string_view to_string(LossClass klass) noexcept;
[[nodiscard]] Result<LossClass> parse_loss_class(std::string_view text);

/// True for classes that assert loss happened.
[[nodiscard]] constexpr bool asserts_loss(LossClass klass) noexcept {
  return klass == LossClass::ConfirmedLoss || klass == LossClass::SuspectedLoss;
}

/// True for classes that are safe to read as "no loss here".
[[nodiscard]] constexpr bool asserts_absence(LossClass klass) noexcept {
  return klass == LossClass::NoLossObserved;
}

/// Every distinct reason this runtime can cite. Reason codes are part of the
/// explanation contract: they are stable, ordered, and exported verbatim.
enum class ReasonCode : std::uint16_t {
  None = 0,

  // Evidence availability
  NoEvidenceSubmitted,
  EvidenceRejectedBeforeStorage,
  EvidenceStoreEvictedOlderItems,
  EvidencePresent,

  // Freshness
  EvidenceFresh,
  AllEvidenceStale,
  EvidenceAgeExceeded,
  EvidenceRecoveredFromPersistence,
  EvidenceEpochRetired,
  EvidenceEpochUnknown,
  EvidenceGenerationSuperseded,
  EvidenceGenerationUnknown,
  EvidenceTopologyRevisionChanged,
  EvidenceSourceUnknown,
  EvidenceFutureDated,
  EvidenceUndated,
  EvidenceReceivedBeforeObserved,

  // Structure
  DiscontinuityObserved,
  CounterWrapObserved,
  CounterResetObserved,
  CounterWrapUnresolved,
  ImplausibleDeltaObserved,
  SequenceRestartObserved,
  EpochChangeObserved,
  GenerationChangeObserved,
  OutOfOrderEvidence,
  MissingCounterBaseline,
  ImplausibleEvidenceObserved,
  UndefinedRatio,

  // Coverage
  CoverageBelowConfirmationThreshold,
  CoverageBelowAbsenceThreshold,
  PartialHopCoverage,
  UnobservedPathSpan,
  EndpointReportsUnpaired,
  EndpointPairingWindowExceeded,

  // Classification outcome
  LossRatioAtOrAboveConfirmedThreshold,
  DirectLossCounterAdvanced,
  LossRatioBelowConfirmationThreshold,
  ZeroLossWithAdequateCoverage,
  DirectLossCounterStatic,

  // Conflict
  SourceDisagreementOnClass,
  SourceDisagreementOnRatio,
  ConflictResolvedByAuthority,
  ConflictUnresolved,
  ConflictToleranceApplied,
  LowerAuthorityClaimRetained,

  // Method
  MethodNotImplemented,
  MethodRequiresSynchronizedClocks,
  MethodRequiresStableGeneration,
  GranularityExceedsMethodCapability,
  SyntheticEvidenceUsed,

  // Bounds
  ResultSetTruncated,
  EvidenceListTruncated,
  AggregationWindowBoundReached,
  AggregationSaturated,
  HistoryBoundReached,
  PersistenceBoundReached,

  // Localization
  LocalizedToFinestSupportedGranularity,
  LocalizationClampedToEvidenceGranularity,
  AmbiguityRetained,
  PathUnknown,
  HopEvidenceAbsent,
};

[[nodiscard]] std::string_view to_string(ReasonCode code) noexcept;

enum class ConfidenceBand : std::uint8_t {
  None = 0,
  VeryLow = 1,
  Low = 2,
  Moderate = 3,
  High = 4,
  VeryHigh = 5,
};

[[nodiscard]] std::string_view to_string(ConfidenceBand band) noexcept;
[[nodiscard]] ConfidenceBand band_for_score(std::int32_t score) noexcept;

/// One named contribution to a confidence score. Scores are integers so the
/// result is identical on every platform and every run.
struct ConfidenceTerm {
  ReasonCode code{ReasonCode::None};
  std::int32_t points{0};
  std::string detail{};
};

struct Attribution {
  ConfidenceBand band{ConfidenceBand::None};
  std::int32_t score{0};
  std::vector<ConfidenceTerm> terms{};
  std::string rationale{};
};

struct LossPolicy {
  FreshnessPolicy freshness{};

  /// Ratio at or above which fresh, adequately covered evidence confirms loss.
  std::uint32_t confirmed_ratio_bp{100};
  /// Ratio above zero below which loss is only suspected.
  std::uint32_t suspected_ratio_bp{1};
  /// Minimum offered count required before a ratio may confirm loss.
  std::uint64_t min_offered_for_confirmation{100};
  /// Minimum offered count required before a zero ratio may assert absence.
  std::uint64_t min_offered_for_absence{100};
  /// Ratio spread between same-authority sources above which they conflict.
  std::uint32_t conflict_tolerance_bp{50};
  /// Whether direct-loss counters confirm on any positive delta.
  bool direct_loss_counter_confirms{true};
  /// Hard caps on explanation payload size.
  std::size_t max_reasons{64};
  std::size_t max_evidence_ids{256};
  std::size_t max_claims{64};
  std::size_t max_confidence_terms{32};
};

struct ClassificationInput {
  SubjectRef subject{};
  std::vector<LossObservation> observations{};
  std::vector<RejectionNote> rejections{};
  std::vector<BoundNote> bounds{};
  Timestamp evaluated_at{};
  /// Number of counter or sequence series that were expected but absent, used
  /// to distinguish "nothing to look at" from "nothing found".
  std::size_t expected_series{0};
};

/// Per-source roll-up used for conflict detection and for explanations.
struct SourceClaim {
  SourceId source{};
  EpochId epoch{};
  MeasurementMethod method{MeasurementMethod::Unknown};
  Granularity granularity{Granularity::Unknown};
  LossClass klass{LossClass::Unknown};
  Freshness freshness{Freshness::Unknown};
  SourceAuthority authority{SourceAuthority::None};
  std::uint64_t lost{0};
  std::uint64_t offered{0};
  bool ratio_defined{false};
  std::uint32_t ratio_bp{0};
  std::size_t observation_count{0};
  bool synthetic{false};
  std::vector<MeasurementId> evidence_ids{};

  [[nodiscard]] std::string to_string() const;
};

struct Classification {
  SubjectRef subject{};
  LossClass klass{LossClass::Unknown};
  FreshnessSummary freshness{};
  Attribution attribution{};
  std::vector<ReasonCode> reasons{};
  std::vector<BoundNote> bounds{};
  std::vector<RejectionNote> rejections{};
  std::vector<SourceClaim> claims{};
  std::uint64_t lost_total{0};
  std::uint64_t offered_total{0};
  bool ratio_defined{false};
  std::uint32_t ratio_bp{0};
  bool totals_saturated{false};
  std::size_t evidence_considered{0};
  std::size_t evidence_admissible{0};
  std::vector<MeasurementId> evidence_ids{};
  bool evidence_ids_truncated{false};
  std::string summary{};

  [[nodiscard]] bool ok() const noexcept { return klass != LossClass::Unknown; }
  [[nodiscard]] std::string to_string() const;
};

/// Deterministic classifier.
///
/// Rule order is fixed and documented; each rule can only narrow the
/// conclusion. No rule can promote stale or unsupported evidence into loss.
class Classifier {
 public:
  Classifier(FreshnessContext context, LossPolicy policy)
      : context_(context), policy_(policy) {}

  [[nodiscard]] Classification classify(const ClassificationInput& input) const;

  /// Rolls observations up per source, deterministically ordered by
  /// (authority descending, source id ascending).
  [[nodiscard]] std::vector<SourceClaim> claims_for(const ClassificationInput& input) const;

  [[nodiscard]] const LossPolicy& policy() const noexcept { return policy_; }
  void set_policy(LossPolicy policy) noexcept { policy_ = policy; }
  [[nodiscard]] const FreshnessContext& context() const noexcept { return context_; }
  void set_context(FreshnessContext context) noexcept { context_ = context; }

 private:
  FreshnessContext context_{};
  LossPolicy policy_{};
};

}  // namespace loss_observatory
