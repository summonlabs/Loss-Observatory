#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "fixtures.hpp"
#include "loss_observatory/report.hpp"
#include "loss_observatory/script.hpp"
#include "loss_observatory/testkit/testkit.hpp"

using namespace loss_observatory;

namespace {

const Timestamp kNow = Timestamp::from_unix_seconds(7000);

const char* kScenario =
    "source id=s1 name=leaf-a kind=counter-telemetry authority=primary\n"
    "incarnation source=s1 epoch=E1 name=boot-1 at=2026-01-01T00:00:00Z active=true\n"
    "path id=p1 kind=synthetic hops=n1:0->n2:0,n2:0->n3:0\n"
    "queue id=q1 node=n2 port=0 priority=3 kind=egress-port-queue\n"
    "flow id=f1 src=n1:0 dst=n3:0 proto=tcp path=p1 gen=7\n"
    "counter source=s1 epoch=E1 gen=7 seq=1 counter=c1 scope=dropped-packets value=100 bits=32 "
    "class=flow subject=flow:f1\n"
    "counter source=s1 epoch=E1 gen=7 seq=2 counter=c1 scope=dropped-packets value=150 bits=32 "
    "class=flow subject=flow:f1\n";

EngineConfig config_for(const std::string& directory) {
  EngineConfig config = lofixture::persistent_config(directory, "restart");
  config.loss.freshness.max_age = Duration::from_seconds(3600);
  return config;
}

}  // namespace

LO_TEST(restart, live_loss_is_recorded_and_persisted) {
  const std::string directory = lofixture::scratch_directory("restart-live");
  ManualClock clock{kNow};
  {
    auto engine = lofixture::make_engine(clock, config_for(directory));
    run_script_ok(*engine, kScenario);
    auto classification = engine->classify_flow(FlowId::from_canonical_text("f1"), kNow);
    LO_REQUIRE(classification.ok());
    LO_CHECK(classification.value().klass == LossClass::ConfirmedLoss);
    LO_REQUIRE(engine->persist().ok());
    LO_REQUIRE(engine->stop().ok());
  }
  lofixture::remove_directory(directory);
}

LO_TEST(restart, open_episode_is_sealed_and_not_revived) {
  const std::string directory = lofixture::scratch_directory("restart-seal");
  ManualClock clock{kNow};
  // An episode that the previous process left open at the moment it died.
  LossEpisode still_open{};
  {
    auto engine = lofixture::make_engine(clock, config_for(directory));
    run_script_ok(*engine, kScenario);
    auto classification = engine->classify_flow(FlowId::from_canonical_text("f1"), kNow);
    LO_REQUIRE(classification.ok());
    LO_CHECK(classification.value().klass == LossClass::ConfirmedLoss);
    LO_CHECK_EQ(engine->episodes().open_episodes().size(), 1ULL);
    // Simulate a process that died while an episode was open: the journal is
    // left holding an Open record for a second episode.
    still_open.id = EpisodeId::from_canonical_text("episode/open-when-the-process-died");
    still_open.flow = FlowId::from_canonical_text("f1");
    still_open.path = PathId::from_canonical_text("p1");
    still_open.generation = GenerationId::from_canonical_text("7");
    still_open.state = EpisodeState::Open;
    still_open.started_at = kNow;
    still_open.last_observed_at = kNow;
    still_open.lost_total = 9;
    LO_REQUIRE(engine->persistence()->append_episode(still_open).ok());
    LO_REQUIRE(engine->persist().ok());
    LO_REQUIRE(engine->stop().ok());
  }
  {
    // A crash-shaped restart: the journal is loaded by a brand new engine.
    ObservatoryEngine reopened{config_for(directory), clock};
    LO_REQUIRE(reopened.start().ok());
    auto recovery = reopened.load();
    LO_REQUIRE(recovery.ok());
    LO_CHECK(recovery.value().episodes_restored >= 1ULL);
    LO_CHECK(recovery.value().episodes_sealed >= 1ULL);
    LO_CHECK_EQ(reopened.episodes().open_episodes().size(), 0ULL);

    const FlowId flow = FlowId::from_canonical_text("f1");
    auto history = reopened.history(EpisodeQuery{});
    LO_REQUIRE(history.ok());
    bool saw_recovery_seal = false;
    std::size_t open_episodes = 0;
    for (const LossEpisode& episode : history.value().episodes) {
      if (episode.open()) {
        ++open_episodes;
      }
      if (episode.id == still_open.id) {
        saw_recovery_seal = true;
        LO_CHECK(episode.state == EpisodeState::ClosedByRestart);
        LO_CHECK(episode.closed_at == episode.last_observed_at);
      }
    }
    LO_CHECK(saw_recovery_seal);
    LO_CHECK_EQ(open_episodes, 0ULL);

    auto classification = reopened.classify_flow(flow, kNow);
    LO_REQUIRE(classification.ok());
    LO_CHECK(classification.value().klass == LossClass::StaleEvidence);
    LO_CHECK(!asserts_loss(classification.value().klass));
    LO_CHECK_EQ(classification.value().freshness.fresh, 0ULL);

    auto derivation = reopened.derive_all();
    LO_REQUIRE(derivation.ok());
    for (const LossObservation& observation : derivation.value().observations) {
      LO_CHECK(observation.recovered_from_persistence);
    }
    LO_CHECK(testkit::restart_does_not_revive(history.value(), derivation.value().observations,
                                              config_for(directory).loss.freshness,
                                              FreshnessContext{}, kNow));

    // Loss after the restart opens a brand new episode rather than waking the
    // sealed one.
    const std::string after = std::string("incarnation source=s1 epoch=E1 name=boot-2 at=2026-01-01T00:00:00Z active=true\n") +
                              "counter source=s1 epoch=E1 gen=7 seq=11 counter=c1 scope=dropped-packets value=400 bits=32 class=flow subject=flow:f1\n" +
                              "counter source=s1 epoch=E1 gen=7 seq=12 counter=c1 scope=dropped-packets value=470 bits=32 class=flow subject=flow:f1\n";
    run_script_ok(reopened, after);
    auto live = reopened.classify_flow(flow, kNow);
    LO_REQUIRE(live.ok());
    LO_CHECK(live.value().klass == LossClass::ConfirmedLoss);

    auto later = reopened.history(EpisodeQuery{});
    LO_REQUIRE(later.ok());
    LO_CHECK_EQ(later.value().episodes.size(), 3ULL);
    std::size_t open_count = 0;
    for (const LossEpisode& episode : later.value().episodes) {
      if (episode.open()) {
        ++open_count;
      }
    }
    LO_CHECK_EQ(open_count, 1ULL);
    LO_REQUIRE(reopened.stop().ok());
  }
  lofixture::remove_directory(directory);
}

LO_TEST(restart, declared_topology_is_restored_but_is_not_evidence) {
  const std::string directory = lofixture::scratch_directory("restart-topology");
  ManualClock clock{kNow};
  {
    auto engine = lofixture::make_engine(clock, config_for(directory));
    run_script_ok(*engine, kScenario);
    LO_REQUIRE(engine->persist().ok());
    LO_REQUIRE(engine->stop().ok());
  }
  {
    ObservatoryEngine reopened{config_for(directory), clock};
    LO_REQUIRE(reopened.start().ok());
    LO_REQUIRE(reopened.load().ok());
    LO_CHECK_EQ(reopened.topology().path_count(), 1ULL);
    LO_CHECK_EQ(reopened.topology().flow_count(), 1ULL);
    LO_CHECK_EQ(reopened.topology().queue_count(), 1ULL);
    auto path = reopened.topology().find_path(PathId::from_canonical_text("p1"));
    LO_REQUIRE(path.ok());
    LO_CHECK_EQ(path.value().hops.size(), 2ULL);
    LO_CHECK(reopened.sources().source_count() > 0ULL);
    LO_REQUIRE(reopened.stop().ok());
  }
  lofixture::remove_directory(directory);
}

LO_TEST(restart, a_truncated_journal_loses_only_the_damaged_tail) {
  const std::string directory = lofixture::scratch_directory("restart-truncated");
  const std::string journal = (std::filesystem::path(directory) / "restart.loj").string();
  ManualClock clock{kNow};
  {
    auto engine = lofixture::make_engine(clock, config_for(directory));
    run_script_ok(*engine, kScenario);
    LO_REQUIRE(engine->persist().ok());
    LO_REQUIRE(engine->stop().ok());
  }
  {
    std::error_code error;
    const auto size = std::filesystem::file_size(journal, error);
    LO_REQUIRE(!error);
    LO_REQUIRE(size > 40ULL);
    std::filesystem::resize_file(journal, size - 8, error);
    LO_REQUIRE(!error);
  }
  {
    ObservatoryEngine reopened{config_for(directory), clock};
    LO_REQUIRE(reopened.start().ok());
    auto recovery = reopened.load();
    LO_REQUIRE(recovery.ok());
    LO_CHECK(recovery.value().truncated);
    // Whatever survived is history, never current proof.
    auto classification = reopened.classify_flow(FlowId::from_canonical_text("f1"), kNow);
    LO_REQUIRE(classification.ok());
    LO_CHECK(!asserts_loss(classification.value().klass));
    LO_REQUIRE(reopened.stop().ok());
  }
  lofixture::remove_directory(directory);
}

LO_TEST(restart, a_second_session_appends_without_reviving_the_first) {
  const std::string directory = lofixture::scratch_directory("restart-sessions");
  ManualClock clock{kNow};
  for (int session = 0; session < 3; ++session) {
    ObservatoryEngine engine{config_for(directory), clock};
    LO_REQUIRE(engine.start().ok());
    auto recovery = engine.load();
    LO_REQUIRE(recovery.ok());
    if (session > 0) {
      LO_CHECK(recovery.value().evidence_restored >= 1ULL);
    }
    const std::string text =
        "source id=s1 name=leaf-a kind=counter-telemetry authority=primary\n"
        "incarnation source=s1 epoch=E1 name=boot at=2026-01-01T00:00:00Z active=true\n"
        "path id=p1 kind=synthetic hops=n1:0->n2:0,n2:0->n3:0\n"
        "queue id=q1 node=n2 port=0 priority=3 kind=egress-port-queue\n"
        "flow id=f1 src=n1:0 dst=n3:0 proto=tcp path=p1 gen=7\n" +
        std::string("counter source=s1 epoch=E1 gen=7 seq=") + std::to_string(session * 10 + 1) +
        " counter=c1 scope=dropped-packets value=" + std::to_string(100 + session * 10) +
        " bits=32 class=flow subject=flow:f1\n" +
        "counter source=s1 epoch=E1 gen=7 seq=" + std::to_string(session * 10 + 2) +
        " counter=c1 scope=dropped-packets value=" + std::to_string(160 + session * 10) +
        " bits=32 class=flow subject=flow:f1\n";
    auto program = parse_script(text);
    LO_REQUIRE(program.ok());
    const ScriptRunResult run = apply_script(engine, program.value());
    LO_CHECK(run.applied >= 6ULL);
    auto classification = engine.classify_flow(FlowId::from_canonical_text("f1"), kNow);
    LO_REQUIRE(classification.ok());
    LO_CHECK(classification.value().klass == LossClass::ConfirmedLoss);
    LO_REQUIRE(engine.persist().ok());
    LO_REQUIRE(engine.stop().ok());
  }
  lofixture::remove_directory(directory);
}
