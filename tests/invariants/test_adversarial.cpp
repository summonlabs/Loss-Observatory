#include <limits>
#include <string>
#include <vector>

#include "fixtures.hpp"
#include "loss_observatory/report.hpp"
#include "loss_observatory/script.hpp"

using namespace loss_observatory;

namespace {

const Timestamp kNow = Timestamp::from_unix_seconds(8000);

EvidenceItem base_item(std::uint64_t id) {
  EvidenceItem item{};
  item.header.id = MeasurementId::from_value(id);
  item.header.source = SourceId::from_canonical_text("adv");
  item.header.epoch = EpochId::from_canonical_text("adv-epoch");
  item.header.generation = GenerationId::from_canonical_text("gen/1");
  item.header.source_sequence = SequenceId::from_value(id);
  item.header.observed_at.value = kNow - Duration::from_seconds(1);
  item.header.received_at.value = kNow - Duration::from_seconds(1);
  item.header.method = MeasurementMethod::CounterDelta;
  item.header.subject = SubjectRef::queue(QueueId::from_canonical_text("q1"));
  item.header.granularity = Granularity::Queue;
  CounterSample sample{};
  sample.counter = CounterId::from_canonical_text("c1");
  sample.scope = CounterScope::DroppedPackets;
  sample.value = 100;
  sample.width_bits = 32;
  sample.queue = QueueId::from_canonical_text("q1");
  item.payload = sample;
  return item;
}

/// The engine borrows the clock, so both live here together.
struct Armed {
  ManualClock clock{kNow};
  std::unique_ptr<ObservatoryEngine> engine{};
};

std::unique_ptr<Armed> armed_engine(const std::string& name) {
  EngineConfig config = lofixture::default_config(name);
  config.loss.freshness.max_age = Duration::from_seconds(100000);
  auto armed = std::make_unique<Armed>();
  armed->engine = lofixture::make_engine(armed->clock, config);
  ObservatoryEngine* engine = armed->engine.get();
  const std::string text =
      "source id=adv name=adv kind=synthetic authority=primary\n"
      "incarnation source=adv epoch=adv-epoch name=boot at=2026-01-01T00:00:00Z active=true\n"
      "path id=p1 kind=synthetic hops=n1:0->n2:0\n"
      "queue id=q1 node=n1 port=0 priority=1 kind=egress-port-queue\n"
      "flow id=f1 src=n1:0 dst=n2:0 proto=tcp path=p1 gen=1\n";
  auto program = parse_script(text);
  if (!program.ok()) {
    throw lotest::Failure{"adversarial setup did not parse"};
  }
  const ScriptRunResult run = apply_script(*engine, program.value());
  if (!run.ok()) {
    throw lotest::Failure{"adversarial setup failed:\n" + run.render()};
  }
  return armed;
}

EpochId active_epoch(ObservatoryEngine& engine) {
  auto incarnation = engine.sources().current_incarnation(SourceId::from_canonical_text("adv"));
  if (!incarnation.ok()) {
    throw lotest::Failure{"no active incarnation"};
  }
  return incarnation.value().epoch;
}

}  // namespace

LO_TEST(adversarial, structurally_invalid_evidence_is_refused_with_a_reason) {
  auto armed = armed_engine("adv-invalid");
  ObservatoryEngine* engine = armed->engine.get();

  EvidenceItem nil_id = base_item(1);
  nil_id.header.id = MeasurementId{};
  auto outcome = engine->ingest(nil_id);
  LO_REQUIRE(outcome.ok());
  LO_CHECK(!outcome.value().accepted);
  LO_CHECK(outcome.value().reason == RejectionReason::Malformed);

  EvidenceItem nil_subject = base_item(2);
  nil_subject.header.subject = SubjectRef{};
  outcome = engine->ingest(nil_subject);
  LO_REQUIRE(outcome.ok());
  LO_CHECK(!outcome.value().accepted);

  EvidenceItem unknown_method = base_item(3);
  unknown_method.header.method = MeasurementMethod::Unknown;
  outcome = engine->ingest(unknown_method);
  LO_REQUIRE(outcome.ok());
  LO_CHECK(!outcome.value().accepted);

  EvidenceItem unknown_granularity = base_item(4);
  unknown_granularity.header.granularity = Granularity::Unknown;
  outcome = engine->ingest(unknown_granularity);
  LO_REQUIRE(outcome.ok());
  LO_CHECK(!outcome.value().accepted);

  EvidenceItem no_time = base_item(5);
  no_time.header.observed_at.value = Timestamp{};
  outcome = engine->ingest(no_time);
  LO_REQUIRE(outcome.ok());
  LO_CHECK(!outcome.value().accepted);

  EvidenceItem nil_counter = base_item(6);
  std::get<CounterSample>(nil_counter.payload).counter = CounterId{};
  outcome = engine->ingest(nil_counter);
  LO_REQUIRE(outcome.ok());
  LO_CHECK(!outcome.value().accepted);

  EvidenceItem bad_width = base_item(7);
  std::get<CounterSample>(bad_width.payload).width_bits = 17;
  outcome = engine->ingest(bad_width);
  LO_REQUIRE(outcome.ok());
  LO_CHECK(!outcome.value().accepted);
}

LO_TEST(adversarial, evidence_cannot_claim_more_precision_than_its_method_allows) {
  auto armed = armed_engine("adv-granularity");
  ObservatoryEngine* engine = armed->engine.get();
  EvidenceItem item = base_item(10);
  item.header.epoch = active_epoch(*engine);
  item.header.method = MeasurementMethod::SequenceGap;
  item.header.granularity = Granularity::Queue;  // a sequence gap is flow-scoped
  SequenceReport report{};
  report.lowest_sequence = 1;
  report.highest_sequence = 10;
  report.received_count = 9;
  item.payload = report;
  auto outcome = engine->ingest(item);
  LO_REQUIRE(outcome.ok());
  LO_CHECK(!outcome.value().accepted);
  LO_CHECK(outcome.value().reason == RejectionReason::Malformed);
}

LO_TEST(adversarial, oversized_notes_are_refused) {
  auto armed = armed_engine("adv-note");
  ObservatoryEngine* engine = armed->engine.get();
  EvidenceItem item = base_item(11);
  item.header.epoch = active_epoch(*engine);
  item.header.note = std::string(kMaxEvidenceNoteBytes + 1, 'x');
  auto outcome = engine->ingest(item);
  LO_REQUIRE(outcome.ok());
  LO_CHECK(!outcome.value().accepted);
  LO_CHECK(outcome.value().reason == RejectionReason::NoteTooLong);
}

LO_TEST(adversarial, replays_and_stale_epochs_are_fenced) {
  auto armed = armed_engine("adv-replay");
  ObservatoryEngine* engine = armed->engine.get();
  const EpochId epoch = active_epoch(*engine);
  EvidenceItem first = base_item(20);
  first.header.epoch = epoch;
  lofixture::ingest_ok(*engine, first);

  EvidenceItem replay = base_item(21);
  replay.header.epoch = epoch;
  replay.header.source_sequence = first.header.source_sequence;
  auto outcome = engine->ingest(replay);
  LO_REQUIRE(outcome.ok());
  LO_CHECK(!outcome.value().accepted);
  LO_CHECK(outcome.value().reason == RejectionReason::ReplayedSequence);

  EvidenceItem duplicate = base_item(20);
  duplicate.header.epoch = epoch;
  duplicate.header.source_sequence = SequenceId::from_value(999);
  outcome = engine->ingest(duplicate);
  LO_REQUIRE(outcome.ok());
  LO_CHECK(!outcome.value().accepted);
  LO_CHECK(outcome.value().reason == RejectionReason::ReplayedSequence ||
           outcome.value().reason == RejectionReason::DuplicateMeasurement);

  EvidenceItem stale = base_item(22);
  stale.header.epoch = EpochId::from_canonical_text("never-activated");
  outcome = engine->ingest(stale);
  LO_REQUIRE(outcome.ok());
  LO_CHECK(!outcome.value().accepted);
  LO_CHECK(outcome.value().reason == RejectionReason::StaleEpoch);

  EvidenceItem ghost = base_item(23);
  ghost.header.source = SourceId::from_canonical_text("who");
  outcome = engine->ingest(ghost);
  LO_REQUIRE(outcome.ok());
  LO_CHECK(!outcome.value().accepted);
  LO_CHECK(outcome.value().reason == RejectionReason::UnknownSource);
}

LO_TEST(adversarial, evidence_about_undeclared_entities_is_refused) {
  auto armed = armed_engine("adv-topology");
  ObservatoryEngine* engine = armed->engine.get();
  EvidenceItem item = base_item(30);
  item.header.epoch = active_epoch(*engine);
  item.header.subject = SubjectRef::queue(QueueId::from_canonical_text("never-declared"));
  lofixture::ingest_ok(*engine, item);

  auto derivation = engine->derive_all();
  LO_REQUIRE(derivation.ok());
  LO_CHECK_EQ(derivation.value().observations.size(), 0ULL);
  LO_REQUIRE(!derivation.value().rejections.empty());
  LO_CHECK(derivation.value().rejections[0].reason == RejectionReason::TopologyUnknown);
}

LO_TEST(adversarial, extreme_counter_values_do_not_overflow) {
  auto armed = armed_engine("adv-extreme");
  ObservatoryEngine* engine = armed->engine.get();
  const EpochId epoch = active_epoch(*engine);
  const std::uint64_t huge = std::numeric_limits<std::uint64_t>::max();

  EvidenceItem first = base_item(40);
  first.header.epoch = epoch;
  first.header.source_sequence = SequenceId::from_value(1);
  std::get<CounterSample>(first.payload).value = huge - 1;
  std::get<CounterSample>(first.payload).width_bits = 64;
  lofixture::ingest_ok(*engine, first);

  EvidenceItem second = base_item(41);
  second.header.epoch = epoch;
  second.header.source_sequence = SequenceId::from_value(2);
  std::get<CounterSample>(second.payload).value = 5;
  std::get<CounterSample>(second.payload).width_bits = 64;
  lofixture::ingest_ok(*engine, second);

  auto derivation = engine->derive_all();
  LO_REQUIRE(derivation.ok());
  LO_REQUIRE(derivation.value().observations.size() == 1ULL);
  const LossObservation& observation = derivation.value().observations[0];
  // A 64-bit modular wrap of this shape is implausible and must be flagged.
  LO_CHECK(observation.validity != ObservationValidity::Valid ||
           observation.lost == 0ULL);
  LO_CHECK(observation.ratio_bp <= 10000U);
}

LO_TEST(adversarial, distinct_subject_capacity_is_enforced_and_reported) {
  ManualClock clock{kNow};
  EngineConfig config = lofixture::default_config("adv-capacity");
  config.store.max_distinct_subjects = 2;
  config.store.max_items_per_subject = 2;
  config.loss.freshness.max_age = Duration::from_seconds(100000);
  auto engine = lofixture::make_engine(clock, config);

  const std::string text =
      "source id=adv name=adv kind=synthetic authority=primary\n"
      "incarnation source=adv epoch=adv-epoch name=boot at=2026-01-01T00:00:00Z active=true\n"
      "path id=p1 kind=synthetic hops=n1:0->n2:0\n"
      "queue id=q1 node=n1 port=0 priority=1 kind=egress-port-queue\n"
      "queue id=q2 node=n1 port=1 priority=1 kind=egress-port-queue\n"
      "queue id=q3 node=n1 port=2 priority=1 kind=egress-port-queue\n"
      "queue id=q4 node=n1 port=3 priority=1 kind=egress-port-queue\n";
  auto program = parse_script(text);
  LO_REQUIRE(program.ok());
  LO_CHECK(apply_script(*engine, program.value()).ok());

  for (std::uint64_t i = 0; i < 4; ++i) {
    EvidenceItem item = base_item(100 + i);
    item.header.epoch = active_epoch(*engine);
    item.header.source_sequence = SequenceId::from_value(100 + i);
    const std::string queue_name = "q" + std::to_string(i + 1);
    item.header.subject = SubjectRef::queue(QueueId::from_canonical_text(queue_name));
    std::get<CounterSample>(item.payload).queue = QueueId::from_canonical_text(queue_name);
    (void)engine->ingest(item);
  }
  LO_CHECK(engine->evidence().subject_count() <= 2ULL);
  const StoreStats stats = engine->evidence().stats();
  LO_CHECK(stats.rejected_capacity >= 1ULL);

  // Overfilling one subject evicts the oldest and says so.
  for (std::uint64_t i = 0; i < 4; ++i) {
    EvidenceItem item = base_item(200 + i);
    item.header.epoch = active_epoch(*engine);
    item.header.source_sequence = SequenceId::from_value(200 + i);
    lofixture::ingest_ok(*engine, item);
  }
  LO_CHECK(engine->evidence().stats().evicted_per_subject >= 1ULL);
  LO_CHECK(!engine->evidence().bound_notes().empty());
}

LO_TEST(adversarial, malformed_scripts_never_crash_the_parser) {
  testkit::DeterministicRng rng(99);
  const char alphabet[] = "\n=# abcdefghijklmnopqrstuvwxyz0123456789:-+_";
  for (int trial = 0; trial < 200; ++trial) {
    std::string text;
    const std::size_t length = static_cast<std::size_t>(rng.next_below(200));
    for (std::size_t i = 0; i < length; ++i) {
      text.push_back(alphabet[rng.next_below(sizeof(alphabet) - 1)]);
    }
    auto program = parse_script(text);
    LO_CHECK(program.ok());
    if (program.ok()) {
      LO_CHECK(program.value().error_count() <= program.value().commands.size() + text.size());
    }
  }
}

LO_TEST(adversarial, absurd_path_and_hop_counts_are_refused) {
  ManualClock clock{kNow};
  auto engine = lofixture::make_engine(clock, lofixture::default_config("adv-path"));
  // Compact hop text keeps the line inside the line bound, so the test
  // exercises the topology bound rather than the lexer bound.
  std::string hops;
  for (std::size_t i = 0; i < 300; ++i) {
    if (i != 0) {
      hops.push_back(',');
    }
    hops += "a" + std::to_string(i) + ":0->a" + std::to_string(i + 1) + ":0";
  }
  const std::string text = "path id=p1 kind=synthetic hops=" + hops + "\n";
  auto program = parse_script(text);
  LO_REQUIRE(program.ok());
  LO_CHECK_EQ(program.value().error_count(), 0ULL);
  const ScriptRunResult run = apply_script(*engine, program.value());
  LO_CHECK(!run.ok());
  LO_REQUIRE(run.steps.size() == 1ULL);
  LO_CHECK(run.steps[0].detail.find("hop limit") != std::string::npos);
  LO_CHECK_EQ(engine->topology().path_count(), 0ULL);
}

LO_TEST(adversarial, a_huge_script_is_bounded) {
  std::string text;
  for (std::size_t i = 0; i < 1000; ++i) {
    text += "source id=s" + std::to_string(i) + " name=n kind=synthetic authority=primary\n";
  }
  ScriptLimits limits{};
  limits.max_commands = 10;
  auto program = parse_script(text, limits);
  LO_REQUIRE(program.ok());
  LO_CHECK(program.value().truncated);
  LO_CHECK_EQ(program.value().commands.size(), 10ULL);
}

LO_TEST(adversarial, classification_of_a_flood_of_discontinuities_is_bounded_and_safe) {
  auto armed = armed_engine("adv-flood");
  ObservatoryEngine* engine = armed->engine.get();
  const EpochId epoch = active_epoch(*engine);
  // Every reading is lower than the one before and nowhere near the top of the
  // range, so every interval is a reset rather than a loss.
  for (std::uint64_t i = 0; i < 100; ++i) {
    EvidenceItem item = base_item(500 + i);
    item.header.epoch = epoch;
    item.header.source_sequence = SequenceId::from_value(500 + i);
    std::get<CounterSample>(item.payload).value = 100000 - i * 500;
    lofixture::ingest_ok(*engine, item);
  }
  auto classification = engine->classify(SubjectRef::queue(QueueId::from_canonical_text("q1")),
                                         kNow);
  LO_REQUIRE(classification.ok());
  LO_CHECK(!asserts_loss(classification.value().klass));
  LO_CHECK(classification.value().reasons.size() <= 64ULL);
  LO_CHECK(classification.value().evidence_ids.size() <= 256ULL);
}
