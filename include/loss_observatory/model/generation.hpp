#pragma once

#include <cstdint>
#include <map>
#include <shared_mutex>
#include <vector>

#include "loss_observatory/core/time.hpp"
#include "loss_observatory/model/ids.hpp"

namespace loss_observatory {

/// Correlation between the generation a piece of evidence observed and the
/// generation that is current for the entity at evaluation time.
enum class GenerationMatch : std::uint8_t {
  /// Evidence cites the generation that was current at its observation time.
  Current = 0,
  /// A newer generation is known for this entity: the evidence describes a
  /// superseded binding and cannot prove anything about the current one.
  Superseded,
  /// No generation record covers the observation time.
  UnknownGeneration,
  /// The entity never had a generation recorded.
  NoGenerationRecorded,
};

[[nodiscard]] std::string_view to_string(GenerationMatch match) noexcept;

struct GenerationStatus {
  GenerationMatch match{GenerationMatch::NoGenerationRecorded};
  GenerationId observed{};
  GenerationId current{};
  Timestamp evidence_time{};
  Timestamp current_since{};
  /// True when the entity's generation changed at or before the evaluation
  /// instant, so an older generation's evidence must be treated as historical.
  bool generation_changed{false};

  [[nodiscard]] std::string to_string() const;
};

/// Records which generation was in force for an entity over time, and answers
/// "which generation was current at instant T" as a pure function of the
/// recorded set.
///
/// Determinism note: records are keyed by (entity, generation) with first/last
/// observed instants, so the answer depends on the recorded evidence set and
/// not on the order in which records arrived.
class GenerationRegistry {
 public:
  GenerationRegistry() = default;
  GenerationRegistry(const GenerationRegistry&) = delete;
  GenerationRegistry& operator=(const GenerationRegistry&) = delete;

  void observe_flow(FlowId flow, GenerationId generation, Timestamp observed_at);
  void observe_path(PathId path, GenerationId generation, Timestamp observed_at);

  [[nodiscard]] GenerationStatus correlate_flow(FlowId flow, GenerationId generation,
                                                Timestamp observation_time) const;
  [[nodiscard]] GenerationStatus correlate_path(PathId path, GenerationId generation,
                                                Timestamp observation_time) const;

  [[nodiscard]] std::vector<std::pair<GenerationId, std::pair<Timestamp, Timestamp>>> flow_generations(
      FlowId flow) const;

  [[nodiscard]] std::size_t tracked_flows() const;
  [[nodiscard]] std::size_t tracked_paths() const;

  void set_max_entities(std::size_t value) noexcept { max_entities_ = value; }

 private:
  struct GenerationRecord {
    Timestamp first_seen{};
    Timestamp last_seen{};
  };

  static void observe(std::map<std::uint64_t, std::map<GenerationId, GenerationRecord>>& table,
                      std::uint64_t entity, GenerationId generation, Timestamp observed_at,
                      std::size_t max_entities);

  static GenerationStatus correlate(const std::map<std::uint64_t, std::map<GenerationId, GenerationRecord>>& table,
                                    std::uint64_t entity, GenerationId generation, Timestamp at);

  mutable std::shared_mutex mutex_{};
  std::map<std::uint64_t, std::map<GenerationId, GenerationRecord>> flows_{};
  std::map<std::uint64_t, std::map<GenerationId, GenerationRecord>> paths_{};
  std::size_t max_entities_{4096};
};

}  // namespace loss_observatory
