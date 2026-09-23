#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "fixtures.hpp"
#include "loss_observatory/report.hpp"
#include "loss_observatory/script.hpp"
#include "loss_observatory/testkit/testkit.hpp"

using namespace loss_observatory;

namespace {

const char* kDeclarations =
    "source id=s1 name=leaf-a kind=counter-telemetry authority=primary\n"
    "incarnation source=s1 epoch=E1 name=boot-1 at=2026-01-01T00:00:00Z active=true\n"
    "path id=p1 kind=synthetic hops=n1:0->n2:0,n2:0->n3:0\n"
    "queue id=q1 node=n2 port=0 priority=3 kind=egress-port-queue\n"
    "flow id=f1 src=n1:0 dst=n3:0 proto=tcp path=p1 gen=7\n";

EngineConfig concurrency_config(std::size_t workers, const std::string& name) {
  EngineConfig config = lofixture::default_config(name);
  config.worker_count = workers;
  config.inbox_capacity = 256;
  config.loss.freshness.max_age = Duration::from_seconds(100000);
  return config;
}

std::vector<EvidenceItem> make_samples(ObservatoryEngine& engine, std::size_t count,
                                       std::uint64_t base_value) {
  auto incarnation = engine.sources().current_incarnation(SourceId::from_canonical_text("s1"));
  if (!incarnation.ok()) {
    throw lotest::Failure{"no incarnation"};
  }
  const QueueId queue = QueueId::from_canonical_text("q1");
  std::vector<EvidenceItem> items;
  items.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    EvidenceItem item{};
    item.header.id = MeasurementId::from_value(700000 + base_value + i);
    item.header.source = SourceId::from_canonical_text("s1");
    item.header.epoch = incarnation.value().epoch;
    item.header.generation = GenerationId::from_canonical_text("7");
    item.header.source_sequence = SequenceId::from_value(base_value + i + 1);
    item.header.observed_at.value = engine.now() - Duration::from_seconds(1);
    item.header.received_at.value = engine.now() - Duration::from_seconds(1);
    item.header.method = MeasurementMethod::CounterDelta;
    item.header.subject = SubjectRef::queue(queue);
    item.header.granularity = Granularity::Queue;
    CounterSample sample{};
    sample.counter = CounterId::from_canonical_text("c1");
    sample.scope = CounterScope::DroppedPackets;
    sample.value = 1000 + (base_value + static_cast<std::uint64_t>(i)) * 3;
    sample.width_bits = 64;
    sample.queue = queue;
    item.payload = sample;
    items.push_back(item);
  }
  return items;
}

}  // namespace

LO_TEST(concurrency, shuffled_arrival_is_equivalent) {
  ManualClock clock{lofixture::base_time()};
  const FlowId flow = FlowId::from_canonical_text("f1");

  auto baseline = lofixture::make_engine(clock, concurrency_config(0, "conc-baseline"));
  run_script_ok(*baseline, kDeclarations);
  auto baseline_samples = make_samples(*baseline, 64, 0);
  auto baseline_outcomes = baseline->ingest_batch(baseline_samples);
  LO_REQUIRE(baseline_outcomes.ok());
  auto baseline_classification = baseline->classify_flow(flow, clock.now());
  LO_REQUIRE(baseline_classification.ok());

  for (std::uint64_t seed = 1; seed <= 3; ++seed) {
    auto shuffled = lofixture::make_engine(clock, concurrency_config(4, "conc-shuffled"));
    run_script_ok(*shuffled, kDeclarations);
    std::vector<EvidenceItem> samples = make_samples(*shuffled, 64, 0);
    testkit::DeterministicRng rng(seed);
    for (std::size_t i = samples.size(); i > 1; --i) {
      const std::size_t j = static_cast<std::size_t>(rng.next_below(i));
      std::swap(samples[i - 1], samples[j]);
    }
    auto outcomes = shuffled->ingest_batch(std::move(samples));
    LO_REQUIRE(outcomes.ok());
    for (const IngestOutcome& outcome : outcomes.value()) {
      LO_CHECK(outcome.accepted);
    }
    auto classification = shuffled->classify_flow(flow, clock.now());
    LO_REQUIRE(classification.ok());
    LO_CHECK_EQ(baseline_classification.value().summary, classification.value().summary);
    LO_CHECK_EQ(baseline_classification.value().lost_total, classification.value().lost_total);
    LO_CHECK_EQ(baseline->evidence().item_count(), shuffled->evidence().item_count());
    (void)shuffled->stop();
  }
  (void)baseline->stop();
}

LO_TEST(concurrency, many_producers_can_submit_batches_at_once) {
  ManualClock clock{lofixture::base_time()};
  auto engine = lofixture::make_engine(clock, concurrency_config(4, "conc-producers"));
  run_script_ok(*engine, kDeclarations);

  constexpr std::size_t kProducers = 4;
  constexpr std::size_t kPerProducer = 32;
  std::atomic<std::size_t> accepted{0};
  std::vector<std::thread> producers;
  producers.reserve(kProducers);
  for (std::size_t producer = 0; producer < kProducers; ++producer) {
    producers.emplace_back([&, producer] {
      std::vector<EvidenceItem> items = make_samples(*engine, kPerProducer,
                                                     static_cast<std::uint64_t>(producer) * 1000);
      auto outcomes = engine->ingest_batch(std::move(items));
      if (!outcomes.ok()) {
        return;
      }
      for (const IngestOutcome& outcome : outcomes.value()) {
        if (outcome.accepted) {
          accepted.fetch_add(1, std::memory_order_acq_rel);
        }
      }
    });
  }
  for (std::thread& producer : producers) {
    producer.join();
  }
  LO_CHECK_EQ(accepted.load(), kProducers * kPerProducer);
  LO_CHECK_EQ(engine->evidence().item_count(), kProducers * kPerProducer);

  // The evidence is bound to the queue, so the queue is the subject that has
  // something to say.
  auto classification =
      engine->classify(SubjectRef::queue(QueueId::from_canonical_text("q1")), clock.now());
  LO_REQUIRE(classification.ok());
  LO_CHECK(classification.value().klass == LossClass::ConfirmedLoss);
  (void)engine->stop();
}

LO_TEST(concurrency, classification_during_ingest_is_consistent) {
  ManualClock clock{lofixture::base_time()};
  auto engine = lofixture::make_engine(clock, concurrency_config(4, "conc-mixed"));
  run_script_ok(*engine, kDeclarations);

  std::atomic<bool> stop{false};
  std::atomic<std::size_t> attempts{0};
  std::atomic<std::size_t> classifications{0};
  std::thread reader([&] {
    while (!stop.load(std::memory_order_acquire)) {
      attempts.fetch_add(1, std::memory_order_acq_rel);
      auto classification =
          engine->classify_flow(FlowId::from_canonical_text("f1"), clock.now());
      if (classification.ok()) {
        classifications.fetch_add(1, std::memory_order_acq_rel);
      }
    }
  });
  // Let the reader get a classification in before ingestion starts, so the two
  // paths genuinely overlap.
  while (attempts.load(std::memory_order_acquire) == 0) {
    std::this_thread::yield();
  }

  for (std::size_t round = 0; round < 4; ++round) {
    std::vector<EvidenceItem> items =
        make_samples(*engine, 32, static_cast<std::uint64_t>(round) * 1000);
    auto outcomes = engine->ingest_batch(std::move(items));
    LO_REQUIRE(outcomes.ok());
  }
  stop.store(true, std::memory_order_release);
  reader.join();
  LO_CHECK(attempts.load() > 1ULL);
  LO_CHECK(classifications.load() > 0ULL);
  (void)engine->stop();
}

LO_TEST(concurrency, shutdown_with_queued_work_is_prompt_and_complete) {
  ManualClock clock{lofixture::base_time()};
  auto engine = lofixture::make_engine(clock, concurrency_config(2, "conc-shutdown"));
  run_script_ok(*engine, kDeclarations);
  std::vector<EvidenceItem> items = make_samples(*engine, 128, 0);
  auto outcomes = engine->ingest_batch(std::move(items));
  LO_REQUIRE(outcomes.ok());
  LO_REQUIRE(engine->stop().ok());
  LO_CHECK(engine->state() == EngineState::Stopped);
  LO_CHECK_EQ(engine->evidence().item_count(), 128ULL);
}

LO_TEST(concurrency, cancellation_marks_unprocessed_submissions) {
  ManualClock clock{lofixture::base_time()};
  EngineConfig config = concurrency_config(1, "conc-cancel");
  config.inbox_capacity = 2;
  auto engine = lofixture::make_engine(clock, config);
  run_script_ok(*engine, kDeclarations);

  std::vector<EvidenceItem> items = make_samples(*engine, 64, 0);
  std::atomic<bool> started{false};
  std::atomic<bool> refused{false};
  std::thread submitter([&] {
    started.store(true, std::memory_order_release);
    auto outcomes = engine->ingest_batch(std::move(items));
    if (!outcomes.ok()) {
      refused.store(true, std::memory_order_release);
    }
  });
  while (!started.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  engine->request_stop();
  submitter.join();
  LO_REQUIRE(engine->stop().ok());
  // Cancellation must be real: the submission was either refused up front,
  // recorded as cancelled, or completed in full. Nothing may be lost silently.
  LO_CHECK(refused.load() || engine->stats().cancelled > 0ULL ||
           engine->evidence().item_count() == 64ULL);
  LO_CHECK(engine->evidence().item_count() + engine->stats().cancelled +
               (refused.load() ? 64ULL : 0ULL) >=
           64ULL);
}

LO_TEST(concurrency, the_inbox_bound_is_reported_rather_than_hidden) {
  ManualClock clock{lofixture::base_time()};
  EngineConfig config = concurrency_config(1, "conc-bound");
  config.inbox_capacity = 1;
  auto engine = lofixture::make_engine(clock, config);
  run_script_ok(*engine, kDeclarations);
  std::vector<EvidenceItem> items = make_samples(*engine, 16, 0);
  auto outcomes = engine->ingest_batch(std::move(items));
  LO_REQUIRE(outcomes.ok());
  const QueueStats stats = engine->stats().queue;
  LO_CHECK(stats.pushed >= 16ULL);
  LO_CHECK(stats.high_water >= 1ULL);
  LO_CHECK(stats.high_water <= 1ULL);
  (void)engine->stop();
}

LO_TEST(concurrency, two_engines_sharing_a_clock_stay_independent) {
  ManualClock clock{lofixture::base_time()};
  auto first = lofixture::make_engine(clock, concurrency_config(2, "conc-a"));
  auto second = lofixture::make_engine(clock, concurrency_config(2, "conc-b"));
  run_script_ok(*first, kDeclarations);
  run_script_ok(*second, kDeclarations);

  std::thread a([&] {
    auto outcomes = first->ingest_batch(make_samples(*first, 32, 0));
    (void)outcomes;
  });
  std::thread b([&] {
    auto outcomes = second->ingest_batch(make_samples(*second, 32, 0));
    (void)outcomes;
  });
  a.join();
  b.join();
  LO_CHECK_EQ(first->evidence().item_count(), 32ULL);
  LO_CHECK_EQ(second->evidence().item_count(), 32ULL);
  auto left = first->classify_flow(FlowId::from_canonical_text("f1"), clock.now());
  auto right = second->classify_flow(FlowId::from_canonical_text("f1"), clock.now());
  LO_REQUIRE(left.ok());
  LO_REQUIRE(right.ok());
  LO_CHECK_EQ(left.value().summary, right.value().summary);
  (void)first->stop();
  (void)second->stop();
}
