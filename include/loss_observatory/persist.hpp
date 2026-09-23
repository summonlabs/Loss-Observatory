#pragma once

#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "loss_observatory/aggregate.hpp"
#include "loss_observatory/core/bytes.hpp"
#include "loss_observatory/episode.hpp"
#include "loss_observatory/evidence/store.hpp"
#include "loss_observatory/model/topology.hpp"

namespace loss_observatory {

/// Persisted record kinds. The numeric values are part of the on-disk format.
enum class RecordType : std::uint16_t {
  Manifest = 1,
  TopologyEntity = 2,
  SourceDescriptorRecord = 3,
  SourceIncarnationRecord = 4,
  EvidenceRecord = 5,
  EpisodeRecord = 6,
  SessionRecord = 7,
  CompactionMarker = 8,
};

[[nodiscard]] std::string_view to_string(RecordType type) noexcept;

struct PersistLimits {
  /// Journal size at which the engine should compact.
  std::uint64_t max_journal_bytes{64ULL * 1024ULL * 1024ULL};
  /// A single record payload larger than this is refused rather than written.
  std::size_t max_payload_bytes{1024ULL * 1024ULL};
  std::size_t max_records_loaded{2000000};
  std::size_t max_evidence_loaded{200000};
  std::size_t max_episodes_loaded{8192};
  std::size_t max_topology_loaded{16384};
  std::size_t max_sources_loaded{1024};
  bool compact_on_open{false};
};

struct PersistConfig {
  bool enabled{false};
  std::string directory{};
  std::string instance{"default"};
  PersistLimits limits{};
};

struct EngineEpoch {
  EpochId id{};
  Timestamp booted_at{};
  std::string incarnation{};
};

/// What recovery actually did. Conservative by construction: the reader stops
/// at the first damaged record and reports the truncation instead of attempting
/// to resynchronise and interpret whatever bytes follow.
struct RecoveryReport {
  bool opened{false};
  bool created{false};
  bool version_mismatch{false};
  bool semantics_mismatch{false};
  bool checksum_failure{false};
  bool truncated{false};
  std::uint64_t records_read{0};
  std::uint64_t records_accepted{0};
  std::uint64_t records_discarded{0};
  std::uint64_t bytes_read{0};
  std::uint64_t valid_bytes{0};
  std::uint64_t topology_restored{0};
  std::uint64_t sources_restored{0};
  std::uint64_t evidence_restored{0};
  std::uint64_t evidence_marked_not_current{0};
  std::uint64_t episodes_restored{0};
  std::uint64_t episodes_sealed{0};
  std::string detail{};

  [[nodiscard]] std::string to_string() const;
};

struct PersistStats {
  bool open{false};
  bool dirty{false};
  std::uint64_t records_written{0};
  std::uint64_t bytes_written{0};
  std::uint64_t evidence_persisted{0};
  std::uint64_t compactions{0};
  std::uint64_t checksum_failures{0};
  std::uint64_t discarded_records{0};
  std::uint64_t refused_payloads{0};
};

/// Append-only, CRC-32C-checked, versioned journal with conservative recovery.
///
/// Integrity and provenance are enforced on every read: a record whose CRC does
/// not match, whose declared length runs past the end of the file, or whose
/// format version differs is never interpreted. Dynamic evidence that is
/// successfully read back is flagged as recovered, which makes it ineligible as
/// proof of current loss.
class PersistenceStore {
 public:
  explicit PersistenceStore(PersistConfig config);
  ~PersistenceStore();

  PersistenceStore(const PersistenceStore&) = delete;
  PersistenceStore& operator=(const PersistenceStore&) = delete;

  /// Creates or opens the journal and appends a session record. Returns the new
  /// engine epoch, which is distinct on every run.
  Result<EngineEpoch> open_session(Timestamp now, std::string incarnation);

  /// Reads the journal back. Declared topology and source registration are
  /// restored as declarations; observations are restored as history and marked
  /// recovered; episodes that were open are sealed as ClosedByRestart.
  Result<RecoveryReport> load(TopologyRegistry& topology, SourceRegistry& sources, EvidenceStore& evidence,
                              EpisodeTracker& episodes, Timestamp now);

  Result<void> append_topology(const TopologyRegistry& topology);
  [[nodiscard]] Result<void> append_topology_entity(const Link& link);
  [[nodiscard]] Result<void> append_topology_entity(const Path& path);
  [[nodiscard]] Result<void> append_topology_entity(const QueueEntity& queue);
  [[nodiscard]] Result<void> append_topology_entity(const FlowBinding& flow);
  [[nodiscard]] Result<void> append_source(const SourceDescriptor& descriptor);
  [[nodiscard]] Result<void> append_incarnation(const SourceIncarnation& incarnation);
  [[nodiscard]] Result<void> append_evidence(const EvidenceItem& item);
  [[nodiscard]] Result<void> append_episode(const LossEpisode& episode);

  Result<void> flush();
  Result<void> close();

  /// Rewrites the journal from live state. Used to bound growth. The rewrite is
  /// staged in a temporary file and swapped in only after it is fully written
  /// and flushed.
  Result<void> compact(const TopologyRegistry& topology, const SourceRegistry& sources,
                       const EvidenceStore& evidence, const EpisodeTracker& episodes);

  [[nodiscard]] PersistStats stats() const;
  [[nodiscard]] RecoveryReport recovery() const;
  [[nodiscard]] const PersistConfig& config() const noexcept { return config_; }
  [[nodiscard]] const EngineEpoch& epoch() const noexcept { return epoch_; }
  [[nodiscard]] std::uint64_t journal_bytes() const;
  [[nodiscard]] bool should_compact() const;

 private:
  [[nodiscard]] Result<void> write_record_locked(RecordType type, ByteSpan payload);
  [[nodiscard]] Result<void> flush_locked();
  [[nodiscard]] Result<void> write_bytes_locked(ByteSpan bytes);
  [[nodiscard]] std::string journal_path() const;
  [[nodiscard]] std::string temp_path() const;

  mutable std::mutex mutex_{};
  PersistConfig config_{};
  PersistStats stats_{};
  RecoveryReport recovery_{};
  EngineEpoch epoch_{};
  std::unique_ptr<std::fstream> stream_{};
  bool manifest_written_{false};
};

/// Canonical binary encodings. Exposed so tests can prove round-trip stability
/// and so a downstream consumer can read the format without linking to the
/// store itself.
[[nodiscard]] std::vector<std::uint8_t> encode_evidence(const EvidenceItem& item);
[[nodiscard]] Result<EvidenceItem> decode_evidence(ByteSpan bytes);
[[nodiscard]] std::vector<std::uint8_t> encode_topology_entity(const Link& link);
[[nodiscard]] std::vector<std::uint8_t> encode_topology_entity(const Path& path);
[[nodiscard]] std::vector<std::uint8_t> encode_topology_entity(const QueueEntity& queue);
[[nodiscard]] std::vector<std::uint8_t> encode_topology_entity(const FlowBinding& flow);
[[nodiscard]] std::vector<std::uint8_t> encode_source(const SourceDescriptor& descriptor);
[[nodiscard]] std::vector<std::uint8_t> encode_incarnation(const SourceIncarnation& incarnation);
[[nodiscard]] std::vector<std::uint8_t> encode_episode(const LossEpisode& episode);

enum class TopologyEntityKind : std::uint8_t { Unknown = 0, Link = 1, Path = 2, Queue = 3, Flow = 4 };
[[nodiscard]] TopologyEntityKind topology_entity_kind(ByteSpan bytes);

}  // namespace loss_observatory
