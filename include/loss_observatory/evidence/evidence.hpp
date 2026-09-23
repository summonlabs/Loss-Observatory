#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "loss_observatory/core/bounded.hpp"
#include "loss_observatory/core/time.hpp"
#include "loss_observatory/model/granularity.hpp"
#include "loss_observatory/model/ids.hpp"
#include "loss_observatory/model/method.hpp"

namespace loss_observatory {

// ---------------------------------------------------------------------------
// Distinct time roles. An observation instant and a receive instant are not
// interchangeable: a delayed delivery must not make old evidence look new.
// ---------------------------------------------------------------------------

struct ObservedAt {
  Timestamp value{};
  friend bool operator==(const ObservedAt& lhs, const ObservedAt& rhs) { return lhs.value == rhs.value; }
  friend bool operator<(const ObservedAt& lhs, const ObservedAt& rhs) { return lhs.value < rhs.value; }
  [[nodiscard]] bool is_zero() const noexcept { return value.is_zero(); }
};

struct ReceivedAt {
  Timestamp value{};
  friend bool operator==(const ReceivedAt& lhs, const ReceivedAt& rhs) { return lhs.value == rhs.value; }
  friend bool operator<(const ReceivedAt& lhs, const ReceivedAt& rhs) { return lhs.value < rhs.value; }
  [[nodiscard]] bool is_zero() const noexcept { return value.is_zero(); }
};

enum class EvidenceKind : std::uint8_t {
  Unknown = 0,
  CounterSample = 1,
  ProbeReport = 2,
  SequenceReport = 3,
  EndpointReport = 4,
};

[[nodiscard]] std::string_view to_string(EvidenceKind kind) noexcept;
[[nodiscard]] Result<EvidenceKind> parse_evidence_kind(std::string_view text);

/// What a counter counts. A drop counter and a receive counter are different
/// evidence even when they are bound to the same port.
enum class CounterScope : std::uint8_t {
  Unknown = 0,
  ReceivedPackets = 1,
  TransmittedPackets = 2,
  DroppedPackets = 3,
  DiscardedPackets = 4,
  ErrorPackets = 5,
  ReceivedBytes = 6,
  TransmittedBytes = 7,
};

[[nodiscard]] std::string_view to_string(CounterScope scope) noexcept;
[[nodiscard]] Result<CounterScope> parse_counter_scope(std::string_view text);

enum class EndpointRole : std::uint8_t {
  Unknown = 0,
  Sender = 1,
  Receiver = 2,
};

[[nodiscard]] std::string_view to_string(EndpointRole role) noexcept;
[[nodiscard]] Result<EndpointRole> parse_endpoint_role(std::string_view text);

inline constexpr std::size_t kMaxEvidenceNoteBytes = 256;
inline constexpr std::size_t kMaxEvidenceMetadataBytes = 512;

/// Raw counter reading. No delta is stored here: deltas are derived later from
/// a deterministically ordered series, so arrival order cannot change them.
struct CounterSample {
  CounterId counter{};
  CounterScope scope{CounterScope::Unknown};
  std::uint64_t value{0};
  /// Declared counter width in bits (16, 32, 64) or 0 when the source does not
  /// declare one. Without a width, a decreasing counter can never be proven to
  /// be a wrap, so it is reported as an unresolved discontinuity.
  std::uint8_t width_bits{0};
  NodeId node{};
  PortId port{};
  QueueId queue{};
  HopId hop{};
  LinkId link{};
};

/// Active probe result. p sent and p received are what the probe actually
/// issued and got back; a probe that timed out entirely is still evidence, but
/// it is evidence of exactly that and nothing more.
struct ProbeReport {
  ProbeId probe{};
  std::uint32_t sent{0};
  std::uint32_t received{0};
  std::uint32_t timed_out{0};
  bool ttl_scoped{false};
  std::uint8_t ttl{0};
  bool one_way{false};
};

/// Delivered-sequence observation over a window. The source states the
/// sequence range it saw and how many packets it actually delivered inside it.
struct SequenceReport {
  std::uint64_t lowest_sequence{0};
  std::uint64_t highest_sequence{0};
  std::uint64_t received_count{0};
  /// The source declares the sequence space restarted (new space, wrap, or
  /// rekey). Declared restarts are discontinuities, never loss.
  bool sequence_restart{false};
};

/// One endpoint's own count for a flow. Comparison needs a matching report from
/// the other endpoint in the same generation and a compatible window.
struct EndpointReport {
  EndpointRole role{EndpointRole::Unknown};
  NodeId node{};
  PortId port{};
  std::uint64_t count{0};
  bool counter_reset{false};
};

using EvidencePayload = std::variant<CounterSample, ProbeReport, SequenceReport, EndpointReport>;

[[nodiscard]] EvidenceKind kind_of(const EvidencePayload& payload) noexcept;
[[nodiscard]] MeasurementMethod default_method_for(EvidenceKind kind) noexcept;

/// The provenance and correlation header carried by every observation.
struct EvidenceHeader {
  MeasurementId id{};
  SourceId source{};
  EpochId epoch{};
  GenerationId generation{};
  RevisionId topology_revision{};
  SequenceId source_sequence{};
  ObservedAt observed_at{};
  ReceivedAt received_at{};
  MeasurementMethod method{MeasurementMethod::Unknown};
  SubjectRef subject{};
  Granularity granularity{Granularity::Unknown};
  std::string note{};
  /// Set when this item was read back from persisted state during this boot.
  /// Recovered evidence is history: it is never admissible as current proof.
  bool recovered_from_persistence{false};
};

struct EvidenceItem {
  EvidenceHeader header{};
  EvidencePayload payload{CounterSample{}};
};

/// Canonical ordering used by every derivation and by export. Sorting by
/// (observed time, source, sequence, measurement id) makes downstream results
/// independent of the order in which evidence happened to arrive.
[[nodiscard]] bool canonical_less(const EvidenceItem& lhs, const EvidenceItem& rhs) noexcept;
[[nodiscard]] std::vector<EvidenceItem> canonical_order(std::vector<EvidenceItem> items);

/// Human-readable single-line rendering used by explain and export.
[[nodiscard]] std::string describe(const EvidenceItem& item);

[[nodiscard]] std::string describe(const CounterSample& sample);
[[nodiscard]] std::string describe(const ProbeReport& report);
[[nodiscard]] std::string describe(const SequenceReport& report);
[[nodiscard]] std::string describe(const EndpointReport& report);

/// Structural validation performed before anything is stored. Returns the first
/// problem found, deterministically, or Ok.
[[nodiscard]] Status validate(const EvidenceItem& item);

}  // namespace loss_observatory
