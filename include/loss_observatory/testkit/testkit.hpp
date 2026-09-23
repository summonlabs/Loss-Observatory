#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "loss_observatory/engine.hpp"

/// Deterministic generators and invariant checkers used by this repository's
/// tests, benchmarks, and examples.
///
/// Everything produced here is SYNTHETIC by construction and is labelled as
/// such in the evidence it emits. Nothing in this header contacts a switch, an
/// ASIC, a NIC, or any external system.
namespace loss_observatory::testkit {

/// SplitMix64. Reproducible across platforms and across runs.
class DeterministicRng {
 public:
  explicit DeterministicRng(std::uint64_t seed) noexcept : state_(seed) {}

  [[nodiscard]] std::uint64_t next_u64() noexcept;
  [[nodiscard]] std::uint32_t next_u32() noexcept;
  /// Uniform in [0, bound); bound must be non-zero.
  [[nodiscard]] std::uint64_t next_below(std::uint64_t bound) noexcept;
  [[nodiscard]] bool next_bool(std::uint32_t percent_true) noexcept;
  [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }

 private:
  std::uint64_t state_{0};
  std::uint64_t seed_{0};
};

struct ScenarioConfig {
  std::uint64_t seed{1};
  std::size_t hop_count{4};
  std::size_t sample_count{16};
  Duration sample_interval = Duration::from_millis(250);
  Timestamp start{Timestamp::from_unix_seconds(1767225600)};  // 2026-01-01T00:00:00Z
  std::uint32_t loss_ratio_bp{250};
  std::uint8_t counter_width_bits{32};
  bool inject_counter_reset{false};
  bool inject_counter_wrap{false};
  bool inject_generation_change{false};
  SourceAuthority authority{SourceAuthority::Primary};
  SourceKind kind{SourceKind::CounterTelemetry};
  std::string source_name{"synthetic-counter"};
};

struct Scenario {
  FlowId flow{};
  PathId path{};
  std::vector<HopId> hops{};
  GenerationId generation{};
  SourceId source{};
  EpochId epoch{};
  std::vector<EvidenceItem> items{};
  std::uint64_t injected_drops{0};
  std::string description{};
};

/// Declares a synthetic topology, source, and flow on p engine and returns the
/// identities involved. The generated source is kind=Synthetic so downstream
/// output can always identify it.
[[nodiscard]] Result<Scenario> install_synthetic_scenario(ObservatoryEngine& engine,
                                                          const ScenarioConfig& config);

/// Generates a synthetic drop-counter series with a known number of injected
/// drops, so a test can assert the runtime recovers exactly that number.
[[nodiscard]] Result<Scenario> make_drop_counter_scenario(ObservatoryEngine& engine,
                                                          const ScenarioConfig& config);

/// Direct evidence generator with no engine coupling, for unit tests.
[[nodiscard]] std::vector<EvidenceItem> make_counter_series(const ScenarioConfig& config,
                                                             CounterScope scope,
                                                             std::uint64_t start_value,
                                                             std::vector<std::uint64_t> increments);

// ---------------------------------------------------------------------------
// Invariant checkers. These encode the properties this runtime claims, so both
// tests and adversarial generators can call the same definition of "correct".
// ---------------------------------------------------------------------------

/// A conclusion must not rest on evidence that fails the fresh-and-admissible
/// test for the instant under evaluation.
[[nodiscard]] bool stale_evidence_cannot_prove_current_loss(const Classification& classification);

/// Discontinuities must be labelled as such and must never be wrapped into a
/// loss total.
[[nodiscard]] bool discontinuities_are_explicit(const Classification& classification,
                                                std::span<const LossObservation> observations);

/// Conflicting admissible sources must produce ConflictingEvidence unless a
/// strictly higher authority resolves them, and the losing claim must survive.
[[nodiscard]] bool disagreement_is_retained(const Classification& classification,
                                            const ConflictResolution& resolution);

/// Localization granularity must never exceed the granularity of the evidence
/// that supports it.
[[nodiscard]] bool localization_within_evidence_granularity(const LocalizationResult& result,
                                                            std::span<const LossObservation> observations);

/// After a restart no previously open episode may be reported as open, and no
/// recovered observation may be admissible under the supplied policy.
[[nodiscard]] bool restart_does_not_revive(const EpisodeQueryResult& history,
                                           std::span<const LossObservation> observations,
                                           const FreshnessPolicy& policy,
                                           const FreshnessContext& context, Timestamp now);

/// Every reason code in p reasons must be reachable and rendered.
[[nodiscard]] bool reasons_are_renderable(std::span<const ReasonCode> reasons);

}  // namespace loss_observatory::testkit
