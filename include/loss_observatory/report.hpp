#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "loss_observatory/aggregate.hpp"
#include "loss_observatory/conflict.hpp"
#include "loss_observatory/episode.hpp"
#include "loss_observatory/localize.hpp"

namespace loss_observatory {

struct ExplainRequest {
  SubjectRef subject{};
  Timestamp at{};
  bool include_localization{true};
  bool include_history{true};
  bool include_claims{true};
  bool include_evidence_list{true};
  bool include_topology{false};
  Granularity requested_max_granularity{Granularity::Queue};
  std::size_t max_lines{512};
  std::size_t max_history_episodes{16};
};

/// A deterministic explanation: the same engine state and the same instant
/// produce byte-identical text.
struct Explanation {
  SubjectRef subject{};
  Timestamp at{};
  LossClass klass{LossClass::Unknown};
  Classification classification{};
  bool has_localization{false};
  LocalizationResult localization{};
  EpisodeQueryResult history{};
  std::vector<AggregateBucket> aggregates{};
  std::vector<MeasurementId> evidence_ids{};
  std::vector<BoundNote> bounds{};
  std::vector<RejectionNote> rejections{};
  std::vector<std::string> lines{};
  bool truncated{false};
  std::string text{};

  [[nodiscard]] std::string to_string() const { return text; }
};

enum class ExportFormat : std::uint8_t {
  /// Line-oriented key=value text. Round-trippable through the script reader.
  ScriptText = 0,
  /// Canonical, stable, human-readable report.
  Report = 1,
  /// Length-prefixed binary records with a CRC-checked frame per item.
  Binary = 2,
};

[[nodiscard]] std::string_view to_string(ExportFormat format) noexcept;
[[nodiscard]] Result<ExportFormat> parse_export_format(std::string_view text);

enum class ExportScope : std::uint8_t {
  Topology = 0,
  Sources,
  Evidence,
  Episodes,
  Aggregates,
  All,
};

[[nodiscard]] std::string_view to_string(ExportScope scope) noexcept;
[[nodiscard]] Result<ExportScope> parse_export_scope(std::string_view text);

struct ExportRequest {
  ExportScope scope{ExportScope::All};
  ExportFormat format{ExportFormat::ScriptText};
  std::optional<FlowId> flow{};
  Timestamp from{};
  Timestamp to{};
  std::size_t max_items{100000};
  bool include_recovered{true};
  bool include_synthetic{true};
};

struct ExportBundle {
  ExportScope scope{ExportScope::All};
  ExportFormat format{ExportFormat::ScriptText};
  std::string text{};
  std::vector<std::uint8_t> binary{};
  std::size_t item_count{0};
  bool truncated{false};
  std::vector<BoundNote> bounds{};
  std::vector<std::string> provenance_lines{};

  [[nodiscard]] std::string to_string() const;
};

/// Renders a single evidence item in the canonical line format.
[[nodiscard]] std::string render_evidence(const EvidenceItem& item);
/// Renders the declared topology in the canonical line format.
[[nodiscard]] std::string render_topology(const TopologyRegistry& topology);
/// Renders source registration and incarnations.
[[nodiscard]] std::string render_sources(const SourceRegistry& sources);
[[nodiscard]] std::string render_episode(const LossEpisode& episode);
[[nodiscard]] std::string render_aggregate(const AggregateBucket& bucket);
[[nodiscard]] std::string render_classification(const Classification& classification);
[[nodiscard]] std::string render_localization(const LocalizationResult& result);
[[nodiscard]] std::string render_attribution(const Attribution& attribution);
[[nodiscard]] std::string render_claim(const SourceClaim& claim);

/// Renders a complete explanation as deterministic text. The same explanation
/// value always produces byte-identical output.
[[nodiscard]] std::string render_explanation(const ExplainRequest& request, const Explanation& explanation);

}  // namespace loss_observatory
