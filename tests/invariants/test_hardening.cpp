// Hardening suite: deliberate attempts to break the runtime.
//
// Every case here was written after the runtime already passed its normal
// suites. The goal is to make it misbehave: random structural mutation of the
// persisted journal, random evidence, repeated restarts, and a query storm
// across every read path while writes are in flight.

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "fixtures.hpp"
#include "loss_observatory/persist.hpp"
#include "loss_observatory/report.hpp"
#include "loss_observatory/script.hpp"
#include "loss_observatory/testkit/testkit.hpp"

using namespace loss_observatory;

namespace {

const Timestamp kNow = Timestamp::from_unix_seconds(9000);

const char* kDeclarations =
    "source id=h1 name=hardening kind=counter-telemetry authority=primary\n"
    "incarnation source=h1 epoch=E1 name=boot at=2026-01-01T00:00:00Z active=true\n"
    "path id=p1 kind=synthetic hops=n1:0->n2:0,n2:0->n3:0\n"
    "queue id=q1 node=n2 port=0 priority=1 kind=egress-port-queue\n"
    "flow id=f1 src=n1:0 dst=n3:0 proto=tcp path=p1 gen=7\n";

std::string samples_text(std::uint64_t sequence_base, std::uint64_t value_base, std::size_t count) {
  std::string text;
  for (std::size_t i = 0; i < count; ++i) {
    text += "counter source=h1 epoch=E1 gen=7 seq=" + std::to_string(sequence_base + i) +
            " counter=c1 scope=dropped-packets value=" + std::to_string(value_base + i * 5) +
            " bits=32 class=flow subject=flow:f1\n";
  }
  return text;
}

EngineConfig hardening_config(const std::string& name) {
  EngineConfig config = lofixture::default_config(name);
  config.loss.freshness.max_age = Duration::from_seconds(100000);
  return config;
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

}  // namespace

LO_TEST(hardening, mutated_journals_never_crash_recovery) {
  const std::string directory = lofixture::scratch_directory("hardening-mutation");
  const std::string journal = (std::filesystem::path(directory) / "mutant.loj").string();

  PersistConfig persist_config{};
  persist_config.enabled = true;
  persist_config.directory = directory;
  persist_config.instance = "mutant";
  {
    PersistenceStore store{persist_config};
    LO_REQUIRE(store.open_session(kNow, "boot").ok());
    for (std::uint64_t i = 1; i <= 6; ++i) {
      EvidenceItem item{};
      item.header.id = MeasurementId::from_value(4000 + i);
      item.header.source = SourceId::from_canonical_text("h1");
      item.header.epoch = EpochId::from_canonical_text("E1");
      item.header.generation = GenerationId::from_canonical_text("7");
      item.header.source_sequence = SequenceId::from_value(i);
      item.header.observed_at.value = kNow;
      item.header.received_at.value = kNow;
      item.header.method = MeasurementMethod::CounterDelta;
      item.header.subject = SubjectRef::flow(FlowId::from_canonical_text("f1"));
      item.header.granularity = Granularity::Flow;
      CounterSample sample{};
      sample.counter = CounterId::from_canonical_text("c1");
      sample.scope = CounterScope::DroppedPackets;
      sample.value = i * 10;
      sample.width_bits = 32;
      item.payload = sample;
      LO_REQUIRE(store.append_evidence(item).ok());
    }
    LO_REQUIRE(store.flush().ok());
    LO_REQUIRE(store.close().ok());
  }
  const std::vector<std::uint8_t> pristine = read_bytes(journal);
  LO_REQUIRE(pristine.size() > 64ULL);

  testkit::DeterministicRng rng(20260101);
  for (int trial = 0; trial < 60; ++trial) {
    std::vector<std::uint8_t> mutated = pristine;
    const std::size_t mutations = 1 + static_cast<std::size_t>(rng.next_below(8));
    for (std::size_t i = 0; i < mutations; ++i) {
      const std::size_t offset = static_cast<std::size_t>(rng.next_below(mutated.size()));
      mutated[offset] = static_cast<std::uint8_t>(rng.next_below(256));
    }
    write_bytes(journal, mutated);

    TopologyRegistry topology{};
    SourceRegistry sources{};
    EvidenceStore evidence{sources};
    EpisodeTracker episodes{};
    PersistenceStore reader{persist_config};
    auto report = reader.load(topology, sources, evidence, episodes, kNow);
    // Recovery must always answer, never crash, and never invent evidence.
    LO_REQUIRE(report.ok());
    LO_CHECK(evidence.item_count() <= 6ULL);
    if (report.value().truncated) {
      LO_CHECK(!report.value().detail.empty());
    }
    for (const EvidenceItem& item : evidence.snapshot()) {
      LO_CHECK(item.header.recovered_from_persistence);
    }
  }

  // The store must still be usable after every one of those mutations.
  PersistenceStore recovered{persist_config};
  LO_REQUIRE(recovered.open_session(kNow, "boot-2").ok());
  EvidenceItem extra{};
  extra.header.id = MeasurementId::from_value(9999);
  extra.header.source = SourceId::from_canonical_text("h1");
  extra.header.epoch = EpochId::from_canonical_text("E1");
  extra.header.source_sequence = SequenceId::from_value(1);
  extra.header.observed_at.value = kNow;
  extra.header.received_at.value = kNow;
  extra.header.method = MeasurementMethod::CounterDelta;
  extra.header.subject = SubjectRef::flow(FlowId::from_canonical_text("f1"));
  extra.header.granularity = Granularity::Flow;
  CounterSample sample{};
  sample.counter = CounterId::from_canonical_text("c1");
  sample.scope = CounterScope::DroppedPackets;
  sample.value = 5;
  sample.width_bits = 32;
  extra.payload = sample;
  LO_REQUIRE(recovered.append_evidence(extra).ok());
  LO_REQUIRE(recovered.flush().ok());
  LO_REQUIRE(recovered.close().ok());
  lofixture::remove_directory(directory);
}

LO_TEST(hardening, random_evidence_is_always_accepted_or_explained) {
  ManualClock clock{kNow};
  auto engine = lofixture::make_engine(clock, hardening_config("hardening-ingest"));
  run_script_ok(*engine, kDeclarations);
  auto incarnation = engine->sources().current_incarnation(SourceId::from_canonical_text("h1"));
  LO_REQUIRE(incarnation.ok());

  testkit::DeterministicRng rng(4242);
  const FlowId flow = FlowId::from_canonical_text("f1");
  std::size_t accepted = 0;
  std::size_t rejected = 0;
  for (std::uint64_t i = 0; i < 500; ++i) {
    EvidenceItem item{};
    item.header.id = MeasurementId::from_value(50000 + i);
    item.header.source = (rng.next_bool(90)) ? SourceId::from_canonical_text("h1")
                                             : SourceId::from_canonical_text("ghost");
    item.header.epoch = (rng.next_bool(90)) ? incarnation.value().epoch
                                            : EpochId::from_canonical_text("other");
    item.header.generation = GenerationId::from_canonical_text("7");
    item.header.source_sequence = SequenceId::from_value(i + 1);
    item.header.observed_at.value = kNow - Duration::from_millis(static_cast<std::int64_t>(rng.next_below(500)));
    item.header.received_at.value = item.header.observed_at.value;
    item.header.method = static_cast<MeasurementMethod>(rng.next_below(7));
    item.header.subject = (rng.next_bool(50)) ? SubjectRef::flow(flow)
                                              : SubjectRef::queue(QueueId::from_canonical_text("q1"));
    item.header.granularity = static_cast<Granularity>(1 + rng.next_below(5));
    switch (rng.next_below(4)) {
      case 0: {
        CounterSample sample{};
        sample.counter = CounterId::from_canonical_text("c" + std::to_string(rng.next_below(3)));
        sample.scope = static_cast<CounterScope>(1 + rng.next_below(7));
        sample.value = rng.next_u64() >> (rng.next_below(40));
        sample.width_bits = static_cast<std::uint8_t>((rng.next_below(4) == 0) ? 17 : 32);
        item.payload = sample;
        break;
      }
      case 1: {
        ProbeReport report{};
        report.probe = ProbeId::from_canonical_text("probe");
        report.sent = static_cast<std::uint32_t>(rng.next_below(1000));
        report.received = static_cast<std::uint32_t>(rng.next_below(1000));
        item.payload = report;
        break;
      }
      case 2: {
        SequenceReport report{};
        report.lowest_sequence = rng.next_below(1000);
        report.highest_sequence = report.lowest_sequence + rng.next_below(1000);
        report.received_count = rng.next_below(1000);
        report.sequence_restart = rng.next_bool(20);
        item.payload = report;
        break;
      }
      default: {
        EndpointReport report{};
        report.role = static_cast<EndpointRole>(1 + rng.next_below(2));
        report.node = NodeId::from_canonical_text("n" + std::to_string(rng.next_below(3)));
        report.port = PortId::from_canonical_text("0");
        report.count = rng.next_below(1000);
        report.counter_reset = rng.next_bool(10);
        item.payload = report;
        break;
      }
    }
    auto outcome = engine->ingest(item);
    LO_REQUIRE(outcome.ok());
    if (outcome.value().accepted) {
      ++accepted;
    } else {
      ++rejected;
      // A refusal always says why.
      LO_CHECK(outcome.value().reason != RejectionReason::None);
      LO_CHECK(!outcome.value().detail.empty() || outcome.value().reason != RejectionReason::Malformed);
    }
  }
  LO_CHECK_EQ(accepted + rejected, 500ULL);
  LO_CHECK(accepted > 0ULL);
  LO_CHECK(rejected > 0ULL);

  // Whatever survived must classify without tripping any invariant.
  auto classification = engine->classify_flow(flow, kNow);
  LO_REQUIRE(classification.ok());
  LO_CHECK(classification.value().ratio_bp <= 10000U);
  LO_CHECK(testkit::reasons_are_renderable(classification.value().reasons));
  auto derivation = engine->derive_all();
  LO_REQUIRE(derivation.ok());
  for (const LossObservation& observation : derivation.value().observations) {
    LO_CHECK(observation.ratio_bp <= 10000U);
    if (observation.validity != ObservationValidity::Valid) {
      LO_CHECK_EQ(observation.lost, 0ULL);
    }
  }
}

LO_TEST(hardening, repeated_restarts_do_not_accumulate_or_revive_state) {
  const std::string directory = lofixture::scratch_directory("hardening-restart");
  ManualClock clock{kNow};
  EngineConfig config = lofixture::persistent_config(directory, "hardening");
  config.loss.freshness.max_age = Duration::from_seconds(100000);
  config.auto_compact = true;
  config.persist.limits.max_journal_bytes = 64ULL * 1024ULL;

  std::uint64_t previous_evidence = 0;
  for (std::size_t session = 0; session < 8; ++session) {
    ObservatoryEngine engine{config, clock};
    LO_REQUIRE(engine.start().ok());
    auto recovery = engine.load();
    LO_REQUIRE(recovery.ok());
    // Recovery never invents evidence.
    LO_CHECK(engine.evidence().item_count() >= previous_evidence || session == 0);
    LO_CHECK_EQ(engine.episodes().open_episodes().size(), 0ULL);

    run_script_ok(engine, kDeclarations);
    run_script_ok(engine, samples_text(1 + session * 10, 1000 + session * 100, 4));
    auto classification = engine.classify_flow(FlowId::from_canonical_text("f1"), kNow);
    LO_REQUIRE(classification.ok());
    LO_CHECK(classification.value().klass == LossClass::ConfirmedLoss);
    // Four readings produce three intervals per session. Exactly the current
    // session's three are fresh: every interval that touches a recovered
    // reading, including the one spanning the restart, is not current.
    LO_CHECK_EQ(classification.value().freshness.fresh, 3ULL);
    LO_CHECK_EQ(classification.value().freshness.not_current, session * 4ULL);
    LO_CHECK_EQ(classification.value().freshness.fresh + classification.value().freshness.not_current,
                session * 4ULL + 3ULL);

    LO_REQUIRE(engine.persist().ok());
    LO_REQUIRE(engine.stop().ok());
    previous_evidence = engine.evidence().item_count();
  }

  // The journal stayed bounded across eight sessions with compaction enabled.
  std::error_code error;
  const auto size = std::filesystem::file_size(std::filesystem::path(directory) / "hardening.loj", error);
  LO_REQUIRE(!error);
  LO_CHECK(size <= config.persist.limits.max_journal_bytes * 2ULL);
  lofixture::remove_directory(directory);
}

LO_TEST(hardening, a_query_storm_during_ingest_stays_consistent) {
  ManualClock clock{kNow};
  EngineConfig config = hardening_config("hardening-storm");
  config.worker_count = 4;
  config.inbox_capacity = 64;
  auto engine = lofixture::make_engine(clock, config);
  run_script_ok(*engine, kDeclarations);

  std::atomic<bool> stop{false};
  std::atomic<std::size_t> queries{0};
  std::atomic<std::size_t> failures{0};
  const FlowId flow = FlowId::from_canonical_text("f1");
  const SubjectRef queue_subject = SubjectRef::queue(QueueId::from_canonical_text("q1"));

  std::vector<std::thread> readers;
  readers.reserve(4);
  for (int worker = 0; worker < 4; ++worker) {
    readers.emplace_back([&, worker] {
      while (!stop.load(std::memory_order_acquire)) {
        const bool ok = [&] {
          switch (worker) {
            case 0:
              return engine->classify_flow(flow, clock.now()).ok();
            case 1:
              return engine->classify(queue_subject, clock.now()).ok();
            case 2: {
              LocalizationRequest request{};
              request.flow = flow;
              request.at = clock.now();
              return engine->localize(request).ok();
            }
            default: {
              ExplainRequest request{};
              request.subject = SubjectRef::flow(flow);
              request.at = clock.now();
              return engine->explain(request).ok();
            }
          }
        }();
        if (!ok) {
          failures.fetch_add(1, std::memory_order_acq_rel);
        }
        queries.fetch_add(1, std::memory_order_acq_rel);
      }
    });
  }

  for (std::uint64_t round = 0; round < 8; ++round) {
    auto outcomes = engine->ingest_batch(
        [&] {
          std::vector<EvidenceItem> items;
          auto incarnation = engine->sources().current_incarnation(SourceId::from_canonical_text("h1"));
          const QueueId queue = QueueId::from_canonical_text("q1");
          for (std::uint64_t i = 0; i < 16; ++i) {
            EvidenceItem item{};
            item.header.id = MeasurementId::from_value(60000 + round * 100 + i);
            item.header.source = SourceId::from_canonical_text("h1");
            item.header.epoch = incarnation.value().epoch;
            item.header.generation = GenerationId::from_canonical_text("7");
            item.header.source_sequence = SequenceId::from_value(round * 100 + i + 1);
            item.header.observed_at.value = clock.now() - Duration::from_millis(1);
            item.header.received_at.value = clock.now() - Duration::from_millis(1);
            item.header.method = MeasurementMethod::CounterDelta;
            item.header.subject = queue_subject;
            item.header.granularity = Granularity::Queue;
            CounterSample sample{};
            sample.counter = CounterId::from_canonical_text("c-storm");
            sample.scope = CounterScope::DroppedPackets;
            sample.value = 1000 + (round * 16 + i) * 4;
            sample.width_bits = 64;
            sample.queue = queue;
            item.payload = sample;
            items.push_back(item);
          }
          return items;
        }());
    LO_REQUIRE(outcomes.ok());
  }

  stop.store(true, std::memory_order_release);
  for (std::thread& reader : readers) {
    reader.join();
  }
  LO_CHECK_EQ(failures.load(), 0ULL);
  LO_CHECK(queries.load() > 0ULL);
  LO_REQUIRE(engine->stop().ok());
  LO_CHECK_EQ(engine->evidence().item_count(), 128ULL);
}

LO_TEST(hardening, a_completely_random_journal_is_refused_not_interpreted) {
  const std::string directory = lofixture::scratch_directory("hardening-garbage");
  const std::string journal = (std::filesystem::path(directory) / "garbage.loj").string();
  PersistConfig persist_config{};
  persist_config.enabled = true;
  persist_config.directory = directory;
  persist_config.instance = "garbage";

  testkit::DeterministicRng rng(777);
  for (int trial = 0; trial < 20; ++trial) {
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(64 + rng.next_below(4096)));
    for (std::uint8_t& byte : bytes) {
      byte = static_cast<std::uint8_t>(rng.next_below(256));
    }
    write_bytes(journal, bytes);

    TopologyRegistry topology{};
    SourceRegistry sources{};
    EvidenceStore evidence{sources};
    EpisodeTracker episodes{};
    PersistenceStore reader{persist_config};
    auto report = reader.load(topology, sources, evidence, episodes, kNow);
    LO_REQUIRE(report.ok());
    // Nothing is ever recovered from a file that does not begin with a valid
    // manifest: the reader refuses rather than guessing at the bytes.
    LO_CHECK_EQ(report.value().records_accepted, 0ULL);
    LO_CHECK_EQ(evidence.item_count(), 0ULL);
    LO_CHECK_EQ(topology.link_count() + topology.path_count() + topology.flow_count() +
                    topology.queue_count(),
                0ULL);
  }
  lofixture::remove_directory(directory);
}

LO_TEST(hardening, an_engine_can_be_stopped_and_started_repeatedly) {
  ManualClock clock{kNow};
  EngineConfig config = hardening_config("hardening-lifecycle");
  config.worker_count = 2;
  ObservatoryEngine engine{config, clock};
  for (int cycle = 0; cycle < 5; ++cycle) {
    LO_REQUIRE(engine.start().ok());
    LO_CHECK(engine.state() == EngineState::Running);
    run_script_ok(engine, kDeclarations);
    run_script_ok(engine, samples_text(1 + static_cast<std::uint64_t>(cycle) * 10, 500, 4));
    auto classification = engine.classify_flow(FlowId::from_canonical_text("f1"), kNow);
    LO_REQUIRE(classification.ok());
    LO_CHECK(classification.value().klass == LossClass::ConfirmedLoss);
    LO_REQUIRE(engine.stop().ok());
    LO_CHECK(engine.state() == EngineState::Stopped);
    // Ingest after shutdown is refused rather than silently dropped.
    EvidenceItem after_stop{};
    after_stop.header.id = MeasurementId::from_value(1);
    auto refused = engine.ingest(after_stop);
    LO_CHECK(!refused.ok());
    LO_CHECK(refused.code() == StatusCode::ShuttingDown);
  }
}

LO_TEST(hardening, extreme_subject_and_source_pressure_is_bounded) {
  ManualClock clock{kNow};
  EngineConfig config = hardening_config("hardening-pressure");
  config.store.max_distinct_subjects = 8;
  config.topology.max_queues = 32;
  auto engine = lofixture::make_engine(clock, config);
  run_script_ok(*engine, kDeclarations);
  auto incarnation = engine->sources().current_incarnation(SourceId::from_canonical_text("h1"));
  LO_REQUIRE(incarnation.ok());

  // Declare far more queues than the store will accept subjects for.
  std::string declaration;
  for (int i = 0; i < 24; ++i) {
    declaration += "queue id=hq" + std::to_string(i) + " node=n2 port=0 priority=1 kind=egress-port-queue\n";
  }
  auto program = parse_script(declaration);
  LO_REQUIRE(program.ok());
  const ScriptRunResult declared = apply_script(*engine, program.value());
  LO_CHECK(declared.applied > 0ULL);

  std::size_t refused = 0;
  for (std::uint64_t i = 0; i < 24; ++i) {
    EvidenceItem item{};
    item.header.id = MeasurementId::from_value(70000 + i);
    item.header.source = SourceId::from_canonical_text("h1");
    item.header.epoch = incarnation.value().epoch;
    item.header.generation = GenerationId::from_canonical_text("7");
    item.header.source_sequence = SequenceId::from_value(i + 1);
    item.header.observed_at.value = kNow;
    item.header.received_at.value = kNow;
    item.header.method = MeasurementMethod::CounterDelta;
    const std::string queue_name = "hq" + std::to_string(i);
    item.header.subject = SubjectRef::queue(QueueId::from_canonical_text(queue_name));
    item.header.granularity = Granularity::Queue;
    CounterSample sample{};
    sample.counter = CounterId::from_canonical_text("c-pressure");
    sample.scope = CounterScope::DroppedPackets;
    sample.value = i;
    sample.width_bits = 32;
    sample.queue = QueueId::from_canonical_text(queue_name);
    item.payload = sample;
    auto outcome = engine->ingest(item);
    LO_REQUIRE(outcome.ok());
    if (!outcome.value().accepted) {
      ++refused;
      LO_CHECK(outcome.value().reason == RejectionReason::StoreCapacity);
    }
  }
  LO_CHECK(refused > 0ULL);
  LO_CHECK(engine->evidence().subject_count() <= 8ULL);
  LO_CHECK(!engine->evidence().bound_notes().empty());
}
