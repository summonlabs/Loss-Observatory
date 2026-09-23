#include "loss_observatory/freshness.hpp"

#include <string>

namespace loss_observatory {

std::string_view to_string(Freshness state) noexcept {
  switch (state) {
    case Freshness::Fresh:
      return "fresh";
    case Freshness::Stale:
      return "stale";
    case Freshness::NotCurrent:
      return "not-current";
    case Freshness::FutureDated:
      return "future-dated";
    case Freshness::Undated:
      return "undated";
    case Freshness::Unknown:
      return "unknown";
  }
  return "unknown";
}

std::string_view to_string(StaleReason reason) noexcept {
  switch (reason) {
    case StaleReason::None:
      return "none";
    case StaleReason::AgeExceeded:
      return "age-exceeded";
    case StaleReason::EpochRetired:
      return "epoch-retired";
    case StaleReason::EpochUnknown:
      return "epoch-unknown";
    case StaleReason::GenerationSuperseded:
      return "generation-superseded";
    case StaleReason::GenerationUnknown:
      return "generation-unknown";
    case StaleReason::TopologyRevisionChanged:
      return "topology-revision-changed";
    case StaleReason::RecoveredFromPersistence:
      return "recovered-from-persistence";
    case StaleReason::SourceUnknown:
      return "source-unknown";
    case StaleReason::SourceIncarnationUnknown:
      return "source-incarnation-unknown";
    case StaleReason::FutureDated:
      return "future-dated";
    case StaleReason::Undated:
      return "undated";
    case StaleReason::ReceivedBeforeObserved:
      return "received-before-observed";
  }
  return "none";
}

std::string FreshnessAssessment::to_string() const {
  std::string result("freshness=");
  result.append(loss_observatory::to_string(state));
  result.append(" reason=");
  result.append(loss_observatory::to_string(reason));
  result.append(" age=");
  result.append(age.to_string());
  result.append(admissible ? " admissible=true" : " admissible=false");
  if (!detail.empty()) {
    result.append(" detail=");
    result.append(detail);
  }
  return result;
}

FreshnessAssessment assess_freshness(const LossObservation& observation, Timestamp now,
                                     const FreshnessPolicy& policy, const FreshnessContext& context) {
  FreshnessAssessment assessment{};

  // 1. Recovered evidence is never current, whatever its timestamps say. This
  //    is checked first so no later rule can promote it.
  if (observation.recovered_from_persistence) {
    assessment.state = Freshness::NotCurrent;
    assessment.reason = StaleReason::RecoveredFromPersistence;
    assessment.detail = "evidence was restored from persisted state during this boot";
    assessment.admissible = false;
    return assessment;
  }

  // 2. Missing instants.
  if (observation.observed_at.is_zero()) {
    assessment.state = Freshness::Undated;
    assessment.reason = StaleReason::Undated;
    assessment.detail = "evidence carries no observation instant";
    return assessment;
  }
  if (!observation.received_at.is_zero() &&
      observation.received_at.value + policy.clock_skew_tolerance < observation.observed_at.value) {
    assessment.state = Freshness::Stale;
    assessment.reason = StaleReason::ReceivedBeforeObserved;
    assessment.detail = "receive instant precedes the observation instant beyond the skew tolerance";
    return assessment;
  }

  const Duration age = now - observation.observed_at.value;
  assessment.age = age;

  // 3. Future-dated evidence. Normal clock skew inside the tolerance is
  //    expected; beyond it, the instant is not trustworthy unless the
  //    deployment has explicitly opted in.
  if (age.is_negative() && age.abs() > policy.future_tolerance) {
    if (!policy.allow_future_dated) {
      assessment.state = Freshness::FutureDated;
      assessment.reason = StaleReason::FutureDated;
      assessment.detail = "observation instant is ahead of local time beyond the skew tolerance";
      return assessment;
    }
    assessment.detail = "observation instant is ahead of local time but future dating is permitted";
  }

  // 4. Age.
  if (age.nanos() > policy.max_age.nanos()) {
    assessment.state = Freshness::Stale;
    assessment.reason = StaleReason::AgeExceeded;
    assessment.detail = "evidence is older than the configured maximum age";
    return assessment;
  }

  // 5. Source and incarnation.
  if (context.sources != nullptr) {
    const Result<SourceDescriptor> descriptor = context.sources->find_source(observation.source);
    if (!descriptor.ok()) {
      assessment.state = Freshness::Unknown;
      assessment.reason = StaleReason::SourceUnknown;
      assessment.detail = "source is not registered";
      return assessment;
    }
    if (!policy.ignore_retired_epoch) {
      const Result<SourceIncarnation> incarnation =
          context.sources->find_incarnation(observation.source, observation.epoch);
      if (!incarnation.ok()) {
        assessment.state = Freshness::Stale;
        assessment.reason = StaleReason::EpochUnknown;
        assessment.detail = "evidence cites an epoch this source never activated";
        return assessment;
      }
      if (incarnation.value().retired) {
        assessment.state = Freshness::Stale;
        assessment.reason = StaleReason::EpochRetired;
        assessment.detail = "evidence cites a retired source incarnation";
        return assessment;
      }
    }
  }

  // 6. Generation correlation.
  if (context.generations != nullptr && !policy.ignore_generation_supersession) {
    GenerationStatus status{};
    bool correlated = false;
    switch (observation.subject.kind()) {
      case SubjectKind::Flow: {
        status = context.generations->correlate_flow(FlowId::from_value(observation.subject.raw_id()),
                                                    observation.generation, observation.observed_at.value);
        correlated = true;
        break;
      }
      case SubjectKind::Path: {
        status = context.generations->correlate_path(PathId::from_value(observation.subject.raw_id()),
                                                    observation.generation, observation.observed_at.value);
        correlated = true;
        break;
      }
      default:
        break;
    }
    if (correlated) {
      if (status.match == GenerationMatch::Superseded) {
        assessment.state = Freshness::Stale;
        assessment.reason = StaleReason::GenerationSuperseded;
        assessment.detail = "a newer generation is current for this entity";
        return assessment;
      }
      if (status.match == GenerationMatch::UnknownGeneration) {
        assessment.state = Freshness::Stale;
        assessment.reason = StaleReason::GenerationUnknown;
        assessment.detail = "no generation record covers the observation instant";
        return assessment;
      }
    }
  }

  // 7. Topology revision correlation.
  if (context.topology != nullptr && !policy.ignore_topology_revision) {
    if (observation.subject.kind() == SubjectKind::Flow) {
      const Result<FlowBinding> flow =
          context.topology->find_flow(FlowId::from_value(observation.subject.raw_id()));
      if (flow.ok() && !observation.topology_revision.is_nil()) {
        if (flow.value().binding_revision != observation.topology_revision) {
          assessment.state = Freshness::Stale;
          assessment.reason = StaleReason::TopologyRevisionChanged;
          assessment.detail = "the flow binding was revised after this evidence was recorded";
          return assessment;
        }
        // The path a flow runs over is part of what the evidence observed: a
        // changed path shape invalidates evidence about the old shape even when
        // the binding itself was not re-declared.
        if (!flow.value().path.is_nil()) {
          const Result<Path> declared = context.topology->find_path(flow.value().path);
          if (declared.ok() && declared.value().revision != observation.topology_revision) {
            assessment.state = Freshness::Stale;
            assessment.reason = StaleReason::TopologyRevisionChanged;
            assessment.detail = "the declared path changed after this evidence was recorded";
            return assessment;
          }
        }
      }
    } else if (observation.subject.kind() == SubjectKind::Path) {
      const Result<Path> path = context.topology->find_path(PathId::from_value(observation.subject.raw_id()));
      if (path.ok() && !observation.topology_revision.is_nil() &&
          path.value().revision != observation.topology_revision) {
        assessment.state = Freshness::Stale;
        assessment.reason = StaleReason::TopologyRevisionChanged;
        assessment.detail = "the declared path changed after this evidence was recorded";
        return assessment;
      }
    }
  }

  assessment.state = Freshness::Fresh;
  assessment.reason = StaleReason::None;
  assessment.admissible = true;
  if (assessment.detail.empty()) {
    assessment.detail = "fresh and admissible";
  }
  return assessment;
}

std::string FreshnessSummary::to_string() const {
  std::string result("freshness[total=");
  result.append(std::to_string(total));
  result.append(" fresh=");
  result.append(std::to_string(fresh));
  result.append(" stale=");
  result.append(std::to_string(stale));
  result.append(" not-current=");
  result.append(std::to_string(not_current));
  result.append(" future-dated=");
  result.append(std::to_string(future_dated));
  result.append(" undated=");
  result.append(std::to_string(undated));
  result.append(" unknown=");
  result.append(std::to_string(unknown));
  result.push_back(']');
  return result;
}

FreshnessSummary summarize(std::span<const FreshnessAssessment> assessments) {
  FreshnessSummary summary{};
  summary.total = assessments.size();
  for (const FreshnessAssessment& assessment : assessments) {
    switch (assessment.state) {
      case Freshness::Fresh:
        ++summary.fresh;
        break;
      case Freshness::Stale:
        ++summary.stale;
        break;
      case Freshness::NotCurrent:
        ++summary.not_current;
        break;
      case Freshness::FutureDated:
        ++summary.future_dated;
        break;
      case Freshness::Undated:
        ++summary.undated;
        break;
      case Freshness::Unknown:
        ++summary.unknown;
        break;
    }
  }
  return summary;
}

}  // namespace loss_observatory
