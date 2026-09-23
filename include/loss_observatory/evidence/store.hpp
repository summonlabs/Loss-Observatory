#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "loss_observatory/core/bounded.hpp"
#include "loss_observatory/evidence/derive.hpp"
#include "loss_observatory/model/source.hpp"
#include "loss_observatory/model/topology.hpp"

namespace loss_observatory {

struct StoreLimits {
  std::size_t max_items{200000};
  std::size_t max_items_per_subject{8192};
  std::size_t max_distinct_subjects{8192};
  std::size_t max_counter_series{4096};
  std::size_t max_points_per_series{4096};
  std::size_t max_measurement_ids_tracked{200000};
  std::size_t max_note_bytes{kMaxEvidenceNoteBytes};
  std::size_t max_snapshot_items{200000};
};

struct StoreStats {
  std::uint64_t accepted{0};
  std::uint64_t rejected_malformed{0};
  std::uint64_t rejected_unknown_source{0};
  std::uint64_t rejected_stale_epoch{0};
  std::uint64_t rejected_replay{0};
  std::uint64_t accepted_out_of_order{0};
  std::uint64_t rejected_duplicate{0};
  std::uint64_t rejected_unsupported_method{0};
  std::uint64_t rejected_granularity{0};
  std::uint64_t rejected_capacity{0};
  std::uint64_t evicted{0};
  std::uint64_t evicted_per_subject{0};
};

struct IngestOutcome {
  MeasurementId id{};
  bool accepted{false};
  bool out_of_order{false};
  RejectionReason reason{RejectionReason::None};
  FenceOutcome fence{FenceOutcome::UnknownSource};
  std::string detail{};

  [[nodiscard]] bool ok() const noexcept { return accepted; }
};

/// Bounded, fenced, provenance-checked evidence store.
///
/// Ingest is the only door into the runtime. Everything that enters has been
/// validated, attributed to a live source incarnation, sequence-fenced, and
/// de-duplicated. Anything rejected is reported with a reason, so a caller can
/// always tell "we have no evidence" from "we refused your evidence".
class EvidenceStore {
 public:
  EvidenceStore(SourceRegistry& sources, StoreLimits limits = {})
      : sources_(&sources), limits_(limits) {}

  EvidenceStore(const EvidenceStore&) = delete;
  EvidenceStore& operator=(const EvidenceStore&) = delete;

  [[nodiscard]] IngestOutcome submit(EvidenceItem item);

  /// Inserts an item recovered from persistence. Recovery bypasses the
  /// sequence fence, which belongs to a previous process epoch, but it does
  /// not bypass provenance: the item is marked recovered, so freshness can
  /// never treat it as current.
  [[nodiscard]] IngestOutcome restore(EvidenceItem item);

  /// Canonical-order copy of every retained item.
  [[nodiscard]] std::vector<EvidenceItem> snapshot() const;

  /// Canonical-order copy limited to one subject.
  [[nodiscard]] std::vector<EvidenceItem> snapshot_for(SubjectRef subject) const;

  [[nodiscard]] std::size_t item_count() const;
  [[nodiscard]] std::size_t subject_count() const;
  [[nodiscard]] StoreStats stats() const;
  [[nodiscard]] BoundNotes bound_notes() const;
  [[nodiscard]] const StoreLimits& limits() const noexcept { return limits_; }

  /// Marks every retained item as recovered-from-persistence and returns how
  /// many were marked. Used by the restart path: recovered evidence is
  /// preserved as history and is never admissible as current.
  std::size_t mark_all_recovered();

  void clear();

 private:
  struct SubjectBucket {
    std::deque<EvidenceItem> items{};
  };

  void record_bound_locked(BoundKind kind, std::uint64_t limit, std::uint64_t observed,
                           const std::string& subject);

  mutable std::mutex mutex_{};
  SourceRegistry* sources_{nullptr};
  StoreLimits limits_{};
  std::map<SubjectRef, SubjectBucket> subjects_{};
  std::set<MeasurementId> seen_ids_{};
  std::deque<MeasurementId> seen_order_{};
  /// Global insertion order, so the total-item bound can evict the oldest
  /// retained item across all subjects deterministically.
  std::deque<MeasurementId> insertion_order_{};
  StoreStats stats_{};
  mutable BoundNotes bounds_{};
};

}  // namespace loss_observatory
