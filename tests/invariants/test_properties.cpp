#include <algorithm>
#include <string>
#include <vector>

#include "fixtures.hpp"
#include "loss_observatory/report.hpp"
#include "loss_observatory/testkit/testkit.hpp"

using namespace loss_observatory;

namespace {

/// Deterministic pseudo-random scenario generator. Every parameter comes from
/// the seed, so a failing case is reproducible from the seed alone.
struct RandomScenario {
  testkit::ScenarioConfig config{};
  std::uint64_t seed{0};
};

Classification classify_scenario(const RandomScenario& scenario, std::vector<LossObservation>* out,
                                 LocalizationResult* localization_out) {
  ManualClock clock{scenario.config.start};
  EngineConfig engine_config = lofixture::default_config("property");
  engine_config.loss.freshness.max_age = Duration::from_seconds(100000);
  auto engine = lofixture::make_engine(clock, engine_config);
  auto made = testkit::make_drop_counter_scenario(*engine, scenario.config);
  if (!made.ok()) {
    throw lotest::Failure{"scenario generation failed: " + made.status().to_string()};
  }
  const Timestamp at = scenario.config.start +
                       Duration::from_nanos(scenario.config.sample_interval.nanos() *
                                            static_cast<std::int64_t>(scenario.config.sample_count + 2));
  auto derivation = engine->derive_all();
  if (!derivation.ok()) {
    throw lotest::Failure{"derivation failed"};
  }
  if (out != nullptr) {
    *out = derivation.value().observations;
  }
  if (localization_out != nullptr) {
    LocalizationRequest request{};
    request.flow = made.value().flow;
    request.at = at;
    auto localized = engine->localize(request);
    if (!localized.ok()) {
      throw lotest::Failure{"localization failed"};
    }
    *localization_out = localized.value();
  }
  auto classification = engine->classify_flow(made.value().flow, at);
  if (!classification.ok()) {
    throw lotest::Failure{"classification failed"};
  }
  return classification.value();
}

}  // namespace

LO_TEST(properties, invariants_hold_across_seeded_scenarios) {
  for (std::uint64_t seed = 1; seed <= 12; ++seed) {
    RandomScenario scenario{};
    scenario.seed = seed;
    scenario.config.seed = seed;
    scenario.config.hop_count = 2 + (seed % 4);
    scenario.config.sample_count = 4 + (seed % 9);
    scenario.config.loss_ratio_bp = static_cast<std::uint32_t>((seed * 137) % 1200);
    scenario.config.counter_width_bits = (seed % 3 == 0) ? 16 : 32;
    scenario.config.inject_counter_reset = (seed % 4 == 0);
    scenario.config.inject_counter_wrap = (seed % 5 == 0);

    std::vector<LossObservation> observations;
    LocalizationResult localization{};
    const Classification classification = classify_scenario(scenario, &observations, &localization);

    // Property 1: a loss conclusion always rests on admissible evidence.
    LO_CHECK(testkit::stale_evidence_cannot_prove_current_loss(classification));
    // Property 2: discontinuities are labelled and never counted as loss.
    LO_CHECK(testkit::discontinuities_are_explicit(classification, observations));
    // Property 3: every cited reason renders.
    LO_CHECK(testkit::reasons_are_renderable(classification.reasons));
    // Property 4: ratios stay inside their range.
    LO_CHECK(classification.ratio_bp <= 10000U);
    for (const LossObservation& observation : observations) {
      LO_CHECK(observation.ratio_bp <= 10000U);
      if (observation.validity != ObservationValidity::Valid) {
        LO_CHECK_EQ(observation.lost, 0ULL);
      }
    }
    // Property 5: localization never outruns its evidence.
    LO_CHECK(testkit::localization_within_evidence_granularity(localization, observations));
  }
}

LO_TEST(properties, the_same_seed_always_produces_the_same_answer) {
  for (std::uint64_t seed = 1; seed <= 6; ++seed) {
    RandomScenario scenario{};
    scenario.seed = seed;
    scenario.config.seed = seed;
    scenario.config.sample_count = 8;
    const Classification first = classify_scenario(scenario, nullptr, nullptr);
    const Classification second = classify_scenario(scenario, nullptr, nullptr);
    LO_CHECK_EQ(first.summary, second.summary);
    LO_CHECK(first.klass == second.klass);
    LO_CHECK_EQ(first.attribution.score, second.attribution.score);
  }
}

LO_TEST(properties, derivation_is_invariant_under_input_permutation) {
  for (std::uint64_t seed = 1; seed <= 8; ++seed) {
    testkit::ScenarioConfig config{};
    config.seed = seed;
    config.sample_count = 10;
    std::vector<EvidenceItem> items = testkit::make_counter_series(
        config, CounterScope::DroppedPackets, 1000, std::vector<std::uint64_t>(10, 7));

    DerivePolicy policy{};
    policy.require_declared_topology = false;
    DerivationContext context{};
    const DerivationResult baseline = derive_observations(items, context, policy);

    testkit::DeterministicRng rng(seed);
    for (int trial = 0; trial < 4; ++trial) {
      std::vector<EvidenceItem> shuffled = items;
      for (std::size_t i = shuffled.size(); i > 1; --i) {
        const std::size_t j = static_cast<std::size_t>(rng.next_below(i));
        std::swap(shuffled[i - 1], shuffled[j]);
      }
      const DerivationResult permuted = derive_observations(shuffled, context, policy);
      LO_REQUIRE(permuted.observations.size() == baseline.observations.size());
      for (std::size_t i = 0; i < baseline.observations.size(); ++i) {
        LO_CHECK(baseline.observations[i].id == permuted.observations[i].id);
        LO_CHECK_EQ(baseline.observations[i].lost, permuted.observations[i].lost);
        LO_CHECK(baseline.observations[i].validity == permuted.observations[i].validity);
      }
    }
  }
}

LO_TEST(properties, injected_drops_are_never_under_reported) {
  for (std::uint64_t seed = 1; seed <= 6; ++seed) {
    testkit::ScenarioConfig config{};
    config.seed = seed;
    config.sample_count = 12;
    config.loss_ratio_bp = 300;
    config.start = Timestamp::from_unix_seconds(1767225600);
    ManualClock clock{config.start};
    EngineConfig engine_config = lofixture::default_config("drops");
    engine_config.loss.freshness.max_age = Duration::from_seconds(100000);
    auto engine = lofixture::make_engine(clock, engine_config);
    auto scenario = testkit::make_drop_counter_scenario(*engine, config);
    LO_REQUIRE(scenario.ok());
    const Timestamp at = config.start + Duration::from_nanos(config.sample_interval.nanos() * 20);
    auto classification = engine->classify_flow(scenario.value().flow, at);
    LO_REQUIRE(classification.ok());
    LO_CHECK(classification.value().klass == LossClass::ConfirmedLoss);
    LO_CHECK_EQ(classification.value().lost_total, scenario.value().injected_drops);
  }
}

LO_TEST(properties, discontinuity_injections_never_inflate_the_loss_total) {
  for (const bool reset : {true, false}) {
    testkit::ScenarioConfig config{};
    config.seed = 42;
    config.sample_count = 12;
    config.loss_ratio_bp = 300;
    config.inject_counter_reset = reset;
    config.start = Timestamp::from_unix_seconds(1767225600);
    ManualClock clock{config.start};
    EngineConfig engine_config = lofixture::default_config("discontinuity");
    engine_config.loss.freshness.max_age = Duration::from_seconds(100000);
    auto engine = lofixture::make_engine(clock, engine_config);
    auto scenario = testkit::make_drop_counter_scenario(*engine, config);
    LO_REQUIRE(scenario.ok());
    const Timestamp at = config.start + Duration::from_nanos(config.sample_interval.nanos() * 20);
    auto derivation = engine->derive_all();
    LO_REQUIRE(derivation.ok());
    for (const LossObservation& observation : derivation.value().observations) {
      if (observation.validity != ObservationValidity::Valid) {
        LO_CHECK_EQ(observation.lost, 0ULL);
      }
    }
    auto classification = engine->classify_flow(scenario.value().flow, at);
    LO_REQUIRE(classification.ok());
    LO_CHECK(classification.value().lost_total <= scenario.value().injected_drops);
  }
}

LO_TEST(properties, freshness_never_promotes_recovered_evidence) {
  LossPolicy policy{};
  policy.freshness.max_age = Duration::from_seconds(100000);
  for (std::uint64_t seed = 1; seed <= 20; ++seed) {
    LossObservation observation{};
    observation.source = SourceId::from_canonical_text("src");
    observation.epoch = EpochId::from_canonical_text("epoch/1");
    observation.subject = SubjectRef::flow(FlowId::from_canonical_text("flow/1"));
    observation.observed_at.value = Timestamp::from_unix_seconds(static_cast<std::int64_t>(seed) * 10);
    observation.received_at.value = observation.observed_at.value;
    observation.recovered_from_persistence = true;
    const FreshnessAssessment assessment = assess_freshness(
        observation, Timestamp::from_unix_seconds(2000000), policy.freshness, FreshnessContext{});
    LO_CHECK(!assessment.admissible);
    LO_CHECK(assessment.state == Freshness::NotCurrent);
  }
}
