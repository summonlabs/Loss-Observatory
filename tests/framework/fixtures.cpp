#include "fixtures.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace lofixture {

EngineConfig default_config(std::string instance) {
  EngineConfig config{};
  config.instance_name = std::move(instance);
  config.worker_count = 0;
  config.persist.enabled = false;
  return config;
}

std::unique_ptr<ObservatoryEngine> make_engine(ManualClock& clock, EngineConfig config) {
  auto engine = std::make_unique<ObservatoryEngine>(std::move(config), clock);
  const Result<void> started = engine->start();
  if (!started.ok()) {
    throw lotest::Failure{"engine failed to start: " + started.status().to_string()};
  }
  return engine;
}

EngineConfig persistent_config(const std::string& directory, const std::string& instance) {
  EngineConfig config = default_config(instance);
  config.persist.enabled = true;
  config.persist.directory = directory;
  config.persist.instance = instance;
  config.persist_evidence_on_ingest = true;
  return config;
}

Result<Network> declare_linear_network(ObservatoryEngine& engine, std::size_t hop_count, Timestamp at,
                                       std::string_view seed, SourceAuthority authority,
                                       SourceKind kind) {
  Network network{};
  network.name = std::string(seed);
  const std::string prefix(seed);
  network.path = PathId::from_canonical_text("path/" + prefix);
  network.flow = FlowId::from_canonical_text("flow/" + prefix);
  network.generation = GenerationId::from_canonical_text("gen/" + prefix);
  network.source = SourceId::from_canonical_text("source/" + prefix);

  Path path{};
  path.id = network.path;
  path.kind = PathKind::Synthetic;
  for (std::size_t index = 0; index < hop_count; ++index) {
    Hop hop{};
    hop.id = HopId::from_canonical_text("hop/" + prefix + "/" + std::to_string(index));
    hop.index = static_cast<std::uint32_t>(index);
    hop.node = NodeId::from_canonical_text("node/" + prefix + "/" + std::to_string(index));
    hop.ingress_port = PortId::from_canonical_text("in/" + prefix + "/" + std::to_string(index));
    hop.egress_port = PortId::from_canonical_text("out/" + prefix + "/" + std::to_string(index));
    hop.ingress_link = LinkId::from_canonical_text("inlink/" + prefix + "/" + std::to_string(index));
    hop.egress_link = LinkId::from_canonical_text("outlink/" + prefix + "/" + std::to_string(index));
    path.hops.push_back(hop);
  }
  const Result<UpsertResult> path_result = engine.topology().upsert_path(path, true);
  if (!path_result.ok()) {
    return path_result.status();
  }
  const Result<Path> stored = engine.topology().find_path(network.path);
  if (!stored.ok()) {
    return stored.status();
  }
  network.hops = stored.value().hops;

  FlowBinding flow{};
  flow.id = network.flow;
  flow.source = Endpoint{network.hops.front().node, network.hops.front().ingress_port};
  flow.destination = Endpoint{network.hops.back().node, network.hops.back().egress_port};
  flow.protocol = ProtocolId::from_canonical_text("proto/tcp");
  flow.path = network.path;
  flow.generation = network.generation;
  flow.binding_revision = stored.value().revision;
  const Result<UpsertResult> flow_result = engine.topology().upsert_flow(flow, true);
  if (!flow_result.ok()) {
    return flow_result.status();
  }

  SourceDescriptor descriptor{};
  descriptor.id = network.source;
  descriptor.name = "source-" + prefix;
  descriptor.kind = kind;
  descriptor.authority = authority;
  const Result<UpsertOutcome> source_result = engine.sources().register_source(descriptor, true);
  if (!source_result.ok()) {
    return source_result.status();
  }
  const Result<EpochId> epoch =
      engine.sources().activate_incarnation(network.source, "incarnation-" + prefix, at);
  if (!epoch.ok()) {
    return epoch.status();
  }
  network.epoch = epoch.value();
  return network;
}

ItemBuilder& ItemBuilder::source(SourceId value) {
  item_.header.source = value;
  return *this;
}
ItemBuilder& ItemBuilder::epoch(EpochId value) {
  item_.header.epoch = value;
  return *this;
}
ItemBuilder& ItemBuilder::generation(GenerationId value) {
  item_.header.generation = value;
  return *this;
}
ItemBuilder& ItemBuilder::revision(RevisionId value) {
  item_.header.topology_revision = value;
  return *this;
}
ItemBuilder& ItemBuilder::sequence(std::uint64_t value) {
  item_.header.source_sequence = SequenceId::from_value(value);
  return *this;
}
ItemBuilder& ItemBuilder::subject(SubjectRef value) {
  item_.header.subject = value;
  return *this;
}
ItemBuilder& ItemBuilder::granularity(Granularity value) {
  item_.header.granularity = value;
  return *this;
}
ItemBuilder& ItemBuilder::times(Timestamp observed, Timestamp received) {
  item_.header.observed_at.value = observed;
  item_.header.received_at.value = received;
  return *this;
}
ItemBuilder& ItemBuilder::method(MeasurementMethod value) {
  item_.header.method = value;
  return *this;
}
ItemBuilder& ItemBuilder::note(std::string value) {
  item_.header.note = std::move(value);
  return *this;
}
ItemBuilder& ItemBuilder::counter(CounterSample value) {
  item_.payload = value;
  return *this;
}
ItemBuilder& ItemBuilder::probe(ProbeReport value) {
  item_.payload = value;
  return *this;
}
ItemBuilder& ItemBuilder::sequence_report(SequenceReport value) {
  item_.payload = value;
  return *this;
}
ItemBuilder& ItemBuilder::endpoint(EndpointReport value) {
  item_.payload = value;
  return *this;
}

void ingest_ok(ObservatoryEngine& engine, const EvidenceItem& item) {
  const Result<IngestOutcome> outcome = engine.ingest(item);
  if (!outcome.ok()) {
    throw lotest::Failure{"ingest failed: " + outcome.status().to_string()};
  }
  if (!outcome.value().accepted) {
    throw lotest::Failure{"ingest rejected: " + std::string(to_string(outcome.value().reason)) + " (" +
                          outcome.value().detail + ")"};
  }
}

ScriptRunResult run_script_ok(ObservatoryEngine& engine, std::string_view text) {
  auto program = parse_script(text);
  if (!program.ok()) {
    throw lotest::Failure{"script did not parse: " + program.status().to_string()};
  }
  ScriptRunResult run = apply_script(engine, program.value());
  if (!run.ok()) {
    throw lotest::Failure{"script failed:\n" + run.render()};
  }
  return run;
}

Timestamp base_time() { return Timestamp::from_unix_seconds(1767225600); }

std::string scratch_directory(std::string_view name) {
  const std::filesystem::path directory =
      std::filesystem::path(LO_TEST_DATA_DIR) / std::string(name);
  std::error_code error;
  std::filesystem::remove_all(directory, error);
  std::filesystem::create_directories(directory, error);
  return directory.string();
}

void remove_directory(const std::string& path) {
  std::error_code error;
  std::filesystem::remove_all(path, error);
}

std::string read_file(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file.good()) {
    throw lotest::Failure{"cannot read file: " + path};
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

}  // namespace lofixture
