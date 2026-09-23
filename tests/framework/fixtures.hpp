#pragma once

#include <memory>
#include <string>
#include <vector>

#include "lo_test.hpp"
#include "loss_observatory/engine.hpp"
#include "loss_observatory/script.hpp"
#include "loss_observatory/testkit/testkit.hpp"

namespace lofixture {

using namespace loss_observatory;

/// Default engine configuration for tests: no worker threads (so every test is
/// single-threaded and deterministic unless it deliberately opts in), no
/// persistence, and a synthetic clock.
[[nodiscard]] EngineConfig default_config(std::string instance = "test");

/// Engine bound to a ManualClock the caller keeps alive.
[[nodiscard]] std::unique_ptr<ObservatoryEngine> make_engine(ManualClock& clock, EngineConfig config);

[[nodiscard]] EngineConfig persistent_config(const std::string& directory, const std::string& instance);

/// A declared linear path plus one flow and one source incarnation.
struct Network {
  FlowId flow{};
  PathId path{};
  GenerationId generation{};
  SourceId source{};
  EpochId epoch{};
  std::vector<Hop> hops{};
  std::string name{};
};

[[nodiscard]] Result<Network> declare_linear_network(ObservatoryEngine& engine, std::size_t hop_count,
                                                     Timestamp at, std::string_view seed,
                                                     SourceAuthority authority = SourceAuthority::Primary,
                                                     SourceKind kind = SourceKind::CounterTelemetry);

/// Convenience builder so tests read as declarations rather than assignments.
class ItemBuilder {
 public:
  explicit ItemBuilder(MeasurementId id) { item_.header.id = id; }

  ItemBuilder& source(SourceId value);
  ItemBuilder& epoch(EpochId value);
  ItemBuilder& generation(GenerationId value);
  ItemBuilder& revision(RevisionId value);
  ItemBuilder& sequence(std::uint64_t value);
  ItemBuilder& subject(SubjectRef value);
  ItemBuilder& granularity(Granularity value);
  ItemBuilder& times(Timestamp observed, Timestamp received);
  ItemBuilder& method(MeasurementMethod value);
  ItemBuilder& note(std::string value);
  ItemBuilder& counter(CounterSample value);
  ItemBuilder& probe(ProbeReport value);
  ItemBuilder& sequence_report(SequenceReport value);
  ItemBuilder& endpoint(EndpointReport value);

  [[nodiscard]] EvidenceItem build() const { return item_; }

 private:
  EvidenceItem item_{};
};

/// Submits and requires acceptance, so a test cannot silently proceed on
/// evidence the runtime refused.
void ingest_ok(ObservatoryEngine& engine, const EvidenceItem& item);

/// Runs a script and requires that every command succeeded. The result is
/// informational: a failure throws, so discarding it is safe.
ScriptRunResult run_script_ok(ObservatoryEngine& engine, std::string_view text);

[[nodiscard]] Timestamp base_time();

/// A unique scratch directory for a test, removed by the caller when done.
[[nodiscard]] std::string scratch_directory(std::string_view name);
void remove_directory(const std::string& path);

[[nodiscard]] std::string read_file(const std::string& path);

}  // namespace lofixture

// Test suites read better with the fixtures in scope. These are test-only
// declarations; nothing here is exported by the library.
using lofixture::base_time;
using lofixture::declare_linear_network;
using lofixture::default_config;
using lofixture::ingest_ok;
using lofixture::make_engine;
using lofixture::persistent_config;
using lofixture::read_file;
using lofixture::remove_directory;
using lofixture::run_script_ok;
using lofixture::scratch_directory;
