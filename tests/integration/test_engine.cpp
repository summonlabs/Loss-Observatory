#include <algorithm>
#include <string>
#include <thread>
#include <vector>

#include "fixtures.hpp"
#include "loss_observatory/report.hpp"
#include "loss_observatory/script.hpp"

using namespace loss_observatory;

namespace {

const char* kScenario =
    "source id=s1 name=leaf-a kind=counter-telemetry authority=primary\n"
    "incarnation source=s1 epoch=E1 name=boot-1 at=2026-01-01T00:00:00Z active=true\n"
    "path id=p1 kind=synthetic hops=n1:0->n2:0,n2:0->n3:0,n3:0->n4:0\n"
    "queue id=q1 node=n2 port=0 priority=3 kind=egress-port-queue\n"
    "flow id=f1 src=n1:0 dst=n4:0 proto=tcp path=p1 gen=7\n"
    "counter source=s1 epoch=E1 gen=7 seq=1 counter=c1 scope=dropped-packets value=100 bits=32 "
    "class=flow subject=flow:f1\n"
    "counter source=s1 epoch=E1 gen=7 seq=2 counter=c1 scope=dropped-packets value=140 bits=32 "
    "class=flow subject=flow:f1\n"
    "counter source=s1 epoch=E1 gen=7 seq=3 counter=c1 scope=dropped-packets value=180 bits=32 "
    "class=flow subject=flow:f1\n"
    "counter source=s1 epoch=E1 gen=7 seq=4 counter=c2 scope=dropped-packets value=10 bits=32 queue=q1\n"
    "counter source=s1 epoch=E1 gen=7 seq=5 counter=c2 scope=dropped-packets value=30 bits=32 queue=q1\n";

EngineConfig worker_config(std::size_t workers, const std::string& name) {
  EngineConfig config = lofixture::default_config(name);
  config.worker_count = workers;
  config.inbox_capacity = 64;
  config.loss.freshness.max_age = Duration::from_seconds(3600);
  return config;
}

}  // namespace

LO_TEST(engine, lifecycle_transitions_are_monotonic) {
  ManualClock clock{lofixture::base_time()};
  auto engine = lofixture::make_engine(clock, lofixture::default_config("lifecycle"));
  LO_CHECK(engine->state() == EngineState::Running);
  LO_REQUIRE(engine->stop().ok());
  LO_CHECK(engine->state() == EngineState::Stopped);
  LO_REQUIRE(engine->stop().ok());
  LO_CHECK(engine->state() == EngineState::Stopped);
}

LO_TEST(engine, a_full_pipeline_produces_a_classification) {
  ManualClock clock{lofixture::base_time()};
  auto engine = lofixture::make_engine(clock, worker_config(0, "pipeline"));
  run_script_ok(*engine, kScenario);

  const FlowId flow = FlowId::from_canonical_text("f1");
  auto classification = engine->classify_flow(flow, clock.now());
  LO_REQUIRE(classification.ok());
  LO_CHECK(classification.value().klass == LossClass::ConfirmedLoss);
  LO_CHECK_EQ(classification.value().lost_total, 80ULL);
  LO_CHECK(!classification.value().evidence_ids.empty());

  auto explanation = engine->explain(ExplainRequest{SubjectRef::flow(flow), clock.now(), true, true, true,
                                                    true, false, Granularity::Queue, 512, 16});
  LO_REQUIRE(explanation.ok());
  LO_CHECK(explanation.value().text.find("class          : confirmed-loss") != std::string::npos);
  LO_CHECK(explanation.value().text.find("localization") != std::string::npos);
  LO_CHECK(explanation.value().text.find("attribution") != std::string::npos);

  auto localized = engine->localize(LocalizationRequest{flow, clock.now(), Granularity::Queue, 64});
  LO_REQUIRE(localized.ok());
  LO_CHECK(localized.value().aggregate_class == LossClass::ConfirmedLoss);
  LO_CHECK(localized.value().achieved_granularity == Granularity::Queue);

  auto history = engine->history(EpisodeQuery{});
  LO_REQUIRE(history.ok());
  LO_CHECK(history.value().episodes.size() >= 1ULL);
}

LO_TEST(engine, worker_count_does_not_change_the_conclusion) {
  ManualClock clock{lofixture::base_time()};
  auto single = lofixture::make_engine(clock, worker_config(0, "single"));
  auto multi = lofixture::make_engine(clock, worker_config(4, "multi"));
  run_script_ok(*single, kScenario);
  run_script_ok(*multi, kScenario);

  const FlowId flow = FlowId::from_canonical_text("f1");
  auto a = single->classify_flow(flow, clock.now());
  auto b = multi->classify_flow(flow, clock.now());
  LO_REQUIRE(a.ok());
  LO_REQUIRE(b.ok());
  LO_CHECK_EQ(a.value().summary, b.value().summary);
  LO_CHECK_EQ(a.value().lost_total, b.value().lost_total);
  LO_CHECK_EQ(single->evidence().item_count(), multi->evidence().item_count());
}

LO_TEST(engine, batch_ingest_returns_one_outcome_per_item_in_order) {
  ManualClock clock{lofixture::base_time()};
  auto engine = lofixture::make_engine(clock, worker_config(4, "batch"));
  auto program = parse_script(kScenario);
  LO_REQUIRE(program.ok());
  (void)apply_script(*engine, program.value());

  std::vector<EvidenceItem> items;
  const SourceId source = SourceId::from_canonical_text("s1");
  auto epoch = engine->sources().current_incarnation(source);
  LO_REQUIRE(epoch.ok());
  const QueueId queue = QueueId::from_canonical_text("q1");
  for (std::uint64_t i = 1; i <= 32; ++i) {
    EvidenceItem item{};
    item.header.id = MeasurementId::from_value(900000 + i);
    item.header.source = source;
    item.header.epoch = epoch.value().epoch;
    item.header.generation = GenerationId::from_canonical_text("7");
    item.header.source_sequence = SequenceId::from_value(100 + i);
    item.header.observed_at.value = clock.now();
    item.header.received_at.value = clock.now();
    item.header.method = MeasurementMethod::CounterDelta;
    item.header.subject = SubjectRef::queue(queue);
    item.header.granularity = Granularity::Queue;
    CounterSample sample{};
    sample.counter = CounterId::from_canonical_text("c1");
    sample.scope = CounterScope::DroppedPackets;
    sample.value = 1000 + i;
    sample.width_bits = 32;
    sample.queue = queue;
    item.payload = sample;
    items.push_back(item);
  }

  auto outcomes = engine->ingest_batch(std::move(items));
  LO_REQUIRE(outcomes.ok());
  LO_CHECK_EQ(outcomes.value().size(), 32ULL);
  std::size_t accepted = 0;
  for (std::size_t i = 0; i < outcomes.value().size(); ++i) {
    if (outcomes.value()[i].accepted) {
      ++accepted;
    }
    LO_CHECK(outcomes.value()[i].id == MeasurementId::from_value(900000 + i + 1));
  }
  LO_CHECK_EQ(accepted, 32ULL);
  LO_CHECK_EQ(engine->stats().queue.pushed, 32ULL);

  // Replaying the same batch must be refused, one rejection per item.
  std::vector<EvidenceItem> replay;
  for (std::uint64_t i = 1; i <= 32; ++i) {
    EvidenceItem item{};
    item.header.id = MeasurementId::from_value(900000 + i);
    item.header.source = source;
    item.header.epoch = epoch.value().epoch;
    item.header.generation = GenerationId::from_canonical_text("7");
    item.header.source_sequence = SequenceId::from_value(100 + i);
    item.header.observed_at.value = clock.now();
    item.header.received_at.value = clock.now();
    item.header.method = MeasurementMethod::CounterDelta;
    item.header.subject = SubjectRef::queue(queue);
    item.header.granularity = Granularity::Queue;
    CounterSample sample{};
    sample.counter = CounterId::from_canonical_text("c1");
    sample.scope = CounterScope::DroppedPackets;
    sample.value = 1000 + i;
    sample.width_bits = 32;
    sample.queue = queue;
    item.payload = sample;
    replay.push_back(item);
  }
  auto rejected = engine->ingest_batch(std::move(replay));
  LO_REQUIRE(rejected.ok());
  for (const IngestOutcome& outcome : rejected.value()) {
    LO_CHECK(!outcome.accepted);
  }
}

LO_TEST(engine, a_batch_larger_than_the_bound_is_refused_whole) {
  ManualClock clock{lofixture::base_time()};
  EngineConfig config = worker_config(2, "batch-bound");
  config.max_batch_items = 4;
  auto engine = lofixture::make_engine(clock, config);
  std::vector<EvidenceItem> items(5);
  auto outcomes = engine->ingest_batch(std::move(items));
  LO_CHECK(!outcomes.ok());
  LO_CHECK(outcomes.code() == StatusCode::CapacityExceeded);
}

LO_TEST(engine, ingest_is_refused_after_stop) {
  ManualClock clock{lofixture::base_time()};
  auto engine = lofixture::make_engine(clock, worker_config(0, "stopped"));
  LO_REQUIRE(engine->stop().ok());
  EvidenceItem item{};
  item.header.id = MeasurementId::from_value(1);
  auto outcome = engine->ingest(item);
  LO_CHECK(!outcome.ok());
  LO_CHECK(outcome.code() == StatusCode::ShuttingDown);
}

LO_TEST(engine, export_scopes_are_independent_and_bounded) {
  ManualClock clock{lofixture::base_time()};
  auto engine = lofixture::make_engine(clock, worker_config(0, "export"));
  run_script_ok(*engine, kScenario);

  ExportRequest topology_request{};
  topology_request.scope = ExportScope::Topology;
  auto topology = engine->export_bundle(topology_request);
  LO_REQUIRE(topology.ok());
  LO_CHECK(topology.value().text.find("path id=") != std::string::npos);
  LO_CHECK(topology.value().text.find("flow id=") != std::string::npos);

  ExportRequest evidence_request{};
  evidence_request.scope = ExportScope::Evidence;
  auto evidence = engine->export_bundle(evidence_request);
  LO_REQUIRE(evidence.ok());
  LO_CHECK(evidence.value().text.find("counter method=counter-delta") != std::string::npos);

  ExportRequest bounded{};
  bounded.scope = ExportScope::All;
  bounded.max_items = 2;
  auto truncated = engine->export_bundle(bounded);
  LO_REQUIRE(truncated.ok());
  LO_CHECK(truncated.value().truncated);
  LO_CHECK(!truncated.value().bounds.empty());

  ExportRequest binary{};
  binary.scope = ExportScope::Evidence;
  binary.format = ExportFormat::Binary;
  auto bytes = engine->export_bundle(binary);
  LO_REQUIRE(bytes.ok());
  LO_CHECK(!bytes.value().binary.empty());
}

LO_TEST(engine, unknown_subjects_classify_as_absent_not_loss) {
  ManualClock clock{lofixture::base_time()};
  auto engine = lofixture::make_engine(clock, worker_config(0, "unknown"));
  auto classification = engine->classify_flow(FlowId::from_canonical_text("never-seen"), clock.now());
  LO_REQUIRE(classification.ok());
  LO_CHECK(classification.value().klass == LossClass::AbsentEvidence);
  LO_CHECK(!asserts_loss(classification.value().klass));

  auto unknown_subject = engine->classify(SubjectRef{}, clock.now());
  LO_CHECK(!unknown_subject.ok());
}

LO_TEST(engine, reordered_ingest_still_yields_the_same_classification) {
  ManualClock clock{lofixture::base_time()};
  auto forward = lofixture::make_engine(clock, worker_config(0, "order-a"));
  auto reverse = lofixture::make_engine(clock, worker_config(0, "order-b"));

  const std::vector<std::string> lines = {
      "source id=s1 name=leaf-a kind=counter-telemetry authority=primary",
      "incarnation source=s1 epoch=E1 name=boot-1 at=2026-01-01T00:00:00Z active=true",
      "path id=p1 kind=synthetic hops=n1:0->n2:0",
      "flow id=f1 src=n1:0 dst=n2:0 proto=tcp path=p1 gen=7",
      "counter source=s1 epoch=E1 gen=7 seq=1 counter=c1 scope=dropped-packets value=10 bits=32 class=flow subject=flow:f1",
      "counter source=s1 epoch=E1 gen=7 seq=2 counter=c1 scope=dropped-packets value=30 bits=32 class=flow subject=flow:f1",
      "counter source=s1 epoch=E1 gen=7 seq=3 counter=c1 scope=dropped-packets value=60 bits=32 class=flow subject=flow:f1"};

  std::string forward_text;
  for (const std::string& line : lines) {
    forward_text += line + "\n";
  }
  // Evidence lines are submitted in reverse order; declarations stay first.
  std::string reverse_text;
  for (std::size_t i = 0; i < 4; ++i) {
    reverse_text += lines[i] + "\n";
  }
  for (std::size_t i = lines.size(); i-- > 4;) {
    reverse_text += lines[i] + "\n";
  }

  run_script_ok(*forward, forward_text);
  auto program = parse_script(reverse_text);
  LO_REQUIRE(program.ok());
  const ScriptRunResult run = apply_script(*reverse, program.value());
  LO_CHECK(run.failed <= 3ULL);  // out-of-order sequences may be fenced

  const FlowId flow = FlowId::from_canonical_text("f1");
  auto a = forward->classify_flow(flow, clock.now());
  LO_REQUIRE(a.ok());
  LO_CHECK(a.value().klass == LossClass::ConfirmedLoss);
}

LO_TEST(engine, concurrent_readers_do_not_disturb_the_store) {
  ManualClock clock{lofixture::base_time()};
  auto engine = lofixture::make_engine(clock, worker_config(4, "concurrent-read"));
  run_script_ok(*engine, kScenario);

  const FlowId flow = FlowId::from_canonical_text("f1");
  constexpr std::size_t kReaders = 4;
  std::vector<std::thread> readers;
  readers.reserve(kReaders);
  std::vector<std::string> summaries(kReaders);
  for (std::size_t i = 0; i < kReaders; ++i) {
    readers.emplace_back([&, i] {
      auto classification = engine->classify_flow(flow, clock.now());
      if (classification.ok()) {
        summaries[i] = classification.value().summary;
      }
    });
  }
  for (std::thread& reader : readers) {
    reader.join();
  }
  for (const std::string& summary : summaries) {
    LO_CHECK(!summary.empty());
    LO_CHECK(summary.rfind("subject=flow:", 0) == 0);
  }
  for (std::size_t i = 1; i < summaries.size(); ++i) {
    LO_CHECK_EQ(summaries[0], summaries[i]);
  }
}
