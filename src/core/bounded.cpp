#include "loss_observatory/core/bounded.hpp"

#include <algorithm>
#include <string>

namespace loss_observatory {

std::string_view to_string(BoundKind kind) noexcept {
  switch (kind) {
    case BoundKind::None:
      return "none";
    case BoundKind::QueueDepth:
      return "queue-depth";
    case BoundKind::EvidenceItems:
      return "evidence-items";
    case BoundKind::EvidencePerSubject:
      return "evidence-per-subject";
    case BoundKind::DistinctSubjects:
      return "distinct-subjects";
    case BoundKind::DistinctSources:
      return "distinct-sources";
    case BoundKind::TopologyEntities:
      return "topology-entities";
    case BoundKind::HistoryEpisodes:
      return "history-episodes";
    case BoundKind::AggregationWindows:
      return "aggregation-windows";
    case BoundKind::AggregationSources:
      return "aggregation-sources";
    case BoundKind::ResultSet:
      return "result-set";
    case BoundKind::PayloadBytes:
      return "payload-bytes";
    case BoundKind::MetadataBytes:
      return "metadata-bytes";
    case BoundKind::TextBytes:
      return "text-bytes";
    case BoundKind::PersistenceBytes:
      return "persistence-bytes";
    case BoundKind::PersistenceRecords:
      return "persistence-records";
    case BoundKind::OpenEpisodes:
      return "open-episodes";
    case BoundKind::ExplanationReasons:
      return "explanation-reasons";
    case BoundKind::ExplanationClaims:
      return "explanation-claims";
    case BoundKind::ExportItems:
      return "export-items";
  }
  return "none";
}

std::string_view to_string(PushOutcome outcome) noexcept {
  switch (outcome) {
    case PushOutcome::Accepted:
      return "accepted";
    case PushOutcome::RejectedFull:
      return "rejected-full";
    case PushOutcome::RejectedClosed:
      return "rejected-closed";
    case PushOutcome::RejectedInvalid:
      return "rejected-invalid";
  }
  return "rejected-invalid";
}

void BoundNotes::add(BoundKind kind, std::uint64_t limit, std::uint64_t observed, std::string subject,
                     std::size_t max_notes) {
  if (notes_.size() >= max_notes) {
    truncated_ = true;
    return;
  }
  for (const BoundNote& existing : notes_) {
    if (existing.kind == kind && existing.subject == subject) {
      return;
    }
  }
  BoundNote note{};
  note.kind = kind;
  note.limit = limit;
  note.observed = observed;
  note.subject = std::move(subject);
  notes_.push_back(std::move(note));
}

void BoundNotes::clear() noexcept {
  notes_.clear();
  truncated_ = false;
}

}  // namespace loss_observatory
