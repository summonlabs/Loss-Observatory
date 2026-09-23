#include <string>
#include <vector>

#include "fixtures.hpp"
#include "loss_observatory/episode.hpp"

using namespace loss_observatory;

namespace {

const FlowId kFlow = FlowId::from_canonical_text("flow/ep");
const PathId kPath = PathId::from_canonical_text("path/ep");
const GenerationId kGeneration = GenerationId::from_canonical_text("gen/1");
const Timestamp kStart = Timestamp::from_unix_seconds(4000);

Classification classification(LossClass klass, std::uint64_t lost, std::uint64_t offered,
                              std::uint32_t ratio_bp = 0) {
  Classification value{};
  value.subject = SubjectRef::flow(kFlow);
  value.klass = klass;
  value.lost_total = lost;
  value.offered_total = offered;
  value.ratio_defined = offered > 0;
  value.ratio_bp = ratio_bp;
  value.evidence_admissible = 2;
  value.evidence_ids.push_back(MeasurementId::from_value(lost + 1));
  SourceClaim claim{};
  claim.source = SourceId::from_canonical_text("src");
  claim.klass = klass;
  claim.freshness = Freshness::Fresh;
  value.claims.push_back(claim);
  return value;
}

}  // namespace

LO_TEST(episode, loss_opens_an_episode_and_absence_closes_it) {
  EpisodeTracker tracker{};
  const EpisodeId opened =
      tracker.observe(classification(LossClass::ConfirmedLoss, 10, 1000, 100), kFlow, kPath, kGeneration,
                      kStart);
  LO_CHECK(!opened.is_nil());
  LO_CHECK_EQ(tracker.open_episodes().size(), 1ULL);

  const EpisodeId extended =
      tracker.observe(classification(LossClass::ConfirmedLoss, 5, 500, 100), kFlow, kPath, kGeneration,
                      kStart + Duration::from_seconds(10));
  LO_CHECK(extended == opened);
  const std::vector<LossEpisode> after_extension = tracker.episodes();
  LO_REQUIRE(after_extension.size() == 1ULL);
  const LossEpisode& episode = after_extension[0];
  LO_CHECK_EQ(episode.lost_total, 15ULL);
  LO_CHECK_EQ(episode.offered_total, 1500ULL);
  LO_CHECK_EQ(episode.observation_count, 4ULL);
  LO_CHECK(episode.open());

  (void)tracker.observe(classification(LossClass::NoLossObserved, 0, 1000, 0), kFlow, kPath, kGeneration,
                        kStart + Duration::from_seconds(20));
  LO_CHECK_EQ(tracker.open_episodes().size(), 0ULL);
  const std::vector<LossEpisode> closed = tracker.episodes();
  LO_REQUIRE(closed.size() == 1ULL);
  LO_CHECK(closed[0].state == EpisodeState::ClosedByAbsence);
  LO_CHECK(closed[0].closed_at == kStart + Duration::from_seconds(20));
}

LO_TEST(episode, inconclusive_classes_do_not_close_an_open_episode) {
  EpisodeTracker tracker{};
  (void)tracker.observe(classification(LossClass::ConfirmedLoss, 10, 1000, 100), kFlow, kPath, kGeneration,
                        kStart);
  (void)tracker.observe(classification(LossClass::IncompleteEvidence, 0, 0, 0), kFlow, kPath, kGeneration,
                        kStart + Duration::from_seconds(1));
  (void)tracker.observe(classification(LossClass::Discontinuity, 0, 0, 0), kFlow, kPath, kGeneration,
                        kStart + Duration::from_seconds(2));
  LO_CHECK_EQ(tracker.open_episodes().size(), 1ULL);
}

LO_TEST(episode, idle_episodes_are_closed_at_their_last_observation) {
  EpisodeLimits limits{};
  limits.idle_timeout = Duration::from_seconds(30);
  EpisodeTracker tracker{limits};
  (void)tracker.observe(classification(LossClass::ConfirmedLoss, 10, 1000, 100), kFlow, kPath, kGeneration,
                        kStart);
  LO_CHECK_EQ(tracker.close_idle(kStart + Duration::from_seconds(10)), 0ULL);
  LO_CHECK_EQ(tracker.close_idle(kStart + Duration::from_seconds(31)), 1ULL);
  const std::vector<LossEpisode> idle = tracker.episodes();
  LO_REQUIRE(idle.size() == 1ULL);
  LO_CHECK(idle[0].state == EpisodeState::ClosedByIdle);
  LO_CHECK(idle[0].closed_at == kStart);
}

LO_TEST(episode, sealing_closes_every_open_episode) {
  EpisodeTracker tracker{};
  (void)tracker.observe(classification(LossClass::ConfirmedLoss, 10, 1000, 100), kFlow, kPath, kGeneration,
                        kStart);
  LO_CHECK_EQ(tracker.seal_all(EpisodeState::ClosedExplicitly, kStart + Duration::from_seconds(5), "stop"),
              1ULL);
  LO_CHECK_EQ(tracker.open_episodes().size(), 0ULL);
  const std::vector<LossEpisode> sealed = tracker.episodes();
  LO_REQUIRE(sealed.size() == 1ULL);
  LO_CHECK(sealed[0].state == EpisodeState::ClosedExplicitly);
  LO_CHECK_EQ(tracker.seal_all(EpisodeState::ClosedExplicitly, kStart, "again"), 0ULL);
}

LO_TEST(episode, a_sealed_episode_is_never_extended_by_later_loss) {
  EpisodeTracker tracker{};
  const EpisodeId first = tracker.observe(classification(LossClass::ConfirmedLoss, 10, 1000, 100), kFlow,
                                          kPath, kGeneration, kStart);
  (void)tracker.seal_all(EpisodeState::ClosedByRestart, kStart + Duration::from_seconds(5), "restart");
  const EpisodeId second =
      tracker.observe(classification(LossClass::ConfirmedLoss, 7, 100, 700), kFlow, kPath, kGeneration,
                      kStart + Duration::from_seconds(60));
  LO_CHECK(!second.is_nil());
  LO_CHECK(second != first);
  LO_CHECK_EQ(tracker.episodes().size(), 2ULL);
  EpisodeQueryResult query = tracker.query(EpisodeQuery{});
  bool saw_closed = false;
  bool saw_open = false;
  for (const LossEpisode& episode : query.episodes) {
    if (episode.id == first) {
      LO_CHECK(!episode.open());
      LO_CHECK(episode.state == EpisodeState::ClosedByRestart);
      LO_CHECK_EQ(episode.lost_total, 10ULL);
      saw_closed = true;
    }
    if (episode.id == second) {
      LO_CHECK(episode.open());
      LO_CHECK_EQ(episode.lost_total, 7ULL);
      saw_open = true;
    }
  }
  LO_CHECK(saw_closed);
  LO_CHECK(saw_open);
}

LO_TEST(episode, restored_open_episodes_arrive_closed) {
  EpisodeTracker tracker{};
  LossEpisode episode{};
  episode.id = EpisodeId::from_canonical_text("episode/restored");
  episode.flow = kFlow;
  episode.path = kPath;
  episode.generation = kGeneration;
  episode.state = EpisodeState::Open;
  episode.started_at = kStart;
  episode.last_observed_at = kStart + Duration::from_seconds(3);
  episode.lost_total = 42;
  tracker.restore(episode);
  const std::vector<LossEpisode> restored = tracker.episodes();
  LO_REQUIRE(restored.size() == 1ULL);
  LO_CHECK(!restored[0].open());
  LO_CHECK(restored[0].state == EpisodeState::ClosedByRestart);
  LO_CHECK(restored[0].closed_at == episode.last_observed_at);
  LO_CHECK_EQ(tracker.open_episodes().size(), 0ULL);
}

LO_TEST(episode, identity_is_deterministic_and_replay_safe) {
  const EpisodeId first = make_episode_id(kFlow, kGeneration, kStart);
  const EpisodeId again = make_episode_id(kFlow, kGeneration, kStart);
  const EpisodeId other = make_episode_id(kFlow, kGeneration, kStart + Duration::from_seconds(1));
  LO_CHECK(first == again);
  LO_CHECK(first != other);
  LO_CHECK(!first.is_nil());

  // Restoring the same episode twice must not duplicate it.
  EpisodeTracker tracker{};
  LossEpisode episode{};
  episode.id = first;
  episode.flow = kFlow;
  episode.generation = kGeneration;
  episode.started_at = kStart;
  episode.last_observed_at = kStart;
  episode.state = EpisodeState::ClosedByAbsence;
  tracker.restore(episode);
  tracker.restore(episode);
  LO_CHECK_EQ(tracker.episodes().size(), 1ULL);
}

LO_TEST(episode, queries_filter_and_report_truncation) {
  EpisodeLimits limits{};
  limits.max_query_results = 1;
  EpisodeTracker tracker{limits};
  for (std::size_t i = 0; i < 3; ++i) {
    (void)tracker.seal_all(EpisodeState::ClosedExplicitly, kStart, "seed");
    (void)tracker.observe(classification(LossClass::ConfirmedLoss, 1, 10, 1000), kFlow, kPath, kGeneration,
                          kStart + Duration::from_seconds(static_cast<std::int64_t>(i) * 60));
  }
  EpisodeQuery query{};
  query.flow = kFlow;
  query.max_results = 1;
  const EpisodeQueryResult result = tracker.query(query);
  LO_CHECK_EQ(result.total_matching, 3ULL);
  LO_CHECK_EQ(result.episodes.size(), 1ULL);
  LO_CHECK(result.truncated);
  LO_CHECK(!result.bounds.empty());

  EpisodeQuery other{};
  other.flow = FlowId::from_canonical_text("flow/other");
  LO_CHECK_EQ(tracker.query(other).episodes.size(), 0ULL);
}

LO_TEST(episode, the_episode_bound_evicts_closed_history_first) {
  EpisodeLimits limits{};
  limits.max_episodes = 3;
  EpisodeTracker tracker{limits};
  for (std::size_t i = 0; i < 6; ++i) {
    (void)tracker.seal_all(EpisodeState::ClosedExplicitly, kStart, "cycle");
    (void)tracker.observe(classification(LossClass::ConfirmedLoss, 1, 10, 1000), kFlow, kPath, kGeneration,
                          kStart + Duration::from_seconds(static_cast<std::int64_t>(i) * 60));
  }
  LO_CHECK(tracker.episode_count() <= 4ULL);
  LO_CHECK(tracker.evicted_total() > 0ULL);
}
