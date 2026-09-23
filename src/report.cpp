#include "loss_observatory/report.hpp"

#include <algorithm>

#include "loss_observatory/version.hpp"
#include <string>
#include <vector>

namespace loss_observatory {
namespace {

void append_reason_lines(std::string& out, const std::vector<ReasonCode>& reasons, std::string_view indent) {
  for (const ReasonCode reason : reasons) {
    out.append(indent);
    out.append("- ");
    out.append(loss_observatory::to_string(reason));
    out.push_back('\n');
  }
}

void append_bound_lines(std::string& out, const std::vector<BoundNote>& bounds, std::string_view indent) {
  for (const BoundNote& note : bounds) {
    out.append(indent);
    out.append("- ");
    out.append(loss_observatory::to_string(note.kind));
    out.append(" limit=");
    out.append(std::to_string(note.limit));
    out.append(" observed=");
    out.append(std::to_string(note.observed));
    if (!note.subject.empty()) {
      out.append(" subject=");
      out.append(note.subject);
    }
    out.push_back('\n');
  }
}

}  // namespace

std::string_view to_string(ExportFormat format) noexcept {
  switch (format) {
    case ExportFormat::ScriptText:
      return "script-text";
    case ExportFormat::Report:
      return "report";
    case ExportFormat::Binary:
      return "binary";
  }
  return "script-text";
}

Result<ExportFormat> parse_export_format(std::string_view text) {
  if (text == "script-text" || text == "script" || text == "text") {
    return ExportFormat::ScriptText;
  }
  if (text == "report") {
    return ExportFormat::Report;
  }
  if (text == "binary") {
    return ExportFormat::Binary;
  }
  return make_status(StatusCode::InvalidArgument, "unknown export format");
}

std::string_view to_string(ExportScope scope) noexcept {
  switch (scope) {
    case ExportScope::Topology:
      return "topology";
    case ExportScope::Sources:
      return "sources";
    case ExportScope::Evidence:
      return "evidence";
    case ExportScope::Episodes:
      return "episodes";
    case ExportScope::Aggregates:
      return "aggregates";
    case ExportScope::All:
      return "all";
  }
  return "all";
}

Result<ExportScope> parse_export_scope(std::string_view text) {
  if (text == "topology") {
    return ExportScope::Topology;
  }
  if (text == "sources") {
    return ExportScope::Sources;
  }
  if (text == "evidence") {
    return ExportScope::Evidence;
  }
  if (text == "episodes") {
    return ExportScope::Episodes;
  }
  if (text == "aggregates") {
    return ExportScope::Aggregates;
  }
  if (text == "all") {
    return ExportScope::All;
  }
  return make_status(StatusCode::InvalidArgument, "unknown export scope");
}

std::string ExportBundle::to_string() const {
  std::string result = "export scope=";
  result.append(loss_observatory::to_string(scope));
  result.append(" format=");
  result.append(loss_observatory::to_string(format));
  result.append(" items=");
  result.append(std::to_string(item_count));
  result.append(truncated ? " truncated=true" : " truncated=false");
  result.append(" bytes=");
  result.append(std::to_string(format == ExportFormat::Binary ? binary.size() : text.size()));
  return result;
}

std::string render_evidence(const EvidenceItem& item) { return describe(item); }

std::string render_topology(const TopologyRegistry& topology) { return topology.canonical_dump(); }

std::string render_sources(const SourceRegistry& sources) {
  std::string result;
  for (const SourceDescriptor& descriptor : sources.sources()) {
    result.append(descriptor.to_string());
    result.push_back('\n');
  }
  for (const SourceIncarnation& incarnation : sources.incarnations()) {
    result.append(incarnation.to_string());
    result.push_back('\n');
  }
  return result;
}

std::string render_episode(const LossEpisode& episode) { return episode.to_string(); }

std::string render_aggregate(const AggregateBucket& bucket) { return bucket.to_string(); }

std::string render_claim(const SourceClaim& claim) { return claim.to_string(); }

std::string render_attribution(const Attribution& attribution) {
  std::string result("attribution band=");
  result.append(loss_observatory::to_string(attribution.band));
  result.append(" score=");
  result.append(std::to_string(attribution.score));
  result.push_back('\n');
  for (const ConfidenceTerm& term : attribution.terms) {
    result.append("  term ");
    if (term.points >= 0) {
      result.push_back('+');
    }
    result.append(std::to_string(term.points));
    result.push_back(' ');
    result.append(loss_observatory::to_string(term.code));
    if (!term.detail.empty()) {
      result.append(" (");
      result.append(term.detail);
      result.push_back(')');
    }
    result.push_back('\n');
  }
  return result;
}

std::string render_classification(const Classification& classification) {
  std::string result("subject=");
  result.append(classification.subject.to_string());
  result.append(" class=");
  result.append(loss_observatory::to_string(classification.klass));
  result.append(" lost=");
  result.append(std::to_string(classification.lost_total));
  result.append(" offered=");
  result.append(std::to_string(classification.offered_total));
  result.append(" ratio_bp=");
  result.append(classification.ratio_defined ? std::to_string(classification.ratio_bp)
                                             : std::string("undefined"));
  result.append(" confidence=");
  result.append(loss_observatory::to_string(classification.attribution.band));
  result.append(" score=");
  result.append(std::to_string(classification.attribution.score));
  return result;
}

std::string render_localization(const LocalizationResult& result) {
  std::string out = result.to_string();
  out.push_back('\n');
  for (const LocalizedSegment& segment : result.segments) {
    out.append("  ");
    out.append(segment.to_string());
    out.push_back('\n');
  }
  for (const AmbiguityNote& note : result.ambiguity) {
    out.append("  ");
    out.append(note.to_string());
    out.push_back('\n');
  }
  return out;
}

std::string render_explanation(const ExplainRequest& request, const Explanation& explanation) {
  std::string out;
  out.append(kProductName);
  out.append(" explanation\n");
  out.append("  subject        : ");
  out.append(explanation.subject.to_string());
  out.push_back('\n');
  out.append("  evaluated at   : ");
  out.append(explanation.at.to_string());
  out.push_back('\n');
  out.append("  class          : ");
  out.append(loss_observatory::to_string(explanation.klass));
  out.push_back('\n');
  out.append("  summary        : ");
  out.append(explanation.classification.to_string());
  out.push_back('\n');
  out.append("  freshness      : ");
  out.append(explanation.classification.freshness.to_string());
  out.push_back('\n');
  out.append("  confidence     : ");
  out.append(loss_observatory::to_string(explanation.classification.attribution.band));
  out.append(" (score ");
  out.append(std::to_string(explanation.classification.attribution.score));
  out.append(")\n");

  out.append("  reasons        :\n");
  if (explanation.classification.reasons.empty()) {
    out.append("    - none\n");
  } else {
    append_reason_lines(out, explanation.classification.reasons, "    ");
  }

  out.append("  attribution    :\n");
  for (const ConfidenceTerm& term : explanation.classification.attribution.terms) {
    out.append("    ");
    if (term.points >= 0) {
      out.push_back('+');
    }
    out.append(std::to_string(term.points));
    out.push_back(' ');
    out.append(loss_observatory::to_string(term.code));
    if (!term.detail.empty()) {
      out.append(" (");
      out.append(term.detail);
      out.push_back(')');
    }
    out.push_back('\n');
  }

  if (request.include_claims) {
    out.append("  claims         :\n");
    if (explanation.classification.claims.empty()) {
      out.append("    - none\n");
    }
    for (const SourceClaim& claim : explanation.classification.claims) {
      out.append("    - ");
      out.append(claim.to_string());
      out.push_back('\n');
    }
  }

  if (request.include_localization) {
    out.append("  localization   :\n");
    out.append("    ");
    out.append(explanation.localization.to_string());
    out.push_back('\n');
    for (const LocalizedSegment& segment : explanation.localization.segments) {
      out.append("    ");
      out.append(segment.to_string());
      out.push_back('\n');
    }
    for (const AmbiguityNote& note : explanation.localization.ambiguity) {
      out.append("    ");
      out.append(note.to_string());
      out.push_back('\n');
    }
  }

  if (request.include_history) {
    out.append("  history        : ");
    out.append(explanation.history.to_string());
    out.push_back('\n');
    for (const LossEpisode& episode : explanation.history.episodes) {
      out.append("    ");
      out.append(episode.to_string());
      out.push_back('\n');
    }
  }

  if (request.include_evidence_list) {
    out.append("  evidence ids   : ");
    out.append(std::to_string(explanation.evidence_ids.size()));
    if (explanation.classification.evidence_ids_truncated) {
      out.append(" (truncated)");
    }
    out.push_back('\n');
  }

  if (!explanation.aggregates.empty()) {
    out.append("  aggregates     :\n");
    for (const AggregateBucket& bucket : explanation.aggregates) {
      out.append("    ");
      out.append(bucket.to_string());
      out.push_back('\n');
    }
  }

  if (!explanation.bounds.empty()) {
    out.append("  bounds         :\n");
    append_bound_lines(out, explanation.bounds, "    ");
  }

  if (!explanation.rejections.empty()) {
    out.append("  rejections     :\n");
    for (const RejectionNote& note : explanation.rejections) {
      out.append("    - id=");
      out.append(note.id.to_string());
      out.append(" source=");
      out.append(note.source.to_string());
      out.append(" reason=");
      out.append(loss_observatory::to_string(note.reason));
      if (!note.detail.empty()) {
        out.append(" detail=");
        out.append(note.detail);
      }
      out.push_back('\n');
    }
  }

  if (explanation.truncated) {
    out.append("  note           : output truncated at the configured line bound\n");
  }
  return out;
}

}  // namespace loss_observatory
