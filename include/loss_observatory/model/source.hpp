#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include "loss_observatory/core/time.hpp"
#include "loss_observatory/model/granularity.hpp"
#include "loss_observatory/model/ids.hpp"

namespace loss_observatory {

/// Where a piece of evidence came from. Every observation in this runtime
/// carries a source, an incarnation, and a sequence: evidence without a
/// provenance chain is not admissible.
enum class SourceKind : std::uint8_t {
  Unknown = 0,
  CounterTelemetry = 1,
  ActiveProbe = 2,
  SequenceTracker = 3,
  EndpointComparison = 4,
  /// Produced by this repository's own deterministic generators. Always
  /// labelled synthetic; never presented as a hardware or fabric measurement.
  Synthetic = 5,
};

/// Total order over authority, used for deterministic conflict resolution.
/// Larger is more authoritative. Equality means "no winner can be picked".
enum class SourceAuthority : std::uint8_t {
  None = 0,
  Advisory = 1,
  Secondary = 2,
  Primary = 3,
};

[[nodiscard]] std::string_view to_string(SourceKind kind) noexcept;
[[nodiscard]] std::string_view to_string(SourceAuthority authority) noexcept;
[[nodiscard]] Result<SourceKind> parse_source_kind(std::string_view text);
[[nodiscard]] Result<SourceAuthority> parse_source_authority(std::string_view text);

inline constexpr std::size_t kMaxSourceNameBytes = 96;
inline constexpr std::size_t kMaxIncarnationNameBytes = 96;

struct SourceDescriptor {
  SourceId id{};
  std::string name{};
  SourceKind kind{SourceKind::Unknown};
  SourceAuthority authority{SourceAuthority::None};

  friend bool operator==(const SourceDescriptor& lhs, const SourceDescriptor& rhs) {
    return lhs.id == rhs.id && lhs.name == rhs.name && lhs.kind == rhs.kind &&
           lhs.authority == rhs.authority;
  }
  [[nodiscard]] std::string to_string() const;
};

/// One activation of a source. A restart, process restart, or agent replacement
/// produces a new epoch. Sequence numbers and counters are only comparable
/// within a single epoch and generation.
struct SourceIncarnation {
  SourceId source{};
  EpochId epoch{};
  std::string incarnation{};
  Timestamp activated_at{};
  bool retired{false};

  friend bool operator==(const SourceIncarnation& lhs, const SourceIncarnation& rhs) {
    return lhs.source == rhs.source && lhs.epoch == rhs.epoch && lhs.incarnation == rhs.incarnation &&
           lhs.activated_at == rhs.activated_at && lhs.retired == rhs.retired;
  }
  [[nodiscard]] std::string to_string() const;
};

enum class FenceOutcome : std::uint8_t {
  /// Sequence is newer than anything seen for this (source, epoch).
  Accepted = 0,
  /// Sequence is older than the high-water mark but was not seen before. The
  /// observation is retained as history and excluded from current-state
  /// reasoning unless a policy explicitly allows out-of-order assembly.
  Reordered,
  /// Sequence was already observed for this (source, epoch).
  Replayed,
  /// The source epoch is not the currently active incarnation.
  StaleEpoch,
  /// The source itself is unknown.
  UnknownSource,
  /// No incarnation was ever activated for this source.
  NoActiveIncarnation,
};

[[nodiscard]] std::string_view to_string(FenceOutcome outcome) noexcept;

struct FenceDecision {
  FenceOutcome outcome{FenceOutcome::UnknownSource};
  SequenceId high_water{};
  bool admissible{false};
  std::string detail{};
};

/// Bounded duplicate-detection window. Sequences older than the window that are
/// not in the seen set are classified as Reordered rather than Replayed: the
/// runtime does not guess that a very old sequence is a duplicate.
struct SequenceFenceConfig {
  std::size_t window{256};
};

/// Registry of sources, their incarnations, and their sequence fences.
class SourceRegistry {
 public:
  SourceRegistry() = default;
  SourceRegistry(const SourceRegistry&) = delete;
  SourceRegistry& operator=(const SourceRegistry&) = delete;

  [[nodiscard]] Result<UpsertOutcome> register_source(SourceDescriptor descriptor, bool allow_replace = false);

  /// Activates a new incarnation of p source, retiring any previous one. The
  /// previous epoch is never reused, so evidence fenced to it stays fenced.
  [[nodiscard]] Result<EpochId> activate_incarnation(SourceId source, std::string incarnation_name,
                                                     Timestamp activated_at);

  /// Restores an incarnation exactly as it was recorded, preserving its epoch.
  /// A restored incarnation is retired: it belongs to a previous process, and
  /// evidence citing it must never become current again.
  [[nodiscard]] Result<void> restore_incarnation(SourceIncarnation incarnation);

  /// Activates a specific, already-known epoch as the current incarnation,
  /// retiring the others. This is an explicit operator action used when
  /// re-establishing a source identity (for example when importing a declared
  /// export); it is never taken automatically, and evidence citing a retired
  /// epoch stays fenced.
  [[nodiscard]] Result<void> activate_incarnation_with_epoch(SourceId source, EpochId epoch,
                                                             std::string incarnation_name,
                                                             Timestamp activated_at);

  /// Marks the current incarnation retired; subsequent evidence for it is
  /// fenced as StaleEpoch.
  [[nodiscard]] Result<void> retire_incarnation(SourceId source, EpochId epoch);

  [[nodiscard]] Result<SourceDescriptor> find_source(SourceId id) const;
  [[nodiscard]] Result<SourceIncarnation> current_incarnation(SourceId id) const;
  [[nodiscard]] Result<SourceIncarnation> find_incarnation(SourceId id, EpochId epoch) const;
  [[nodiscard]] Result<SourceAuthority> authority_of(SourceId id) const;

  /// Applies the sequence fence. p admissibility is decided here and nowhere
  /// else, so no other component can quietly accept a replay.
  [[nodiscard]] Result<FenceDecision> observe_sequence(SourceId source, EpochId epoch, SequenceId sequence);

  [[nodiscard]] std::vector<SourceDescriptor> sources() const;
  [[nodiscard]] std::vector<SourceIncarnation> incarnations() const;

  [[nodiscard]] std::size_t source_count() const;
  [[nodiscard]] std::size_t max_sources() const noexcept { return max_sources_; }
  void set_max_sources(std::size_t value) noexcept { max_sources_ = value; }

  void set_sequence_window(std::size_t window);
  [[nodiscard]] std::size_t sequence_window() const;

 private:
  struct SequenceState {
    SequenceId high_water{};
    std::deque<SequenceId> seen{};
  };

  [[nodiscard]] FenceDecision apply_fence(SequenceState& state, SequenceId sequence) const;

  mutable std::shared_mutex mutex_{};
  std::map<SourceId, SourceDescriptor> sources_{};
  std::map<SourceId, std::vector<SourceIncarnation>> incarnations_{};
  std::map<std::pair<SourceId, std::uint64_t>, SequenceState> sequences_{};
  std::size_t max_sources_{256};
  std::size_t sequence_window_{256};
};

}  // namespace loss_observatory
