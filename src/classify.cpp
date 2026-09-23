#include "loss_observatory/classify.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <string>

#include "loss_observatory/conflict.hpp"
#include "loss_observatory/core/checked.hpp"
#include "loss_observatory/report.hpp"

namespace loss_observatory {
namespace {

void push_reason(std::vector<ReasonCode>& reasons, ReasonCode code, std::size_t limit) {
  if (reasons.size() >= limit) {
    return;
  }
  if (std::find(reasons.begin(), reasons.end(), code) != reasons.end()) {
    return;
  }
  reasons.push_back(code);
}

[[nodiscard]] ReasonCode stale_reason_code(StaleReason reason) {
  switch (reason) {
    case StaleReason::None:
      return ReasonCode::EvidenceFresh;
    case StaleReason::AgeExceeded:
      return ReasonCode::EvidenceAgeExceeded;
    case StaleReason::EpochRetired:
      return ReasonCode::EvidenceEpochRetired;
    case StaleReason::EpochUnknown:
      return ReasonCode::EvidenceEpochUnknown;
    case StaleReason::GenerationSuperseded:
      return ReasonCode::EvidenceGenerationSuperseded;
    case StaleReason::GenerationUnknown:
      return ReasonCode::EvidenceGenerationUnknown;
    case StaleReason::TopologyRevisionChanged:
      return ReasonCode::EvidenceTopologyRevisionChanged;
    case StaleReason::RecoveredFromPersistence:
      return ReasonCode::EvidenceRecoveredFromPersistence;
    case StaleReason::SourceUnknown:
    case StaleReason::SourceIncarnationUnknown:
      return ReasonCode::EvidenceSourceUnknown;
    case StaleReason::FutureDated:
      return ReasonCode::EvidenceFutureDated;
    case StaleReason::Undated:
      return ReasonCode::EvidenceUndated;
    case StaleReason::ReceivedBeforeObserved:
      return ReasonCode::EvidenceReceivedBeforeObserved;
  }
  return ReasonCode::EvidenceSourceUnknown;
}

struct Tally {
  std::uint64_t lost{0};
  std::uint64_t offered{0};
  bool offered_known{false};
  bool ratio_seen{false};
  bool direct_seen{false};
  bool direct_positive{false};
  bool valid_direct{false};
  bool valid_ratio{false};
  bool valid_nonloss{false};
  bool discontinuity{false};
  bool implausible{false};
  bool insufficient{false};
  bool unsupported{false};
  bool undefined_ratio{false};
  bool saturated{false};
};

void tally_observation(Tally& tally, const LossObservation& observation) {
  switch (observation.validity) {
    case ObservationValidity::Valid:
      break;
    case ObservationValidity::Discontinuity:
      tally.discontinuity = true;
      return;
    case ObservationValidity::Implausible:
      tally.implausible = true;
      return;
    case ObservationValidity::Insufficient:
      tally.insufficient = true;
      return;
    case ObservationValidity::UndefinedRatio:
      tally.undefined_ratio = true;
      return;
    case ObservationValidity::UnsupportedMethod:
    case ObservationValidity::UnsupportedPrecondition:
      tally.unsupported = true;
      return;
  }

  switch (observation.semantics) {
    case LossSemantics::DirectLoss: {
      tally.direct_seen = true;
      tally.valid_direct = true;
      const auto before = tally.lost;
      tally.lost += observation.lost;
      if (tally.lost < before) {
        tally.lost = std::numeric_limits<std::uint64_t>::max();
        tally.saturated = true;
      }
      if (observation.lost > 0) {
        tally.direct_positive = true;
      }
      return;
    }
    case LossSemantics::RatioLoss: {
      tally.ratio_seen = true;
      if (!observation.ratio_defined) {
        tally.undefined_ratio = true;
        return;
      }
      tally.valid_ratio = true;
      const auto lost_before = tally.lost;
      const auto offered_before = tally.offered;
      tally.lost += observation.lost;
      tally.offered += observation.offered;
      if (tally.lost < lost_before || tally.offered < offered_before) {
        tally.lost = std::numeric_limits<std::uint64_t>::max();
        tally.offered = std::numeric_limits<std::uint64_t>::max();
        tally.saturated = true;
      }
      tally.offered_known = true;
      return;
    }
    case LossSemantics::NonLoss: {
      tally.valid_nonloss = true;
      return;
    }
  }
}

[[nodiscard]] bool has_loss_capable_evidence(const Tally& tally) {
  return tally.valid_direct || tally.valid_ratio;
}

[[nodiscard]] std::uint32_t tally_ratio_bp(const Tally& tally) {
  std::uint32_t ratio = 0;
  if (!tally.offered_known || tally.offered == 0) {
    return 0;
  }
  if (!checked::ratio_basis_points(tally.lost, tally.offered, ratio)) {
    return 0;
  }
  return ratio;
}

}  // namespace

std::string_view to_string(LossClass klass) noexcept {
  switch (klass) {
    case LossClass::Unknown:
      return "unknown";
    case LossClass::AbsentEvidence:
      return "absent-evidence";
    case LossClass::NoLossObserved:
      return "no-loss-observed";
    case LossClass::ConfirmedLoss:
      return "confirmed-loss";
    case LossClass::SuspectedLoss:
      return "suspected-loss";
    case LossClass::Discontinuity:
      return "discontinuity";
    case LossClass::StaleEvidence:
      return "stale-evidence";
    case LossClass::ConflictingEvidence:
      return "conflicting-evidence";
    case LossClass::IncompleteEvidence:
      return "incomplete-evidence";
    case LossClass::UnsupportedMethod:
      return "unsupported-method";
    case LossClass::ImplausibleEvidence:
      return "implausible-evidence";
  }
  return "unknown";
}

Result<LossClass> parse_loss_class(std::string_view text) {
  if (text == "unknown") {
    return LossClass::Unknown;
  }
  if (text == "absent-evidence") {
    return LossClass::AbsentEvidence;
  }
  if (text == "no-loss-observed") {
    return LossClass::NoLossObserved;
  }
  if (text == "confirmed-loss") {
    return LossClass::ConfirmedLoss;
  }
  if (text == "suspected-loss") {
    return LossClass::SuspectedLoss;
  }
  if (text == "discontinuity") {
    return LossClass::Discontinuity;
  }
  if (text == "stale-evidence") {
    return LossClass::StaleEvidence;
  }
  if (text == "conflicting-evidence") {
    return LossClass::ConflictingEvidence;
  }
  if (text == "incomplete-evidence") {
    return LossClass::IncompleteEvidence;
  }
  if (text == "unsupported-method") {
    return LossClass::UnsupportedMethod;
  }
  if (text == "implausible-evidence") {
    return LossClass::ImplausibleEvidence;
  }
  return make_status(StatusCode::InvalidArgument, "unknown loss class");
}

std::string_view to_string(ReasonCode code) noexcept {
  switch (code) {
    case ReasonCode::None:
      return "none";
    case ReasonCode::NoEvidenceSubmitted:
      return "no-evidence-submitted";
    case ReasonCode::EvidenceRejectedBeforeStorage:
      return "evidence-rejected-before-storage";
    case ReasonCode::EvidenceStoreEvictedOlderItems:
      return "evidence-store-evicted-older-items";
    case ReasonCode::EvidencePresent:
      return "evidence-present";
    case ReasonCode::EvidenceFresh:
      return "evidence-fresh";
    case ReasonCode::AllEvidenceStale:
      return "all-evidence-stale";
    case ReasonCode::EvidenceAgeExceeded:
      return "evidence-age-exceeded";
    case ReasonCode::EvidenceRecoveredFromPersistence:
      return "evidence-recovered-from-persistence";
    case ReasonCode::EvidenceEpochRetired:
      return "evidence-epoch-retired";
    case ReasonCode::EvidenceEpochUnknown:
      return "evidence-epoch-unknown";
    case ReasonCode::EvidenceGenerationSuperseded:
      return "evidence-generation-superseded";
    case ReasonCode::EvidenceGenerationUnknown:
      return "evidence-generation-unknown";
    case ReasonCode::EvidenceTopologyRevisionChanged:
      return "evidence-topology-revision-changed";
    case ReasonCode::EvidenceSourceUnknown:
      return "evidence-source-unknown";
    case ReasonCode::EvidenceFutureDated:
      return "evidence-future-dated";
    case ReasonCode::EvidenceUndated:
      return "evidence-undated";
    case ReasonCode::EvidenceReceivedBeforeObserved:
      return "evidence-received-before-observed";
    case ReasonCode::DiscontinuityObserved:
      return "discontinuity-observed";
    case ReasonCode::CounterWrapObserved:
      return "counter-wrap-observed";
    case ReasonCode::CounterResetObserved:
      return "counter-reset-observed";
    case ReasonCode::CounterWrapUnresolved:
      return "counter-wrap-unresolved";
    case ReasonCode::ImplausibleDeltaObserved:
      return "implausible-delta-observed";
    case ReasonCode::SequenceRestartObserved:
      return "sequence-restart-observed";
    case ReasonCode::EpochChangeObserved:
      return "epoch-change-observed";
    case ReasonCode::GenerationChangeObserved:
      return "generation-change-observed";
    case ReasonCode::OutOfOrderEvidence:
      return "out-of-order-evidence";
    case ReasonCode::MissingCounterBaseline:
      return "missing-counter-baseline";
    case ReasonCode::ImplausibleEvidenceObserved:
      return "implausible-evidence-observed";
    case ReasonCode::UndefinedRatio:
      return "undefined-ratio";
    case ReasonCode::CoverageBelowConfirmationThreshold:
      return "coverage-below-confirmation-threshold";
    case ReasonCode::CoverageBelowAbsenceThreshold:
      return "coverage-below-absence-threshold";
    case ReasonCode::PartialHopCoverage:
      return "partial-hop-coverage";
    case ReasonCode::UnobservedPathSpan:
      return "unobserved-path-span";
    case ReasonCode::EndpointReportsUnpaired:
      return "endpoint-reports-unpaired";
    case ReasonCode::EndpointPairingWindowExceeded:
      return "endpoint-pairing-window-exceeded";
    case ReasonCode::LossRatioAtOrAboveConfirmedThreshold:
      return "loss-ratio-at-or-above-confirmed-threshold";
    case ReasonCode::DirectLossCounterAdvanced:
      return "direct-loss-counter-advanced";
    case ReasonCode::LossRatioBelowConfirmationThreshold:
      return "loss-ratio-below-confirmation-threshold";
    case ReasonCode::ZeroLossWithAdequateCoverage:
      return "zero-loss-with-adequate-coverage";
    case ReasonCode::DirectLossCounterStatic:
      return "direct-loss-counter-static";
    case ReasonCode::SourceDisagreementOnClass:
      return "source-disagreement-on-class";
    case ReasonCode::SourceDisagreementOnRatio:
      return "source-disagreement-on-ratio";
    case ReasonCode::ConflictResolvedByAuthority:
      return "conflict-resolved-by-authority";
    case ReasonCode::ConflictUnresolved:
      return "conflict-unresolved";
    case ReasonCode::ConflictToleranceApplied:
      return "conflict-tolerance-applied";
    case ReasonCode::LowerAuthorityClaimRetained:
      return "lower-authority-claim-retained";
    case ReasonCode::MethodNotImplemented:
      return "method-not-implemented";
    case ReasonCode::MethodRequiresSynchronizedClocks:
      return "method-requires-synchronized-clocks";
    case ReasonCode::MethodRequiresStableGeneration:
      return "method-requires-stable-generation";
    case ReasonCode::GranularityExceedsMethodCapability:
      return "granularity-exceeds-method-capability";
    case ReasonCode::SyntheticEvidenceUsed:
      return "synthetic-evidence-used";
    case ReasonCode::ResultSetTruncated:
      return "result-set-truncated";
    case ReasonCode::EvidenceListTruncated:
      return "evidence-list-truncated";
    case ReasonCode::AggregationWindowBoundReached:
      return "aggregation-window-bound-reached";
    case ReasonCode::AggregationSaturated:
      return "aggregation-saturated";
    case ReasonCode::HistoryBoundReached:
      return "history-bound-reached";
    case ReasonCode::PersistenceBoundReached:
      return "persistence-bound-reached";
    case ReasonCode::LocalizedToFinestSupportedGranularity:
      return "localized-to-finest-supported-granularity";
    case ReasonCode::LocalizationClampedToEvidenceGranularity:
      return "localization-clamped-to-evidence-granularity";
    case ReasonCode::AmbiguityRetained:
      return "ambiguity-retained";
    case ReasonCode::PathUnknown:
      return "path-unknown";
    case ReasonCode::HopEvidenceAbsent:
      return "hop-evidence-absent";
  }
  return "none";
}

std::string_view to_string(ConfidenceBand band) noexcept {
  switch (band) {
    case ConfidenceBand::None:
      return "none";
    case ConfidenceBand::VeryLow:
      return "very-low";
    case ConfidenceBand::Low:
      return "low";
    case ConfidenceBand::Moderate:
      return "moderate";
    case ConfidenceBand::High:
      return "high";
    case ConfidenceBand::VeryHigh:
      return "very-high";
  }
  return "none";
}

ConfidenceBand band_for_score(std::int32_t score) noexcept {
  if (score >= 80) {
    return ConfidenceBand::VeryHigh;
  }
  if (score >= 60) {
    return ConfidenceBand::High;
  }
  if (score >= 40) {
    return ConfidenceBand::Moderate;
  }
  if (score >= 20) {
    return ConfidenceBand::Low;
  }
  if (score >= 1) {
    return ConfidenceBand::VeryLow;
  }
  return ConfidenceBand::None;
}

std::string Classification::to_string() const { return render_classification(*this); }

std::string SourceClaim::to_string() const {
  std::string result = "claim source=";
  result.append(source.to_string());
  result.append(" authority=");
  result.append(loss_observatory::to_string(authority));
  result.append(" method=");
  result.append(loss_observatory::to_string(method));
  result.append(" class=");
  result.append(loss_observatory::to_string(klass));
  result.append(" freshness=");
  result.append(loss_observatory::to_string(freshness));
  result.append(" lost=");
  result.append(std::to_string(lost));
  result.append(" offered=");
  result.append(std::to_string(offered));
  result.append(" ratio_bp=");
  result.append(ratio_defined ? std::to_string(ratio_bp) : std::string("undefined"));
  result.append(" observations=");
  result.append(std::to_string(observation_count));
  if (synthetic) {
    result.append(" synthetic=true");
  }
  return result;
}

std::vector<SourceClaim> Classifier::claims_for(const ClassificationInput& input) const {
  std::map<SourceId, SourceClaim> claims;
  std::map<SourceId, Tally> tallies;
  std::map<SourceId, bool> any_fresh;

  std::map<SourceId, Freshness> first_stale;

  for (const LossObservation& observation : input.observations) {
    SourceClaim& claim = claims[observation.source];
    claim.source = observation.source;
    claim.epoch = observation.epoch;
    claim.method = observation.method;
    claim.granularity = finer_of(claim.granularity, observation.granularity);
    ++claim.observation_count;
    claim.synthetic = claim.synthetic || observation.synthetic;
    if (observation.evidence_id != MeasurementId{} && claim.evidence_ids.size() < policy_.max_evidence_ids) {
      claim.evidence_ids.push_back(observation.evidence_id);
    }

    const FreshnessAssessment assessment =
        assess_freshness(observation, input.evaluated_at, policy_.freshness, context_);
    const bool fresh = assessment.admissible;
    any_fresh[observation.source] = any_fresh[observation.source] || fresh;
    if (!fresh && first_stale.find(observation.source) == first_stale.end()) {
      // Observations arrive in canonical order, so the first stale state seen
      // for a source is the one attached to its earliest observation.
      first_stale[observation.source] = assessment.state;
    }

    if (fresh) {
      tally_observation(tallies[observation.source], observation);
    }
  }

  for (auto& entry : claims) {
    SourceClaim& target = entry.second;
    const Tally& tally = tallies[entry.first];
    target.lost = tally.lost;
    target.offered = tally.offered;
    target.ratio_defined = tally.offered_known && tally.offered > 0;
    target.ratio_bp = tally_ratio_bp(tally);
    if (any_fresh[entry.first]) {
      target.freshness = Freshness::Fresh;
    } else {
      const auto stale = first_stale.find(entry.first);
      target.freshness = stale == first_stale.end() ? Freshness::Unknown : stale->second;
    }
    if (context_.sources != nullptr) {
      const Result<SourceAuthority> authority = context_.sources->authority_of(entry.first);
      target.authority = authority.ok() ? authority.value() : SourceAuthority::None;
    }

    if (!any_fresh[entry.first]) {
      target.klass = LossClass::StaleEvidence;
      continue;
    }
    if (has_loss_capable_evidence(tally)) {
      const std::uint32_t ratio = tally_ratio_bp(tally);
      if (tally.valid_ratio && tally.offered >= policy_.min_offered_for_confirmation &&
          ratio >= policy_.confirmed_ratio_bp) {
        target.klass = LossClass::ConfirmedLoss;
      } else if (tally.direct_positive && policy_.direct_loss_counter_confirms) {
        target.klass = LossClass::ConfirmedLoss;
      } else if ((tally.valid_ratio && ratio > 0) || tally.direct_positive) {
        target.klass = LossClass::SuspectedLoss;
      } else if (tally.valid_ratio && tally.offered >= policy_.min_offered_for_absence && ratio == 0) {
        target.klass = LossClass::NoLossObserved;
      } else if (tally.valid_direct && !tally.direct_positive && !tally.ratio_seen) {
        target.klass = LossClass::NoLossObserved;
      } else {
        target.klass = LossClass::IncompleteEvidence;
      }
      continue;
    }
    if (tally.discontinuity) {
      target.klass = LossClass::Discontinuity;
    } else if (tally.implausible) {
      target.klass = LossClass::ImplausibleEvidence;
    } else if (tally.unsupported) {
      target.klass = LossClass::UnsupportedMethod;
    } else {
      target.klass = LossClass::IncompleteEvidence;
    }
  }

  std::vector<SourceClaim> result;
  result.reserve(claims.size());
  for (auto& entry : claims) {
    result.push_back(std::move(entry.second));
  }
  std::sort(result.begin(), result.end(), [](const SourceClaim& lhs, const SourceClaim& rhs) {
    if (lhs.authority != rhs.authority) {
      return static_cast<std::uint8_t>(lhs.authority) > static_cast<std::uint8_t>(rhs.authority);
    }
    return lhs.source < rhs.source;
  });
  return result;
}

Classification Classifier::classify(const ClassificationInput& input) const {
  Classification result{};
  result.subject = input.subject;
  result.bounds = input.bounds;
  result.rejections = input.rejections;
  result.evidence_considered = input.observations.size();

  std::vector<FreshnessAssessment> assessments;
  assessments.reserve(input.observations.size());
  for (const LossObservation& observation : input.observations) {
    assessments.push_back(assess_freshness(observation, input.evaluated_at, policy_.freshness, context_));
  }
  result.freshness = summarize(assessments);

  std::vector<const LossObservation*> admissible;
  Tally tally{};
  for (std::size_t i = 0; i < input.observations.size(); ++i) {
    const FreshnessAssessment& assessment = assessments[i];
    if (!assessment.admissible) {
      push_reason(result.reasons, stale_reason_code(assessment.reason), policy_.max_reasons);
      continue;
    }
    admissible.push_back(&input.observations[i]);
    tally_observation(tally, input.observations[i]);
  }

  result.claims = claims_for(input);
  std::vector<SourceClaim> fresh_claims;
  for (const SourceClaim& claim : result.claims) {
    if (claim.freshness == Freshness::Fresh) {
      fresh_claims.push_back(claim);
    }
  }

  const ConflictResolution conflict = ConflictDetector(policy_).evaluate(fresh_claims);
  for (const SourceClaim& retained : conflict.retained_lower_authority) {
    push_reason(result.reasons, ReasonCode::LowerAuthorityClaimRetained, policy_.max_reasons);
    (void)retained;
  }
  if (conflict.conflicted) {
    push_reason(result.reasons,
                conflict.basis == ConflictBasis::ClassDivergence ? ReasonCode::SourceDisagreementOnClass
                                                                 : ReasonCode::SourceDisagreementOnRatio,
                policy_.max_reasons);
    if (conflict.resolved) {
      push_reason(result.reasons, ReasonCode::ConflictResolvedByAuthority, policy_.max_reasons);
    }
  }
  push_reason(result.reasons, ReasonCode::ConflictToleranceApplied, policy_.max_reasons);

  // ---- decision ladder ---------------------------------------------------
  const bool have_observations = !input.observations.empty();
  if (!have_observations) {
    result.klass = LossClass::AbsentEvidence;
    push_reason(result.reasons,
                input.rejections.empty() ? ReasonCode::NoEvidenceSubmitted
                                         : ReasonCode::EvidenceRejectedBeforeStorage,
                policy_.max_reasons);
    if (!input.bounds.empty()) {
      push_reason(result.reasons, ReasonCode::EvidenceStoreEvictedOlderItems, policy_.max_reasons);
    }
  } else if (admissible.empty()) {
    result.klass = LossClass::StaleEvidence;
    push_reason(result.reasons, ReasonCode::AllEvidenceStale, policy_.max_reasons);
  } else if (conflict.conflicted && !conflict.resolved) {
    result.klass = LossClass::ConflictingEvidence;
    push_reason(result.reasons, ReasonCode::ConflictUnresolved, policy_.max_reasons);
  } else {
    result.lost_total = tally.lost;
    result.offered_total = tally.offered;
    result.totals_saturated = tally.saturated;
    result.ratio_defined = tally.offered_known && tally.offered > 0;
    result.ratio_bp = tally_ratio_bp(tally);

    if (tally.discontinuity && !has_loss_capable_evidence(tally)) {
      result.klass = LossClass::Discontinuity;
      push_reason(result.reasons, ReasonCode::DiscontinuityObserved, policy_.max_reasons);
    } else if (tally.implausible && !has_loss_capable_evidence(tally)) {
      result.klass = LossClass::ImplausibleEvidence;
      push_reason(result.reasons, ReasonCode::ImplausibleEvidenceObserved, policy_.max_reasons);
    } else if (tally.unsupported && !has_loss_capable_evidence(tally)) {
      result.klass = LossClass::UnsupportedMethod;
      push_reason(result.reasons, ReasonCode::MethodNotImplemented, policy_.max_reasons);
    } else if (!has_loss_capable_evidence(tally)) {
      result.klass = LossClass::IncompleteEvidence;
      if (tally.undefined_ratio) {
        push_reason(result.reasons, ReasonCode::UndefinedRatio, policy_.max_reasons);
      }
      if (tally.insufficient) {
        push_reason(result.reasons, ReasonCode::MissingCounterBaseline, policy_.max_reasons);
      }
      if (tally.valid_nonloss) {
        push_reason(result.reasons, ReasonCode::CoverageBelowAbsenceThreshold, policy_.max_reasons);
      }
    } else if (tally.valid_ratio && tally.offered >= policy_.min_offered_for_confirmation &&
               result.ratio_bp >= policy_.confirmed_ratio_bp) {
      result.klass = LossClass::ConfirmedLoss;
      push_reason(result.reasons, ReasonCode::LossRatioAtOrAboveConfirmedThreshold, policy_.max_reasons);
    } else if (tally.direct_positive && policy_.direct_loss_counter_confirms) {
      result.klass = LossClass::ConfirmedLoss;
      push_reason(result.reasons, ReasonCode::DirectLossCounterAdvanced, policy_.max_reasons);
    } else if ((tally.valid_ratio && result.ratio_bp >= policy_.suspected_ratio_bp && result.ratio_bp > 0) ||
               tally.direct_positive) {
      result.klass = LossClass::SuspectedLoss;
      push_reason(result.reasons, ReasonCode::LossRatioBelowConfirmationThreshold, policy_.max_reasons);
      if (tally.valid_ratio && tally.offered < policy_.min_offered_for_confirmation) {
        push_reason(result.reasons, ReasonCode::CoverageBelowConfirmationThreshold, policy_.max_reasons);
      }
    } else if (tally.valid_ratio && tally.offered >= policy_.min_offered_for_absence && result.ratio_bp == 0) {
      result.klass = LossClass::NoLossObserved;
      push_reason(result.reasons, ReasonCode::ZeroLossWithAdequateCoverage, policy_.max_reasons);
    } else if (tally.valid_direct && tally.direct_seen && !tally.direct_positive) {
      result.klass = LossClass::NoLossObserved;
      push_reason(result.reasons, ReasonCode::DirectLossCounterStatic, policy_.max_reasons);
    } else {
      result.klass = LossClass::IncompleteEvidence;
      push_reason(result.reasons, ReasonCode::CoverageBelowAbsenceThreshold, policy_.max_reasons);
    }

    if (tally.discontinuity) {
      push_reason(result.reasons, ReasonCode::DiscontinuityObserved, policy_.max_reasons);
    }
    if (tally.implausible) {
      push_reason(result.reasons, ReasonCode::ImplausibleEvidenceObserved, policy_.max_reasons);
    }
    if (tally.unsupported) {
      push_reason(result.reasons, ReasonCode::MethodNotImplemented, policy_.max_reasons);
    }
    if (tally.saturated) {
      push_reason(result.reasons, ReasonCode::AggregationSaturated, policy_.max_reasons);
    }
  }

  // Reasons derived from the admissible set.
  for (const LossObservation& observation : input.observations) {
    if (observation.discontinuity == DiscontinuityKind::CounterWrap) {
      push_reason(result.reasons, ReasonCode::CounterWrapObserved, policy_.max_reasons);
    } else if (observation.discontinuity == DiscontinuityKind::CounterReset) {
      push_reason(result.reasons, ReasonCode::CounterResetObserved, policy_.max_reasons);
    } else if (observation.discontinuity == DiscontinuityKind::DiscontinuityUnresolved) {
      push_reason(result.reasons, ReasonCode::CounterWrapUnresolved, policy_.max_reasons);
    } else if (observation.discontinuity == DiscontinuityKind::SequenceRestart) {
      push_reason(result.reasons, ReasonCode::SequenceRestartObserved, policy_.max_reasons);
    } else if (observation.discontinuity == DiscontinuityKind::EpochChange) {
      push_reason(result.reasons, ReasonCode::EpochChangeObserved, policy_.max_reasons);
    } else if (observation.discontinuity == DiscontinuityKind::GenerationChange) {
      push_reason(result.reasons, ReasonCode::GenerationChangeObserved, policy_.max_reasons);
    } else if (observation.discontinuity == DiscontinuityKind::OutOfOrder) {
      push_reason(result.reasons, ReasonCode::OutOfOrderEvidence, policy_.max_reasons);
    } else if (observation.discontinuity == DiscontinuityKind::ImplausibleDelta) {
      push_reason(result.reasons, ReasonCode::ImplausibleDeltaObserved, policy_.max_reasons);
    } else if (observation.discontinuity == DiscontinuityKind::MissingBaseline) {
      push_reason(result.reasons, ReasonCode::MissingCounterBaseline, policy_.max_reasons);
    }
    if (observation.synthetic) {
      push_reason(result.reasons, ReasonCode::SyntheticEvidenceUsed, policy_.max_reasons);
    }
  }
  if (tally.valid_ratio || tally.valid_direct) {
    push_reason(result.reasons, ReasonCode::EvidencePresent, policy_.max_reasons);
  }
  if (result.freshness.fresh > 0) {
    push_reason(result.reasons, ReasonCode::EvidenceFresh, policy_.max_reasons);
  }

  // Evidence ids (bounded).
  for (const LossObservation& observation : input.observations) {
    if (result.evidence_ids.size() >= policy_.max_evidence_ids) {
      result.evidence_ids_truncated = true;
      push_reason(result.reasons, ReasonCode::EvidenceListTruncated, policy_.max_reasons);
      break;
    }
    result.evidence_ids.push_back(observation.id);
  }
  result.evidence_admissible = admissible.size();

  // ---- attribution -------------------------------------------------------
  {
    Attribution attribution{};
    auto add_term = [&](ReasonCode code, std::int32_t points, std::string detail) {
      if (attribution.terms.size() >= policy_.max_confidence_terms) {
        return;
      }
      attribution.terms.push_back(ConfidenceTerm{code, points, std::move(detail)});
      attribution.score += points;
    };
    if (result.freshness.fresh > 0) {
      add_term(ReasonCode::EvidenceFresh, 25, std::to_string(result.freshness.fresh) + " fresh observation(s)");
    }
    if (tally.valid_ratio && tally.offered >= policy_.min_offered_for_confirmation) {
      add_term(ReasonCode::LossRatioAtOrAboveConfirmedThreshold, 30,
               std::to_string(tally.offered) + " offered units behind the ratio");
    } else if (tally.valid_ratio) {
      add_term(ReasonCode::CoverageBelowConfirmationThreshold, 5,
               std::to_string(tally.offered) + " offered units is below the confirmation minimum");
    }
    if (tally.valid_direct) {
      add_term(ReasonCode::DirectLossCounterAdvanced, 20, "at least one direct loss counter observation");
    }
    if (fresh_claims.size() > 1) {
      const std::size_t agreement = [&]() {
        std::size_t count = 0;
        for (const SourceClaim& claim : fresh_claims) {
          if (claim.klass == result.klass) {
            ++count;
          }
        }
        return count;
      }();
      if (agreement > 1) {
        add_term(ReasonCode::EvidencePresent, 15,
                 std::to_string(agreement) + " independent sources agree on the conclusion");
      }
    }
    if (conflict.conflicted && conflict.resolved) {
      add_term(ReasonCode::ConflictResolvedByAuthority, 10, "a strictly higher authority resolved disagreement");
    }
    if (conflict.conflicted && !conflict.resolved) {
      add_term(ReasonCode::ConflictUnresolved, -30, "admissible sources disagree and no source outranks them");
    }
    if (result.freshness.stale > 0) {
      add_term(ReasonCode::AllEvidenceStale, -15, std::to_string(result.freshness.stale) + " stale observation(s)");
    }
    if (result.freshness.not_current > 0) {
      add_term(ReasonCode::EvidenceRecoveredFromPersistence, -25,
               std::to_string(result.freshness.not_current) + " recovered observation(s)");
    }
    if (tally.discontinuity) {
      add_term(ReasonCode::DiscontinuityObserved, -20, "the evidence set contains a discontinuity");
    }
    if (tally.implausible) {
      add_term(ReasonCode::ImplausibleEvidenceObserved, -20, "the evidence set contains an implausible record");
    }
    if (tally.unsupported) {
      add_term(ReasonCode::MethodNotImplemented, -20, "the evidence set contains an unsupported method");
    }
    if (!input.bounds.empty()) {
      add_term(ReasonCode::ResultSetTruncated, -10, "a configured bound was reached while gathering evidence");
    }
    if (result.evidence_ids_truncated) {
      add_term(ReasonCode::EvidenceListTruncated, -5, "the evidence reference list was truncated");
    }
    if (tally.saturated) {
      add_term(ReasonCode::AggregationSaturated, -10, "a total saturated during accumulation");
    }
    if (attribution.score < 0) {
      attribution.score = 0;
    }
    if (attribution.score > 100) {
      attribution.score = 100;
    }
    if (!asserts_loss(result.klass) && !asserts_absence(result.klass)) {
      // Confidence describes how firmly the conclusion is held; conclusions
      // that assert nothing carry no confidence in an assertion.
      if (result.klass != LossClass::ConflictingEvidence) {
        attribution.score = attribution.score > 10 ? 10 : attribution.score;
      }
    }
    attribution.band = band_for_score(attribution.score);
    attribution.rationale = "score=" + std::to_string(attribution.score) +
                            " band=" + std::string(loss_observatory::to_string(attribution.band));
    result.attribution = std::move(attribution);
  }

  result.summary = render_classification(result);
  if (result.summary.empty()) {
    result.summary = std::string(loss_observatory::to_string(result.klass));
  }
  return result;
}

}  // namespace loss_observatory
