#include "loss_observatory/localize.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "loss_observatory/core/checked.hpp"

namespace loss_observatory {
namespace {

[[nodiscard]] std::size_t segment_limit(const LocalizationRequest& request, const LocalizePolicy& policy) {
  const std::size_t requested = request.max_segments == 0 ? policy.max_segments : request.max_segments;
  return std::min(requested, policy.max_segments);
}

}  // namespace

std::string_view to_string(SegmentKind kind) noexcept {
  switch (kind) {
    case SegmentKind::Observed:
      return "observed";
    case SegmentKind::FlowWide:
      return "flow-wide";
    case SegmentKind::UnobservedSpan:
      return "unobserved-span";
    case SegmentKind::Unresolved:
      return "unresolved";
  }
  return "unresolved";
}

std::string LocalizedSegment::to_string() const {
  std::string result("segment ");
  result.append(loss_observatory::to_string(kind));
  result.append(" granularity=");
  result.append(loss_observatory::to_string(granularity));
  result.append(" subject=");
  result.append(subject.to_string());
  if (hop_range_known) {
    result.append(" hops=");
    result.append(std::to_string(first_hop));
    if (last_hop != first_hop) {
      result.push_back('-');
      result.append(std::to_string(last_hop));
    }
  }
  result.append(" class=");
  result.append(loss_observatory::to_string(klass));
  result.append(" confidence=");
  result.append(loss_observatory::to_string(confidence));
  result.append(" lost=");
  result.append(std::to_string(lost));
  result.append(" offered=");
  result.append(std::to_string(offered));
  result.append(" ratio_bp=");
  result.append(ratio_defined ? std::to_string(ratio_bp) : std::string("undefined"));
  result.append(" observations=");
  result.append(std::to_string(observation_count));
  return result;
}

std::string AmbiguityNote::to_string() const {
  std::string result("ambiguity reason=");
  result.append(loss_observatory::to_string(reason));
  result.append(" widest_possible=");
  result.append(loss_observatory::to_string(widest_possible));
  result.append(" achieved=");
  result.append(loss_observatory::to_string(achieved));
  result.append(" candidates=");
  result.append(std::to_string(candidate_count));
  if (!detail.empty()) {
    result.append(" detail=");
    result.append(detail);
  }
  return result;
}

std::string LocalizationResult::to_string() const {
  std::string result("localization flow=");
  result.append(flow.to_string());
  result.append(" path=");
  result.append(path_known ? path.to_string() : std::string("undeclared"));
  result.append(" aggregate=");
  result.append(loss_observatory::to_string(aggregate_class));
  result.append(" achieved=");
  result.append(loss_observatory::to_string(achieved_granularity));
  result.append(" evidence=");
  result.append(loss_observatory::to_string(evidence_granularity));
  result.append(" ambiguous=");
  result.append(ambiguous ? "true" : "false");
  result.append(" segments=");
  result.append(std::to_string(segments.size()));
  result.append(" hops=");
  result.append(std::to_string(hops_with_evidence));
  result.push_back('/');
  result.append(std::to_string(hops_in_path));
  return result;
}

LocalizationResult Localizer::localize(const LocalizationRequest& request,
                                       std::span<const LossObservation> observations, Timestamp now,
                                       const LossPolicy& policy,
                                       const FreshnessContext& context) const {
  LocalizationResult result{};
  result.subject = SubjectRef::flow(request.flow);
  result.flow = request.flow;
  // An unspecified instant means "now": localization is always evaluated
  // against an explicit instant, never against a hidden global clock.
  const Timestamp instant = request.at.is_zero() ? now : request.at;
  result.at = instant;
  const std::size_t limit = segment_limit(request, policy_);

  Classifier classifier(context, policy);

  const Result<FlowBinding> flow = topology_->find_flow(request.flow);
  std::vector<Hop> hops;
  if (flow.ok()) {
    result.path = flow.value().path;
    const Result<std::vector<Hop>> declared = topology_->hops_of(result.path);
    if (declared.ok()) {
      hops = declared.value();
      result.path_known = true;
    }
  }
  result.hops_in_path = hops.size();

  // ---- coverage ----------------------------------------------------------
  struct Relevant {
    const LossObservation* observation{nullptr};
    /// Hop positions this observation speaks about. Empty for flow-scoped
    /// evidence, which stays relevant whether or not a path is declared.
    std::vector<std::uint32_t> covered{};
    bool flow_wide{false};
    bool admissible{false};
  };
  std::vector<Relevant> relevant;
  relevant.reserve(observations.size());

  std::vector<FreshnessAssessment> assessments;
  assessments.reserve(observations.size());
  for (const LossObservation& observation : observations) {
    assessments.push_back(assess_freshness(observation, instant, policy.freshness, context));
  }

  for (std::size_t i = 0; i < observations.size(); ++i) {
    const LossObservation& observation = observations[i];
    Relevant entry{};
    entry.observation = &observation;
    entry.admissible = assessments[i].admissible;
    switch (observation.subject.kind()) {
      case SubjectKind::Flow:
        if (observation.subject.raw_id() == request.flow.value()) {
          entry.flow_wide = true;
          for (const Hop& hop : hops) {
            entry.covered.push_back(hop.index);
          }
        }
        break;
      case SubjectKind::Path:
        if (result.path_known && observation.subject.raw_id() == result.path.value()) {
          for (const Hop& hop : hops) {
            entry.covered.push_back(hop.index);
          }
        }
        break;
      case SubjectKind::Hop: {
        for (const Hop& hop : hops) {
          if (hop.id.value() == observation.subject.raw_id()) {
            entry.covered.push_back(hop.index);
            break;
          }
        }
        break;
      }
      case SubjectKind::Link: {
        for (const Hop& hop : hops) {
          if (hop.ingress_link.value() == observation.subject.raw_id() ||
              hop.egress_link.value() == observation.subject.raw_id()) {
            entry.covered.push_back(hop.index);
          }
        }
        break;
      }
      case SubjectKind::Queue: {
        const Result<QueueEntity> queue =
            topology_->find_queue(QueueId::from_value(observation.subject.raw_id()));
        if (queue.ok()) {
          for (const Hop& hop : hops) {
            if (hop.node != queue.value().node) {
              continue;
            }
            if (hop.egress_port == queue.value().port || hop.ingress_port == queue.value().port) {
              entry.covered.push_back(hop.index);
            }
          }
        }
        break;
      }
      case SubjectKind::Source:
      case SubjectKind::Unknown:
        break;
    }
    const bool flow_scoped = entry.flow_wide || observation.subject.kind() == SubjectKind::Flow ||
                             observation.subject.kind() == SubjectKind::Path;
    if (!flow_scoped && entry.covered.empty()) {
      continue;
    }
    relevant.push_back(std::move(entry));
  }

  // ---- aggregate class for the flow --------------------------------------
  // Every observation about the flow feeds the aggregate, stale ones included:
  // the classifier is what decides staleness, and it reports StaleEvidence
  // rather than letting the evidence disappear.
  ClassificationInput aggregate_input{};
  aggregate_input.subject = SubjectRef::flow(request.flow);
  aggregate_input.evaluated_at = instant;
  for (const Relevant& entry : relevant) {
    aggregate_input.observations.push_back(*entry.observation);
  }
  const Classification aggregate = classifier.classify(aggregate_input);
  result.aggregate_class = aggregate.klass;
  result.reasons = aggregate.reasons;

  // Finest granularity the admissible evidence could ever support.
  Granularity evidence_granularity = Granularity::Unknown;
  std::size_t admissible_count = 0;
  for (const Relevant& entry : relevant) {
    if (!entry.admissible) {
      continue;
    }
    ++admissible_count;
    evidence_granularity = finer_of(evidence_granularity, entry.observation->granularity);
  }
  result.evidence_granularity = evidence_granularity;

  if (admissible_count == 0) {
    result.achieved_granularity = Granularity::Unknown;
    AmbiguityNote note{};
    note.widest_possible = aggregate.klass == LossClass::AbsentEvidence ? Granularity::Unknown : Granularity::Flow;
    note.achieved = Granularity::Unknown;
    note.reason = ReasonCode::AmbiguityRetained;
    note.candidate_count = hops.size();
    note.detail = "no admissible evidence covers this flow, so nothing can be localized";
    result.ambiguity.push_back(std::move(note));
    result.ambiguous = true;
    result.reasons.push_back(ReasonCode::AmbiguityRetained);
    result.summary = result.to_string();
    return result;
  }

  // A hop may only ever be named when the declared path is known. Evidence
  // that claims hop-level precision without a declared path is downgraded: the
  // runtime will not manufacture a topology to justify a precise answer.
  const Granularity cap = coarser_of(request.requested_max_granularity, evidence_granularity);
  const bool path_blocks_precision = !result.path_known || hops.size() < 2;

  if (path_blocks_precision) {
    LocalizedSegment segment{};
    segment.kind = SegmentKind::FlowWide;
    segment.granularity = coarser_of(cap, Granularity::Flow);
    segment.subject = SubjectRef::flow(request.flow);
    segment.klass = aggregate.klass;
    segment.confidence = aggregate.attribution.band;
    segment.lost = aggregate.lost_total;
    segment.offered = aggregate.offered_total;
    segment.ratio_defined = aggregate.ratio_defined;
    segment.ratio_bp = aggregate.ratio_bp;
    segment.observation_count = aggregate.evidence_admissible;
    segment.evidence_ids = aggregate.evidence_ids;
    segment.reasons = aggregate.reasons;
    result.segments.push_back(std::move(segment));
    result.achieved_granularity = Granularity::Flow;
    result.ambiguous = true;
    AmbiguityNote note{};
    note.widest_possible = evidence_granularity;
    note.achieved = Granularity::Flow;
    const bool declared = result.path_known;
    note.reason = declared ? ReasonCode::AmbiguityRetained : ReasonCode::PathUnknown;
    note.candidate_count = hops.size();
    note.detail = declared
                      ? "the declared path has fewer than two hops, so no interior localization exists"
                      : "the flow path is not declared, so no entity on the path can be named";
    result.ambiguity.push_back(std::move(note));
    if (!declared) {
      result.reasons.push_back(ReasonCode::PathUnknown);
    }
    result.summary = result.to_string();
    return result;
  }

  // The finest claim supportable by both the evidence and the caller's cap.
  const Granularity attribution_granularity = coarser_of(cap, evidence_granularity);

  if (!is_finer_than(attribution_granularity, Granularity::Path)) {
    // The supported claim is a flow or a path as a whole, so no hop may be
    // named. Either the evidence cannot reach hop scope or the caller capped
    // the request; the reason says which.
    const bool clamped_by_caller = is_finer_than(evidence_granularity, attribution_granularity);
    const Granularity achieved_here = attribution_granularity;
    LocalizedSegment segment{};
    segment.kind = SegmentKind::FlowWide;
    segment.granularity = achieved_here;
    segment.subject = achieved_here == Granularity::Path ? SubjectRef::path(result.path)
                                                         : SubjectRef::flow(request.flow);
    segment.hop_range_known = true;
    segment.first_hop = hops.front().index;
    segment.last_hop = hops.back().index;
    segment.klass = aggregate.klass;
    segment.confidence = aggregate.attribution.band;
    segment.lost = aggregate.lost_total;
    segment.offered = aggregate.offered_total;
    segment.ratio_defined = aggregate.ratio_defined;
    segment.ratio_bp = aggregate.ratio_bp;
    segment.observation_count = aggregate.evidence_admissible;
    segment.evidence_ids = aggregate.evidence_ids;
    segment.reasons = aggregate.reasons;
    result.segments.push_back(std::move(segment));
    result.achieved_granularity = achieved_here;
    result.ambiguous = true;
    AmbiguityNote note{};
    note.widest_possible = evidence_granularity;
    note.achieved = achieved_here;
    note.reason = clamped_by_caller ? ReasonCode::LocalizationClampedToEvidenceGranularity
                                    : ReasonCode::AmbiguityRetained;
    note.candidate_count = hops.size();
    note.detail = clamped_by_caller
                      ? "the caller capped the requested granularity, so the path is reported as a whole"
                      : "the evidence is coarser than a hop, so the path is reported as a whole";
    result.ambiguity.push_back(std::move(note));
    result.reasons.push_back(clamped_by_caller ? ReasonCode::LocalizationClampedToEvidenceGranularity
                                               : ReasonCode::AmbiguityRetained);
    result.summary = result.to_string();
    return result;
  }

  // ---- per-hop classification -------------------------------------------
  std::vector<std::vector<const LossObservation*>> per_hop(hops.size());
  for (const Relevant& entry : relevant) {
    if (!entry.admissible) {
      continue;
    }
    // Only evidence that is at least hop-scoped may be attributed to a hop.
    if (!is_finer_than(entry.observation->granularity, Granularity::Path)) {
      continue;
    }
    for (const std::uint32_t index : entry.covered) {
      for (std::size_t position = 0; position < hops.size(); ++position) {
        if (hops[position].index == index) {
          per_hop[position].push_back(entry.observation);
          break;
        }
      }
    }
  }

  struct HopState {
    bool observed{false};
    LossClass klass{LossClass::Unknown};
    Granularity granularity{Granularity::Unknown};
  };
  std::vector<HopState> states(hops.size());
  for (std::size_t position = 0; position < hops.size(); ++position) {
    if (per_hop[position].empty()) {
      continue;
    }
    ClassificationInput input{};
    input.subject = SubjectRef::hop(hops[position].id);
    input.evaluated_at = instant;
    Granularity granularity = Granularity::Unknown;
    for (const LossObservation* observation : per_hop[position]) {
      input.observations.push_back(*observation);
      granularity = finer_of(granularity, observation->granularity);
    }
    const Classification classified = classifier.classify(input);
    states[position].observed = true;
    states[position].klass = classified.klass;
    states[position].granularity = granularity;
    ++result.hops_with_evidence;
  }

  // ---- segments ----------------------------------------------------------
  struct Run {
    std::size_t begin{0};
    std::size_t end{0};
    bool observed{false};
    LossClass klass{LossClass::Unknown};
    Granularity granularity{Granularity::Unknown};
  };
  std::vector<Run> runs;
  for (std::size_t position = 0; position < hops.size(); ++position) {
    const bool observed = states[position].observed;
    const LossClass klass = observed ? states[position].klass : LossClass::Unknown;
    const Granularity granularity = observed ? states[position].granularity : Granularity::Unknown;
    if (!runs.empty() && runs.back().observed == observed && runs.back().klass == klass &&
        runs.back().granularity == granularity) {
      runs.back().end = position;
      continue;
    }
    runs.push_back(Run{position, position, observed, klass, granularity});
  }

  Granularity achieved = Granularity::Unknown;
  for (const Run& run : runs) {
    LocalizedSegment segment{};
    segment.first_hop = hops[run.begin].index;
    segment.last_hop = hops[run.end].index;
    segment.hop_range_known = true;
    if (run.observed) {
      segment.kind = SegmentKind::Observed;
      segment.klass = run.klass;
      Granularity coarsest = Granularity::Queue;
      for (std::size_t position = run.begin; position <= run.end; ++position) {
        coarsest = coarser_of(coarsest, states[position].granularity);
      }
      if (coarsest == Granularity::Unknown) {
        coarsest = Granularity::Flow;
      }
      segment.granularity = coarsest;
      segment.subject = run.begin == run.end ? SubjectRef::hop(hops[run.begin].id)
                                             : SubjectRef::path(result.path);
    } else {
      segment.kind = SegmentKind::UnobservedSpan;
      segment.klass = LossClass::IncompleteEvidence;
      segment.granularity = Granularity::Unknown;
      segment.subject = SubjectRef::path(result.path);
      segment.reasons.push_back(ReasonCode::HopEvidenceAbsent);
    }
    result.segments.push_back(std::move(segment));
  }

  result.truncated = result.segments.size() > limit;
  if (result.truncated) {
    result.segments.resize(limit);
    result.bounds.push_back(BoundNote{BoundKind::ResultSet, static_cast<std::uint64_t>(limit),
                                      static_cast<std::uint64_t>(limit + 1), "localization-segments"});
    result.reasons.push_back(ReasonCode::ResultSetTruncated);
  }

  // ---- attribute observations to segments (each exactly once) -----------
  std::map<std::size_t, std::size_t> hop_position;
  for (std::size_t position = 0; position < hops.size(); ++position) {
    hop_position[hops[position].index] = position;
  }
  for (const Relevant& entry : relevant) {
    if (!entry.admissible ||
        !is_finer_than(entry.observation->granularity, Granularity::Path)) {
      continue;
    }
    std::size_t lowest = hops.size();
    for (const std::uint32_t index : entry.covered) {
      const auto position = hop_position.find(index);
      if (position != hop_position.end() && position->second < lowest) {
        lowest = position->second;
      }
    }
    if (lowest >= hops.size()) {
      continue;
    }
    for (LocalizedSegment& segment : result.segments) {
      if (!segment.hop_range_known) {
        continue;
      }
      if (hops[lowest].index < segment.first_hop || hops[lowest].index > segment.last_hop) {
        continue;
      }
      if (segment.kind == SegmentKind::UnobservedSpan) {
        break;
      }
      const auto before = segment.lost;
      const auto offered_before = segment.offered;
      segment.lost += entry.observation->lost;
      segment.offered += entry.observation->offered;
      if (segment.lost < before || segment.offered < offered_before) {
        segment.lost = std::numeric_limits<std::uint64_t>::max();
        segment.offered = std::numeric_limits<std::uint64_t>::max();
      }
      segment.ratio_defined = segment.ratio_defined || entry.observation->ratio_defined;
      ++segment.observation_count;
      if (segment.evidence_ids.size() < policy_.max_segments) {
        segment.evidence_ids.push_back(entry.observation->id);
      } else {
        segment.evidence_ids_truncated = true;
      }
      if (segment.granularity == Granularity::Unknown) {
        segment.granularity = entry.observation->granularity;
      }
      break;
    }
  }
  for (LocalizedSegment& segment : result.segments) {
    if (segment.kind == SegmentKind::UnobservedSpan) {
      continue;
    }
    if (segment.offered > 0) {
      std::uint32_t ratio = 0;
      if (checked::ratio_basis_points(segment.lost, segment.offered, ratio)) {
        segment.ratio_bp = ratio;
        segment.ratio_defined = true;
      }
    }
    achieved = finer_of(achieved, segment.granularity);
  }
  if (achieved == Granularity::Unknown) {
    achieved = Granularity::Flow;
  }

  result.achieved_granularity = coarser_of(achieved, coarser_of(cap, evidence_granularity));
  // Defensive invariant: a claim may never be finer than the evidence behind
  // it or than the caller's cap. If this ever triggers, the result is
  // downgraded rather than reported.
  if (is_finer_than(result.achieved_granularity, evidence_granularity) ||
      is_finer_than(result.achieved_granularity, request.requested_max_granularity)) {
    result.achieved_granularity = coarser_of(evidence_granularity, request.requested_max_granularity);
    result.reasons.push_back(ReasonCode::LocalizationClampedToEvidenceGranularity);
  }

  std::size_t unobserved = 0;
  bool multiple_candidates = false;
  for (const LocalizedSegment& segment : result.segments) {
    if (segment.kind == SegmentKind::UnobservedSpan) {
      unobserved += static_cast<std::size_t>(segment.last_hop - segment.first_hop + 1);
    }
  }
  {
    std::map<LossClass, std::size_t> class_counts;
    for (const LocalizedSegment& segment : result.segments) {
      if (segment.kind == SegmentKind::Observed) {
        ++class_counts[segment.klass];
      }
    }
    for (const auto& entry : class_counts) {
      if (entry.second > 1 && asserts_loss(entry.first)) {
        multiple_candidates = true;
      }
    }
  }

  result.ambiguous = unobserved > 0 || multiple_candidates ||
                     !is_finer_than(result.evidence_granularity, Granularity::Hop);
  if (unobserved > 0 && result.ambiguity.size() < policy_.max_ambiguity_notes) {
    AmbiguityNote note{};
    note.widest_possible = evidence_granularity;
    note.achieved = result.achieved_granularity;
    note.reason = ReasonCode::HopEvidenceAbsent;
    note.candidate_count = unobserved;
    note.detail = "hops without evidence are reported as unobserved, never as clean";
    result.ambiguity.push_back(std::move(note));
    result.reasons.push_back(ReasonCode::UnobservedPathSpan);
    result.reasons.push_back(ReasonCode::PartialHopCoverage);
  }
  if (!is_finer_than(result.evidence_granularity, Granularity::Hop) &&
      result.ambiguity.size() < policy_.max_ambiguity_notes) {
    AmbiguityNote note{};
    note.widest_possible = evidence_granularity;
    note.achieved = result.achieved_granularity;
    note.reason = ReasonCode::AmbiguityRetained;
    note.candidate_count = hops.size();
    note.detail =
        "the finest available evidence is coarser than a hop, so several path positions remain consistent";
    result.ambiguity.push_back(std::move(note));
  }
  if (multiple_candidates && result.ambiguity.size() < policy_.max_ambiguity_notes) {
    AmbiguityNote note{};
    note.widest_possible = Granularity::Queue;
    note.achieved = result.achieved_granularity;
    note.reason = ReasonCode::AmbiguityRetained;
    note.candidate_count = 0;
    note.detail = "more than one segment asserts loss; the result does not single out one location";
    result.ambiguity.push_back(std::move(note));
  }
  result.reasons.push_back(is_finer_than(result.achieved_granularity, Granularity::Flow)
                               ? ReasonCode::LocalizedToFinestSupportedGranularity
                               : ReasonCode::LocalizationClampedToEvidenceGranularity);
  std::sort(result.reasons.begin(), result.reasons.end(),
            [](ReasonCode lhs, ReasonCode rhs) { return static_cast<std::uint16_t>(lhs) < static_cast<std::uint16_t>(rhs); });
  result.reasons.erase(std::unique(result.reasons.begin(), result.reasons.end()), result.reasons.end());

  result.summary = result.to_string();
  return result;
}

}  // namespace loss_observatory
