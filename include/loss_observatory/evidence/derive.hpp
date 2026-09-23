#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "loss_observatory/core/bounded.hpp"
#include "loss_observatory/evidence/counter.hpp"
#include "loss_observatory/evidence/evidence.hpp"
#include "loss_observatory/model/generation.hpp"
#include "loss_observatory/model/source.hpp"
#include "loss_observatory/model/topology.hpp"

namespace loss_observatory {

/// How a normalized observation relates loss to a denominator.
enum class LossSemantics : std::uint8_t {
  /// The number reported is itself loss (a drop counter). Magnitude is known;
  /// the offered denominator is not, and this runtime refuses to invent one.
  DirectLoss = 0,
  /// A lost/offered pair from which a ratio is computed.
  RatioLoss,
  /// A throughput observation. A lone receive or transmit counter says nothing
  /// about loss: there is no expectation to compare against.
  NonLoss,
};

[[nodiscard]] std::string_view to_string(LossSemantics semantics) noexcept;

/// Whether an observation is admissible for current-state reasoning.
enum class ObservationValidity : std::uint8_t {
  Valid = 0,
  /// Structurally sound but represents a discontinuity: not loss.
  Discontinuity,
  /// Physically implausible (received more than sent, non-monotonic sequence).
  Implausible,
  /// Needed inputs are missing: no baseline, unpaired endpoint report.
  Insufficient,
  /// Ratio cannot be formed because no denominator exists.
  UndefinedRatio,
  /// The method is declared but not implemented by this runtime.
  UnsupportedMethod,
  /// The method's preconditions (for example a synchronised clock) are unmet.
  UnsupportedPrecondition,
};

[[nodiscard]] std::string_view to_string(ObservationValidity validity) noexcept;

enum class DiscontinuityKind : std::uint8_t {
  None = 0,
  CounterWrap,
  CounterReset,
  DiscontinuityUnresolved,
  ImplausibleDelta,
  SequenceRestart,
  EpochChange,
  GenerationChange,
  TopologyRevisionChange,
  OutOfOrder,
  MissingBaseline,
};

[[nodiscard]] std::string_view to_string(DiscontinuityKind kind) noexcept;
[[nodiscard]] bool is_discontinuity(DiscontinuityKind kind) noexcept;

/// Normalized, provenance-complete statement about loss for one subject.
///
/// Everything downstream (freshness, conflict, classification, localization,
/// episodes, export) consumes this type and never the raw payloads, so the
/// "what does this number actually mean" question is answered exactly once.
struct LossObservation {
  MeasurementId id{};
  MeasurementId evidence_id{};
  SourceId source{};
  EpochId epoch{};
  GenerationId generation{};
  RevisionId topology_revision{};
  SequenceId source_sequence{};
  ObservedAt observed_at{};
  ReceivedAt received_at{};
  SubjectRef subject{};
  Granularity granularity{Granularity::Unknown};
  MeasurementMethod method{MeasurementMethod::Unknown};
  LossSemantics semantics{LossSemantics::NonLoss};
  ObservationValidity validity{ObservationValidity::Valid};
  DiscontinuityKind discontinuity{DiscontinuityKind::None};
  std::uint64_t lost{0};
  std::uint64_t offered{0};
  bool offered_known{false};
  bool ratio_defined{false};
  std::uint32_t ratio_bp{0};
  bool synthetic{false};
  /// Propagated from the originating evidence item. Recovered observations are
  /// reported and retained, and are never treated as current.
  bool recovered_from_persistence{false};
  std::string detail{};

  [[nodiscard]] bool usable() const noexcept { return validity == ObservationValidity::Valid; }
  [[nodiscard]] std::string to_string() const;
};

enum class RejectionReason : std::uint8_t {
  None = 0,
  Malformed,
  PayloadTooLarge,
  NoteTooLong,
  UnknownSource,
  UnknownEpoch,
  StaleEpoch,
  ReplayedSequence,
  ReorderedSequence,
  DuplicateMeasurement,
  UnsupportedMethod,
  GranularityExceedsMethod,
  GranularityUnknown,
  TopologyUnknown,
  GenerationUnknown,
  StoreCapacity,
  Cancelled,
};

[[nodiscard]] std::string_view to_string(RejectionReason reason) noexcept;

struct RejectionNote {
  MeasurementId id{};
  SourceId source{};
  RejectionReason reason{RejectionReason::None};
  std::string detail{};
};

struct DerivePolicy {
  CounterPolicy counter{};
  /// Two endpoint reports may be paired only if their observation times are
  /// within this distance. Otherwise the comparison is Insufficient.
  Duration endpoint_pair_window = Duration::from_millis(1000);
  /// Maximum length of a path walk performed while binding evidence to hops.
  std::size_t max_path_hops{256};
  /// Upper bound on observations produced in a single derivation pass.
  std::size_t max_observations{65536};
  /// When true, evidence about a topology entity that was never declared is
  /// rejected with an explicit note. Topology is declared, never inferred, so
  /// the default is true: evidence cannot invent the thing it describes.
  bool require_declared_topology{true};
};

struct DerivationResult {
  std::vector<LossObservation> observations{};
  std::vector<RejectionNote> rejections{};
  BoundNotes bounds{};
  std::size_t counter_series_examined{0};
  std::size_t endpoint_reports_examined{0};
  std::size_t probe_reports_examined{0};
  std::size_t sequence_reports_examined{0};
};

struct DerivationContext {
  const TopologyRegistry* topology{nullptr};
  const GenerationRegistry* generations{nullptr};
  const SourceRegistry* sources{nullptr};
  bool clock_synchronized{false};
};

/// Pure function: evidence items plus declared context in, normalized
/// observations out. Deterministic for a given input set regardless of the
/// order the items are supplied in.
[[nodiscard]] DerivationResult derive_observations(std::vector<EvidenceItem> items,
                                                   const DerivationContext& context,
                                                   const DerivePolicy& policy);

}  // namespace loss_observatory
