#include "loss_observatory/evidence/store.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace loss_observatory {

IngestOutcome EvidenceStore::submit(EvidenceItem item) {
  IngestOutcome outcome{};
  outcome.id = item.header.id;

  if (item.header.note.size() > limits_.max_note_bytes) {
    outcome.reason = RejectionReason::NoteTooLong;
    outcome.detail = "evidence note exceeds the configured bound";
    std::lock_guard<std::mutex> guard(mutex_);
    ++stats_.rejected_malformed;
    return outcome;
  }

  const Status structural = validate(item);
  if (!structural.ok()) {
    outcome.reason = structural.code() == StatusCode::Unsupported ? RejectionReason::UnsupportedMethod
                                                                 : RejectionReason::Malformed;
    outcome.detail = structural.message();
    std::lock_guard<std::mutex> guard(mutex_);
    if (outcome.reason == RejectionReason::UnsupportedMethod) {
      ++stats_.rejected_unsupported_method;
    } else {
      ++stats_.rejected_malformed;
    }
    return outcome;
  }

  if (!semantics_of(item.header.method).implemented) {
    outcome.reason = RejectionReason::UnsupportedMethod;
    outcome.detail = std::string(method_unusable_reason(item.header.method, true));
    std::lock_guard<std::mutex> guard(mutex_);
    ++stats_.rejected_unsupported_method;
    return outcome;
  }

  // Sequence and epoch fencing happen before storage and nowhere else, so no
  // other component can admit a replay.
  const Result<FenceDecision> decision =
      sources_->observe_sequence(item.header.source, item.header.epoch, item.header.source_sequence);
  if (!decision.ok()) {
    outcome.reason = RejectionReason::UnknownSource;
    outcome.detail = decision.status().message();
    std::lock_guard<std::mutex> guard(mutex_);
    ++stats_.rejected_unknown_source;
    return outcome;
  }
  outcome.fence = decision.value().outcome;
  switch (decision.value().outcome) {
    case FenceOutcome::Replayed:
      outcome.reason = RejectionReason::ReplayedSequence;
      outcome.detail = decision.value().detail.empty() ? "sequence replay" : decision.value().detail;
      {
        std::lock_guard<std::mutex> guard(mutex_);
        ++stats_.rejected_replay;
      }
      return outcome;
    case FenceOutcome::StaleEpoch:
      outcome.reason = RejectionReason::StaleEpoch;
      outcome.detail = decision.value().detail;
      {
        std::lock_guard<std::mutex> guard(mutex_);
        ++stats_.rejected_stale_epoch;
      }
      return outcome;
    case FenceOutcome::UnknownSource:
      outcome.reason = RejectionReason::UnknownSource;
      outcome.detail = decision.value().detail;
      {
        std::lock_guard<std::mutex> guard(mutex_);
        ++stats_.rejected_unknown_source;
      }
      return outcome;
    case FenceOutcome::NoActiveIncarnation:
      outcome.reason = RejectionReason::UnknownEpoch;
      outcome.detail = decision.value().detail;
      {
        std::lock_guard<std::mutex> guard(mutex_);
        ++stats_.rejected_stale_epoch;
      }
      return outcome;
    case FenceOutcome::Reordered:
      outcome.out_of_order = true;
      break;
    case FenceOutcome::Accepted:
      break;
  }

  {
    std::lock_guard<std::mutex> guard(mutex_);
    if (seen_ids_.find(item.header.id) != seen_ids_.end()) {
      ++stats_.rejected_duplicate;
      outcome.reason = RejectionReason::DuplicateMeasurement;
      outcome.detail = "measurement identity was already ingested";
      return outcome;
    }

    const SubjectRef subject = item.header.subject;
    const bool new_subject = subjects_.find(subject) == subjects_.end();
    if (new_subject && subjects_.size() >= limits_.max_distinct_subjects) {
      ++stats_.rejected_capacity;
      record_bound_locked(BoundKind::DistinctSubjects,
                          static_cast<std::uint64_t>(limits_.max_distinct_subjects),
                          static_cast<std::uint64_t>(subjects_.size() + 1), subject.to_string());
      outcome.reason = RejectionReason::StoreCapacity;
      outcome.detail = "distinct subject limit reached";
      return outcome;
    }

    SubjectBucket& bucket = subjects_[subject];
    if (bucket.items.size() >= limits_.max_items_per_subject) {
      bucket.items.pop_front();
      insertion_order_.pop_front();
      ++stats_.evicted;
      ++stats_.evicted_per_subject;
      record_bound_locked(BoundKind::EvidencePerSubject,
                          static_cast<std::uint64_t>(limits_.max_items_per_subject),
                          static_cast<std::uint64_t>(limits_.max_items_per_subject + 1),
                          subject.to_string());
    }
    bucket.items.push_back(item);
    insertion_order_.push_back(item.header.id);
    seen_ids_.insert(item.header.id);
    seen_order_.push_back(item.header.id);
    while (seen_order_.size() > limits_.max_measurement_ids_tracked) {
      seen_ids_.erase(seen_order_.front());
      seen_order_.pop_front();
    }
    while (insertion_order_.size() > limits_.max_items) {
      const MeasurementId victim = insertion_order_.front();
      insertion_order_.pop_front();
      for (auto& entry : subjects_) {
        auto it = std::find_if(entry.second.items.begin(), entry.second.items.end(),
                               [victim](const EvidenceItem& candidate) {
                                 return candidate.header.id == victim;
                               });
        if (it != entry.second.items.end()) {
          entry.second.items.erase(it);
          ++stats_.evicted;
          record_bound_locked(BoundKind::EvidenceItems,
                              static_cast<std::uint64_t>(limits_.max_items),
                              static_cast<std::uint64_t>(limits_.max_items + 1), entry.first.to_string());
          break;
        }
      }
    }

    ++stats_.accepted;
    if (outcome.out_of_order) {
      ++stats_.accepted_out_of_order;
    }
  }
  outcome.accepted = true;
  outcome.reason = RejectionReason::None;
  return outcome;
}

IngestOutcome EvidenceStore::restore(EvidenceItem item) {
  IngestOutcome outcome{};
  outcome.id = item.header.id;
  item.header.recovered_from_persistence = true;

  if (item.header.note.size() > limits_.max_note_bytes) {
    outcome.reason = RejectionReason::NoteTooLong;
    outcome.detail = "recovered evidence note exceeds the configured bound";
    std::lock_guard<std::mutex> guard(mutex_);
    ++stats_.rejected_malformed;
    return outcome;
  }
  const Status structural = validate(item);
  if (!structural.ok()) {
    outcome.reason = RejectionReason::Malformed;
    outcome.detail = structural.message();
    std::lock_guard<std::mutex> guard(mutex_);
    ++stats_.rejected_malformed;
    return outcome;
  }

  // The sequence fence belongs to a previous process epoch and is deliberately
  // not applied here: the point of recovery is to preserve history, and the
  // recovered flag is what keeps that history out of current reasoning.
  std::lock_guard<std::mutex> guard(mutex_);
  if (seen_ids_.find(item.header.id) != seen_ids_.end()) {
    outcome.reason = RejectionReason::DuplicateMeasurement;
    outcome.detail = "recovered measurement was already present";
    return outcome;
  }
  const SubjectRef subject = item.header.subject;
  const bool new_subject = subjects_.find(subject) == subjects_.end();
  if (new_subject && subjects_.size() >= limits_.max_distinct_subjects) {
    outcome.reason = RejectionReason::StoreCapacity;
    outcome.detail = "distinct subject limit reached during recovery";
    ++stats_.rejected_capacity;
    return outcome;
  }
  SubjectBucket& bucket = subjects_[subject];
  if (bucket.items.size() >= limits_.max_items_per_subject) {
    bucket.items.pop_front();
    insertion_order_.pop_front();
    ++stats_.evicted;
    ++stats_.evicted_per_subject;
    record_bound_locked(BoundKind::EvidencePerSubject,
                        static_cast<std::uint64_t>(limits_.max_items_per_subject),
                        static_cast<std::uint64_t>(limits_.max_items_per_subject + 1),
                        subject.to_string());
  }
  bucket.items.push_back(item);
  insertion_order_.push_back(item.header.id);
  seen_ids_.insert(item.header.id);
  seen_order_.push_back(item.header.id);
  while (seen_order_.size() > limits_.max_measurement_ids_tracked) {
    seen_ids_.erase(seen_order_.front());
    seen_order_.pop_front();
  }
  while (insertion_order_.size() > limits_.max_items) {
    const MeasurementId victim = insertion_order_.front();
    insertion_order_.pop_front();
    for (auto& entry : subjects_) {
      auto it = std::find_if(entry.second.items.begin(), entry.second.items.end(),
                             [victim](const EvidenceItem& candidate) { return candidate.header.id == victim; });
      if (it != entry.second.items.end()) {
        entry.second.items.erase(it);
        ++stats_.evicted;
        record_bound_locked(BoundKind::EvidenceItems, static_cast<std::uint64_t>(limits_.max_items),
                            static_cast<std::uint64_t>(limits_.max_items + 1), entry.first.to_string());
        break;
      }
    }
  }
  ++stats_.accepted;
  outcome.accepted = true;
  return outcome;
}

void EvidenceStore::record_bound_locked(BoundKind kind, std::uint64_t limit, std::uint64_t observed,
                                        const std::string& subject) {
  bounds_.add(kind, limit, observed, subject);
}

std::vector<EvidenceItem> EvidenceStore::snapshot() const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<EvidenceItem> result;
  result.reserve(std::min<std::size_t>(insertion_order_.size(), limits_.max_snapshot_items));
  for (const auto& entry : subjects_) {
    for (const EvidenceItem& item : entry.second.items) {
      if (result.size() >= limits_.max_snapshot_items) {
        break;
      }
      result.push_back(item);
    }
  }
  if (result.size() >= limits_.max_snapshot_items && insertion_order_.size() > limits_.max_snapshot_items) {
    bounds_.add(BoundKind::EvidenceItems, static_cast<std::uint64_t>(limits_.max_snapshot_items),
                static_cast<std::uint64_t>(insertion_order_.size()), "snapshot");
  }
  return canonical_order(std::move(result));
}

std::vector<EvidenceItem> EvidenceStore::snapshot_for(SubjectRef subject) const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<EvidenceItem> result;
  const auto it = subjects_.find(subject);
  if (it == subjects_.end()) {
    return result;
  }
  result.reserve(it->second.items.size());
  for (const EvidenceItem& item : it->second.items) {
    result.push_back(item);
  }
  return canonical_order(std::move(result));
}

std::size_t EvidenceStore::item_count() const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::size_t total = 0;
  for (const auto& entry : subjects_) {
    total += entry.second.items.size();
  }
  return total;
}

std::size_t EvidenceStore::subject_count() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return subjects_.size();
}

StoreStats EvidenceStore::stats() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return stats_;
}

BoundNotes EvidenceStore::bound_notes() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return bounds_;
}

std::size_t EvidenceStore::mark_all_recovered() {
  std::lock_guard<std::mutex> guard(mutex_);
  std::size_t marked = 0;
  for (auto& entry : subjects_) {
    for (EvidenceItem& item : entry.second.items) {
      if (!item.header.recovered_from_persistence) {
        item.header.recovered_from_persistence = true;
      }
      ++marked;
    }
  }
  return marked;
}

void EvidenceStore::clear() {
  std::lock_guard<std::mutex> guard(mutex_);
  subjects_.clear();
  seen_ids_.clear();
  seen_order_.clear();
  insertion_order_.clear();
}

}  // namespace loss_observatory
