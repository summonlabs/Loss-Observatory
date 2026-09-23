#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "fixtures.hpp"
#include "loss_observatory/core/bytes.hpp"
#include "loss_observatory/persist.hpp"

using namespace loss_observatory;

namespace {

const Timestamp kNow = Timestamp::from_unix_seconds(6000);

PersistConfig config_for(const std::string& directory) {
  PersistConfig config{};
  config.enabled = true;
  config.directory = directory;
  config.instance = "unit";
  return config;
}

EvidenceItem counter_item(std::uint64_t id, std::uint64_t value) {
  EvidenceItem item{};
  item.header.id = MeasurementId::from_value(id);
  item.header.source = SourceId::from_canonical_text("src");
  item.header.epoch = EpochId::from_canonical_text("epoch/1");
  item.header.generation = GenerationId::from_canonical_text("gen/1");
  item.header.topology_revision = RevisionId::from_canonical_text("rev/1");
  item.header.source_sequence = SequenceId::from_value(id);
  item.header.observed_at.value = kNow - Duration::from_seconds(1);
  item.header.received_at.value = kNow - Duration::from_seconds(1);
  item.header.method = MeasurementMethod::CounterDelta;
  item.header.subject = SubjectRef::queue(QueueId::from_canonical_text("q1"));
  item.header.granularity = Granularity::Queue;
  item.header.note = "unit note";
  CounterSample sample{};
  sample.counter = CounterId::from_canonical_text("c1");
  sample.scope = CounterScope::DroppedPackets;
  sample.value = value;
  sample.width_bits = 32;
  sample.queue = QueueId::from_canonical_text("q1");
  item.payload = sample;
  return item;
}

/// Declares a two-hop path, a queue, a flow, and a source incarnation, then
/// returns the identities an evidence item must cite to be admissible.
struct World {
  SourceId source{};
  EpochId epoch{};
};

World declare_world(ObservatoryEngine& engine) {
  auto network = lofixture::declare_linear_network(engine, 2, kNow, "persist");
  if (!network.ok()) {
    throw lotest::Failure{"network declaration failed"};
  }
  QueueEntity queue{};
  queue.id = QueueId::from_canonical_text("q1");
  queue.node = network.value().hops.front().node;
  queue.port = network.value().hops.front().egress_port;
  queue.priority = PriorityId::from_canonical_text("3");
  queue.kind = QueueKind::EgressPortQueue;
  if (!engine.topology().upsert_queue(queue).ok()) {
    throw lotest::Failure{"queue declaration failed"};
  }
  World world{};
  world.source = network.value().source;
  world.epoch = network.value().epoch;
  return world;
}

EvidenceItem engine_item(std::uint64_t id, std::uint64_t value, const World& world) {
  EvidenceItem item = counter_item(id, value);
  item.header.source = world.source;
  item.header.epoch = world.epoch;
  return item;
}

std::vector<std::uint8_t> read_bytes(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
}

void write_bytes(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::string journal_path(const std::string& directory) {
  return (std::filesystem::path(directory) / "unit.loj").string();
}

}  // namespace

LO_TEST(persist, evidence_round_trips_for_every_payload_kind) {
  std::vector<EvidenceItem> items;
  items.push_back(counter_item(1, 42));

  EvidenceItem probe = counter_item(2, 0);
  probe.header.method = MeasurementMethod::ProbeRoundTrip;
  probe.header.granularity = Granularity::Path;
  probe.header.subject = SubjectRef::path(PathId::from_canonical_text("p1"));
  ProbeReport probe_report{};
  probe_report.probe = ProbeId::from_canonical_text("probe/1");
  probe_report.sent = 100;
  probe_report.received = 95;
  probe_report.timed_out = 5;
  probe_report.ttl_scoped = true;
  probe_report.ttl = 4;
  probe.payload = probe_report;
  items.push_back(probe);

  EvidenceItem sequence = counter_item(3, 0);
  sequence.header.method = MeasurementMethod::SequenceGap;
  sequence.header.granularity = Granularity::Flow;
  sequence.header.subject = SubjectRef::flow(FlowId::from_canonical_text("f1"));
  SequenceReport sequence_report{};
  sequence_report.lowest_sequence = 10;
  sequence_report.highest_sequence = 20;
  sequence_report.received_count = 9;
  sequence_report.sequence_restart = true;
  sequence.payload = sequence_report;
  items.push_back(sequence);

  EvidenceItem endpoint = counter_item(4, 0);
  endpoint.header.method = MeasurementMethod::EndpointComparison;
  endpoint.header.granularity = Granularity::Flow;
  endpoint.header.subject = SubjectRef::flow(FlowId::from_canonical_text("f1"));
  EndpointReport endpoint_report{};
  endpoint_report.role = EndpointRole::Receiver;
  endpoint_report.node = NodeId::from_canonical_text("n2");
  endpoint_report.port = PortId::from_canonical_text("3");
  endpoint_report.count = 900;
  endpoint_report.counter_reset = true;
  endpoint.payload = endpoint_report;
  items.push_back(endpoint);

  for (const EvidenceItem& item : items) {
    const std::vector<std::uint8_t> encoded = encode_evidence(item);
    LO_REQUIRE(!encoded.empty());
    auto decoded = decode_evidence(ByteSpan{encoded});
    LO_REQUIRE(decoded.ok());
    LO_CHECK(decoded.value().header.id == item.header.id);
    LO_CHECK(decoded.value().header.source == item.header.source);
    LO_CHECK(decoded.value().header.epoch == item.header.epoch);
    LO_CHECK(decoded.value().header.generation == item.header.generation);
    LO_CHECK(decoded.value().header.topology_revision == item.header.topology_revision);
    LO_CHECK(decoded.value().header.source_sequence == item.header.source_sequence);
    LO_CHECK(decoded.value().header.observed_at == item.header.observed_at);
    LO_CHECK(decoded.value().header.received_at == item.header.received_at);
    LO_CHECK(decoded.value().header.method == item.header.method);
    LO_CHECK(decoded.value().header.granularity == item.header.granularity);
    LO_CHECK(decoded.value().header.subject == item.header.subject);
    LO_CHECK_EQ(decoded.value().header.note, item.header.note);
    LO_CHECK(kind_of(decoded.value().payload) == kind_of(item.payload));
    LO_CHECK(!decode_evidence(ByteSpan{encoded.data(), encoded.size() - 1}).ok());
  }
}

LO_TEST(persist, declared_state_survives_a_reopen) {
  const std::string directory = lofixture::scratch_directory("persist-reopen");
  const PersistConfig config = config_for(directory);

  ManualClock clock{kNow};
  EngineConfig engine_config = lofixture::persistent_config(directory, "unit");
  engine_config.loss.freshness.max_age = Duration::from_seconds(3600);
  {
    auto engine = lofixture::make_engine(clock, engine_config);
    const World world = declare_world(*engine);
    lofixture::ingest_ok(*engine, engine_item(1, 100, world));
    (void)engine->persist();
    (void)engine->stop();
  }

  ObservatoryEngine reopened{engine_config, clock};
  LO_REQUIRE(reopened.start().ok());
  auto recovery = reopened.load();
  LO_REQUIRE(recovery.ok());
  LO_CHECK(recovery.value().version_mismatch == false);
  LO_CHECK(recovery.value().checksum_failure == false);
  LO_CHECK(recovery.value().truncated == false);
  LO_CHECK_EQ(recovery.value().evidence_restored, 1ULL);
  LO_CHECK_EQ(recovery.value().topology_restored, 3ULL);
  LO_CHECK_EQ(reopened.topology().path_count(), 1ULL);
  LO_CHECK_EQ(reopened.topology().flow_count(), 1ULL);
  LO_CHECK_EQ(reopened.topology().queue_count(), 1ULL);
  LO_CHECK(reopened.sources().source_count() > 0ULL);
  LO_CHECK_EQ(reopened.evidence().item_count(), 1ULL);
  (void)reopened.stop();
  lofixture::remove_directory(directory);
}

LO_TEST(persist, corrupted_record_stops_recovery_conservatively) {
  const std::string directory = lofixture::scratch_directory("persist-corrupt");
  const PersistConfig config = config_for(directory);
  {
    PersistenceStore store{config};
    LO_REQUIRE(store.open_session(kNow, "boot-1").ok());
    for (std::uint64_t i = 1; i <= 4; ++i) {
      LO_REQUIRE(store.append_evidence(counter_item(i, i * 10)).ok());
    }
    LO_REQUIRE(store.flush().ok());
    LO_REQUIRE(store.close().ok());
  }
  std::vector<std::uint8_t> bytes = read_bytes(journal_path(directory));
  LO_REQUIRE(bytes.size() > 40ULL);
  bytes.back() = static_cast<std::uint8_t>(bytes.back() ^ 0xFFU);
  write_bytes(journal_path(directory), bytes);

  TopologyRegistry topology{};
  SourceRegistry sources{};
  EvidenceStore evidence{sources};
  EpisodeTracker episodes{};
  PersistenceStore reader{config};
  auto report = reader.load(topology, sources, evidence, episodes, kNow);
  LO_REQUIRE(report.ok());
  LO_CHECK(report.value().checksum_failure);
  LO_CHECK(report.value().truncated);
  LO_CHECK(report.value().records_discarded >= 1ULL);
  LO_CHECK(report.value().evidence_restored >= 1ULL);
  LO_CHECK(evidence.item_count() < 4ULL);
  for (const EvidenceItem& item : evidence.snapshot()) {
    LO_CHECK(item.header.recovered_from_persistence);
  }
  lofixture::remove_directory(directory);
}

LO_TEST(persist, a_truncated_journal_is_reported_not_guessed) {
  const std::string directory = lofixture::scratch_directory("persist-truncated");
  const PersistConfig config = config_for(directory);
  {
    PersistenceStore store{config};
    LO_REQUIRE(store.open_session(kNow, "boot-1").ok());
    LO_REQUIRE(store.append_evidence(counter_item(1, 10)).ok());
    LO_REQUIRE(store.append_evidence(counter_item(2, 20)).ok());
    LO_REQUIRE(store.flush().ok());
    LO_REQUIRE(store.close().ok());
  }
  std::vector<std::uint8_t> bytes = read_bytes(journal_path(directory));
  LO_REQUIRE(bytes.size() > 30ULL);
  bytes.resize(bytes.size() - 12);
  write_bytes(journal_path(directory), bytes);

  TopologyRegistry topology{};
  SourceRegistry sources{};
  EvidenceStore evidence{sources};
  EpisodeTracker episodes{};
  PersistenceStore reader{config};
  auto report = reader.load(topology, sources, evidence, episodes, kNow);
  LO_REQUIRE(report.ok());
  LO_CHECK(report.value().truncated);
  LO_CHECK(report.value().records_accepted >= 1ULL);
  lofixture::remove_directory(directory);
}

LO_TEST(persist, a_format_version_mismatch_refuses_to_load) {
  const std::string directory = lofixture::scratch_directory("persist-version");
  const PersistConfig config = config_for(directory);
  {
    PersistenceStore store{config};
    LO_REQUIRE(store.open_session(kNow, "boot-1").ok());
    LO_REQUIRE(store.append_evidence(counter_item(1, 10)).ok());
    LO_REQUIRE(store.flush().ok());
    LO_REQUIRE(store.close().ok());
  }
  std::vector<std::uint8_t> bytes = read_bytes(journal_path(directory));
  LO_REQUIRE(bytes.size() > 28ULL);
  // The manifest payload begins right after the 24-byte record header; its
  // first field is the format version. The record's CRC is recomputed so the
  // test exercises the version check rather than the integrity check.
  bytes[24] = 0xEE;
  bytes[25] = 0xEE;
  std::uint32_t length = 0;
  for (int i = 0; i < 4; ++i) {
    length |= static_cast<std::uint32_t>(bytes[8 + i]) << (8 * i);
  }
  const std::uint32_t crc =
      crc32c_extend(0, ByteSpan{bytes.data(), 20});
  const std::uint32_t total = crc32c_extend(crc, ByteSpan{bytes.data() + 24, length});
  for (int i = 0; i < 4; ++i) {
    bytes[20 + i] = static_cast<std::uint8_t>((total >> (8 * i)) & 0xFFU);
  }
  write_bytes(journal_path(directory), bytes);

  TopologyRegistry topology{};
  SourceRegistry sources{};
  EvidenceStore evidence{sources};
  EpisodeTracker episodes{};
  PersistenceStore reader{config};
  auto report = reader.load(topology, sources, evidence, episodes, kNow);
  LO_REQUIRE(report.ok());
  LO_CHECK(report.value().version_mismatch);
  LO_CHECK_EQ(report.value().records_accepted, 0ULL);
  LO_CHECK_EQ(evidence.item_count(), 0ULL);
  lofixture::remove_directory(directory);
}

LO_TEST(persist, a_missing_journal_is_not_an_error) {
  const std::string directory = lofixture::scratch_directory("persist-missing");
  const PersistConfig config = config_for(directory);
  TopologyRegistry topology{};
  SourceRegistry sources{};
  EvidenceStore evidence{sources};
  EpisodeTracker episodes{};
  PersistenceStore reader{config};
  auto report = reader.load(topology, sources, evidence, episodes, kNow);
  LO_REQUIRE(report.ok());
  LO_CHECK(!report.value().truncated);
  LO_CHECK_EQ(report.value().records_read, 0ULL);
  LO_CHECK(!report.value().detail.empty());
  lofixture::remove_directory(directory);
}

LO_TEST(persist, compaction_preserves_state_and_shrinks_the_journal) {
  const std::string directory = lofixture::scratch_directory("persist-compact");
  const PersistConfig config = config_for(directory);
  ManualClock clock{kNow};
  EngineConfig engine_config = lofixture::persistent_config(directory, "unit");
  engine_config.loss.freshness.max_age = Duration::from_seconds(3600);

  auto engine = lofixture::make_engine(clock, engine_config);
  const World world = declare_world(*engine);
  for (std::uint64_t i = 1; i <= 20; ++i) {
    lofixture::ingest_ok(*engine, engine_item(i, i * 5, world));
  }
  LO_REQUIRE(engine->persist().ok());
  // Simulate a journal that accumulated repeated declaration snapshots, which
  // is exactly the growth compaction exists to bound.
  for (int repetition = 0; repetition < 20; ++repetition) {
    LO_REQUIRE(engine->persistence()->append_topology(engine->topology()).ok());
  }
  const std::uint64_t before = engine->persistence()->journal_bytes();
  LO_REQUIRE(engine->persistence()->compact(engine->topology(), engine->sources(), engine->evidence(),
                                           engine->episodes())
                 .ok());
  const std::uint64_t after = engine->persistence()->journal_bytes();
  LO_CHECK(after < before);
  (void)engine->stop();

  ObservatoryEngine reopened{engine_config, clock};
  LO_REQUIRE(reopened.start().ok());
  auto recovery = reopened.load();
  LO_REQUIRE(recovery.ok());
  LO_CHECK_EQ(recovery.value().evidence_restored, 20ULL);
  LO_CHECK_EQ(reopened.evidence().item_count(), 20ULL);
  (void)reopened.stop();
  lofixture::remove_directory(directory);
}

LO_TEST(persist, oversized_payloads_are_refused_not_truncated) {
  const std::string directory = lofixture::scratch_directory("persist-oversized");
  PersistConfig config = config_for(directory);
  // Large enough for the manifest, too small for an evidence record carrying a
  // realistic note.
  config.limits.max_payload_bytes = 128;
  PersistenceStore store{config};
  LO_REQUIRE(store.open_session(kNow, "boot-1").ok());
  EvidenceItem item = counter_item(1, 10);
  item.header.note = std::string(120, 'n');
  auto result = store.append_evidence(item);
  LO_CHECK(!result.ok());
  LO_CHECK(result.code() == StatusCode::CapacityExceeded);
  LO_CHECK_EQ(store.stats().refused_payloads, 1ULL);
  (void)store.close();
  lofixture::remove_directory(directory);
}

LO_TEST(persist, a_disabled_store_reports_rather_than_pretending) {
  PersistConfig config{};
  config.enabled = false;
  PersistenceStore store{config};
  auto session = store.open_session(kNow, "boot-1");
  LO_CHECK(!session.ok());
  LO_CHECK(session.code() == StatusCode::Unsupported);
  TopologyRegistry topology{};
  SourceRegistry sources{};
  EvidenceStore evidence{sources};
  EpisodeTracker episodes{};
  auto report = store.load(topology, sources, evidence, episodes, kNow);
  LO_REQUIRE(report.ok());
  LO_CHECK(!report.value().opened);
  LO_CHECK(!report.value().detail.empty());
}
