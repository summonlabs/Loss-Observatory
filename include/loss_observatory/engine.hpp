#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <stop_token>
#include <algorithm>
#include <string>
#include <thread>
#include <vector>

#include "loss_observatory/aggregate.hpp"
#include "loss_observatory/classify.hpp"
#include "loss_observatory/conflict.hpp"
#include "loss_observatory/episode.hpp"
#include "loss_observatory/evidence/store.hpp"
#include "loss_observatory/localize.hpp"
#include "loss_observatory/persist.hpp"
#include "loss_observatory/report.hpp"

namespace loss_observatory {

enum class EngineState : std::uint8_t {
  Created = 0,
  Running,
  Draining,
  Stopped,
  Failed,
};

[[nodiscard]] std::string_view to_string(EngineState state) noexcept;

struct EngineConfig {
  std::string instance_name{"loss-observatory"};

  TopologyLimits topology{};
  StoreLimits store{};
  DerivePolicy derive{};
  LossPolicy loss{};
  LocalizePolicy localize{};
  AggregateLimits aggregate{};
  EpisodeLimits episodes{};
  PersistConfig persist{};

  /// Number of ingest workers. Zero means "ingest synchronously on the calling
  /// thread", which is the fully deterministic default for tests and replay.
  std::size_t worker_count{0};
  std::size_t inbox_capacity{4096};
  std::size_t max_result_items{1024};
  std::size_t max_batch_items{16384};

  /// Declares whether this deployment has a synchronised clock domain. Without
  /// it, one-way probe methods stay unsupported rather than degrading silently.
  bool clock_synchronized{false};

  bool persist_evidence_on_ingest{true};
  bool aggregate_on_classify{true};
  bool auto_compact{true};
};

struct EngineStats {
  EngineState state{EngineState::Created};
  std::size_t workers{0};
  QueueStats queue{};
  StoreStats store{};
  PersistStats persist{};
  std::uint64_t submissions{0};
  std::uint64_t completions{0};
  std::uint64_t worker_errors{0};
  std::uint64_t derivations{0};
  std::uint64_t classifications{0};
  std::uint64_t localizations{0};
  std::uint64_t episodes_recorded{0};
  std::uint64_t restart_seals{0};
  std::uint64_t persistence_failures{0};
  std::uint64_t cancelled{0};
};

/// The runtime.
///
/// Ownership model (see docs/CONCURRENCY.md):
///   * every subsystem owns exactly one mutex and never calls out while holding
///     it; no subsystem acquires another subsystem's mutex;
///   * workers never invoke user code;
///   * shutdown is a single monotonic transition Created -> Running ->
///     Draining -> Stopped, driven by one atomic and one stop_source.
///
/// The engine therefore has no lock cycle and no reentrancy: the only nested
/// locking that ever occurs is inside a single leaf subsystem.
class ObservatoryEngine {
 public:
  /// The clock is borrowed and must outlive the engine; the engine never reads
  /// a hidden global clock of its own.
  ObservatoryEngine(EngineConfig config, const Clock& clock);
  ~ObservatoryEngine();

  ObservatoryEngine(const ObservatoryEngine&) = delete;
  ObservatoryEngine& operator=(const ObservatoryEngine&) = delete;

  // ---- lifecycle -----------------------------------------------------------
  [[nodiscard]] Result<void> start();
  /// Requests cancellation, drains the inbox, seals open episodes, flushes
  /// persistence, and joins every worker. Safe to call more than once and safe
  /// to call from a worker-adjacent context.
  [[nodiscard]] Result<void> stop();
  /// Signals cancellation without waiting. stop() performs the join.
  void request_stop() noexcept;

  /// Opens persistence (when configured) and recovers prior state.
  [[nodiscard]] Result<RecoveryReport> load();
  /// Flushes the journal, compacting first when it exceeded its bound.
  [[nodiscard]] Result<void> persist();

  // ---- ingest --------------------------------------------------------------
  [[nodiscard]] Result<IngestOutcome> ingest(EvidenceItem item);
  /// Distributes a batch across the configured workers. Results are returned in
  /// submission order regardless of completion order.
  [[nodiscard]] Result<std::vector<IngestOutcome>> ingest_batch(std::vector<EvidenceItem> items);

  // ---- queries -------------------------------------------------------------
  [[nodiscard]] Result<Classification> classify(SubjectRef subject, Timestamp at);
  [[nodiscard]] Result<Classification> classify_flow(FlowId flow, Timestamp at);
  [[nodiscard]] Result<LocalizationResult> localize(const LocalizationRequest& request);
  [[nodiscard]] Result<EpisodeQueryResult> history(const EpisodeQuery& request);
  [[nodiscard]] Result<Explanation> explain(const ExplainRequest& request);
  [[nodiscard]] Result<ExportBundle> export_bundle(const ExportRequest& request);

  // ---- accessors -----------------------------------------------------------
  [[nodiscard]] TopologyRegistry& topology() noexcept { return *topology_; }
  [[nodiscard]] const TopologyRegistry& topology() const noexcept { return *topology_; }
  [[nodiscard]] SourceRegistry& sources() noexcept { return *sources_; }
  [[nodiscard]] const SourceRegistry& sources() const noexcept { return *sources_; }
  [[nodiscard]] EvidenceStore& evidence() noexcept { return *evidence_; }
  [[nodiscard]] const EvidenceStore& evidence() const noexcept { return *evidence_; }
  [[nodiscard]] GenerationRegistry& generations() noexcept { return *generations_; }
  [[nodiscard]] const GenerationRegistry& generations() const noexcept { return *generations_; }
  [[nodiscard]] EpisodeTracker& episodes() noexcept { return *episodes_; }
  [[nodiscard]] const EpisodeTracker& episodes() const noexcept { return *episodes_; }
  [[nodiscard]] BoundedAggregator& aggregator() noexcept { return *aggregate_; }
  [[nodiscard]] PersistenceStore* persistence() noexcept { return persistence_.get(); }

  [[nodiscard]] const Clock& clock() const noexcept { return *clock_; }
  [[nodiscard]] Timestamp now() const noexcept { return clock_->now(); }
  [[nodiscard]] const EngineConfig& config() const noexcept { return config_; }
  [[nodiscard]] EngineStats stats() const;
  [[nodiscard]] EngineState state() const noexcept {
    return static_cast<EngineState>(state_.load(std::memory_order_acquire));
  }
  [[nodiscard]] EngineEpoch epoch() const noexcept;

  /// Derives observations for everything currently retained. Exposed so tests
  /// and tooling can inspect the normalized evidence layer directly.
  [[nodiscard]] Result<DerivationResult> derive_all() const;

 private:
  /// Completion state for one batch submission. Held by shared pointer so that
  /// several callers may have batches in flight at the same time without
  /// sharing buffers.
  struct BatchState {
    std::mutex mutex{};
    std::condition_variable_any cv{};
    std::vector<IngestOutcome> results{};
    std::vector<std::uint8_t> done{};
    std::size_t outstanding{0};
  };

  struct WorkItem {
    EvidenceItem item{};
    std::size_t slot{0};
    std::uint64_t submission{0};
    std::shared_ptr<BatchState> batch{};
  };

  void worker_main(std::stop_token token);
  [[nodiscard]] IngestOutcome submit_sync(EvidenceItem item);
  [[nodiscard]] DerivationResult derive_snapshot() const;
  void record_generations(const EvidenceItem& item);
  void record_aggregates(const DerivationResult& derivation);
  void record_episode(const Classification& classification, SubjectRef subject, Timestamp at);
  [[nodiscard]] FreshnessContext freshness_context() const noexcept;
  [[nodiscard]] Classification classify_observations(SubjectRef subject,
                                                    const std::vector<LossObservation>& observations,
                                                    const DerivationResult& derivation, Timestamp at) const;

  EngineConfig config_{};
  const Clock* clock_{nullptr};

  std::unique_ptr<TopologyRegistry> topology_{};
  std::unique_ptr<SourceRegistry> sources_{};
  std::unique_ptr<GenerationRegistry> generations_{};
  std::unique_ptr<EvidenceStore> evidence_{};
  std::unique_ptr<EpisodeTracker> episodes_{};
  std::unique_ptr<BoundedAggregator> aggregate_{};
  std::unique_ptr<PersistenceStore> persistence_{};

  mutable std::mutex mutex_{};
  std::atomic<int> state_{static_cast<int>(EngineState::Created)};
  // Instrumentation counters are mutable so that const query paths can still
  // record that they ran. They carry no domain state.
  mutable std::atomic<std::uint64_t> submissions_{0};
  mutable std::atomic<std::uint64_t> completions_{0};
  mutable std::atomic<std::uint64_t> worker_errors_{0};
  mutable std::atomic<std::uint64_t> derivations_{0};
  mutable std::atomic<std::uint64_t> classifications_{0};
  mutable std::atomic<std::uint64_t> localizations_{0};
  mutable std::atomic<std::uint64_t> episodes_recorded_{0};
  mutable std::atomic<std::uint64_t> restart_seals_{0};
  mutable std::atomic<std::uint64_t> persistence_failures_{0};
  mutable std::atomic<std::uint64_t> cancelled_{0};

  std::stop_source stop_source_{};
  std::unique_ptr<BoundedQueue<WorkItem>> inbox_{};
  std::vector<std::jthread> workers_{};
  mutable std::mutex inbox_waiters_mutex_{};
  std::vector<std::weak_ptr<BatchState>> inbox_waiters_{};
  std::set<MeasurementId> aggregated_ids_{};
  bool aggregation_bound_hit_{false};
  RevisionId persisted_topology_revision_{};
  std::size_t persisted_source_count_{0};
  std::size_t persisted_incarnation_count_{0};
};

}  // namespace loss_observatory
