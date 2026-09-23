#include "loss_observatory/episode.hpp"

#include <algorithm>
#include <limits>
#include <mutex>
#include <string>

namespace loss_observatory {
namespace {

[[nodiscard]] std::uint64_t saturating_add(std::uint64_t lhs, std::uint64_t rhs, bool& saturated) {
  if (std::numeric_limits<std::uint64_t>::max() - lhs < rhs) {
    saturated = true;
    return std::numeric_limits<std::uint64_t>::max();
  }
  return lhs + rhs;
}

[[nodiscard]] int class_severity(LossClass klass) noexcept {
  switch (klass) {
    case LossClass::Unknown:
      return 0;
    case LossClass::NoLossObserved:
      return 1;
    case LossClass::AbsentEvidence:
      return 2;
    case LossClass::IncompleteEvidence:
      return 3;
    case LossClass::StaleEvidence:
      return 4;
    case LossClass::Discontinuity:
      return 5;
    case LossClass::UnsupportedMethod:
      return 6;
    case LossClass::ImplausibleEvidence:
      return 7;
    case LossClass::ConflictingEvidence:
      return 8;
    case LossClass::SuspectedLoss:
      return 9;
    case LossClass::ConfirmedLoss:
      return 10;
  }
  return 0;
}

[[nodiscard]] LossClass worse_class(LossClass lhs, LossClass rhs) noexcept {
  return class_severity(lhs) >= class_severity(rhs) ? lhs : rhs;
}

}  // namespace

std::string_view to_string(EpisodeState state) noexcept {
  switch (state) {
    case EpisodeState::Open:
      return "open";
    case EpisodeState::ClosedByAbsence:
      return "closed-by-absence";
    case EpisodeState::ClosedByIdle:
      return "closed-by-idle";
    case EpisodeState::ClosedByRestart:
      return "closed-by-restart";
    case EpisodeState::ClosedExplicitly:
      return "closed-explicitly";
  }
  return "open";
}

bool is_closed(EpisodeState state) noexcept { return state != EpisodeState::Open; }

EpisodeId make_episode_id(FlowId flow, GenerationId generation, Timestamp start) {
  std::uint64_t mixed = combine_hash(flow.value(), generation.value());
  mixed = combine_hash(mixed, static_cast<std::uint64_t>(start.unix_nanos()));
  return EpisodeId::from_value(mixed == 0 ? 1 : mixed);
}

std::string LossEpisode::to_string() const {
  std::string result = "episode ";
  result.append(id.to_string());
  result.append(" flow=");
  result.append(flow.to_string());
  result.append(" path=");
  result.append(path.to_string());
  result.append(" gen=");
  result.append(generation.to_string());
  result.append(" state=");
  result.append(loss_observatory::to_string(state));
  result.append(" peak=");
  result.append(loss_observatory::to_string(peak_class));
  result.append(" granularity=");
  result.append(loss_observatory::to_string(granularity));
  result.append(" start=");
  result.append(started_at.to_string());
  result.append(" last=");
  result.append(last_observed_at.to_string());
  if (is_closed(state)) {
    result.append(" closed=");
    result.append(closed_at.to_string());
  }
  result.append(" lost=");
  result.append(std::to_string(lost_total));
  result.append(" offered=");
  result.append(std::to_string(offered_total));
  result.append(" observations=");
  result.append(std::to_string(observation_count));
  if (totals_saturated) {
    result.append(" saturated=true");
  }
  return result;
}

std::string EpisodeQueryResult::to_string() const {
  std::string result("history episodes=");
  result.append(std::to_string(episodes.size()));
  result.append(" matching=");
  result.append(std::to_string(total_matching));
  result.append(truncated ? " truncated=true" : " truncated=false");
  result.append(" evicted=");
  result.append(std::to_string(evicted_episodes));
  return result;
}

EpisodeId EpisodeTracker::observe(const Classification& classification, FlowId flow, PathId path,
                                  GenerationId generation, Timestamp now) {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto key = std::make_pair(flow, generation);
  const bool loss = asserts_loss(classification.klass);
  const auto open_it = open_by_flow_.find(key);

  if (!loss) {
    if (open_it == open_by_flow_.end()) {
      return EpisodeId{};
    }
    // The identity is captured before the map is modified: the erase
    // invalidates the iterator, so it must not be dereferenced afterwards.
    const EpisodeId open_id = open_it->second;
    if (asserts_absence(classification.klass)) {
      const auto episode_it = episodes_.find(open_id);
      if (episode_it != episodes_.end()) {
        episode_it->second.state = EpisodeState::ClosedByAbsence;
        episode_it->second.closed_at = now;
        episode_it->second.state_detail = "absence of loss observed with adequate coverage";
      }
      open_by_flow_.erase(open_it);
    }
    return open_id;
  }

  if (open_it == open_by_flow_.end()) {
    LossEpisode episode{};
    EpisodeId id = make_episode_id(flow, generation, now);
    std::uint64_t salt = 1;
    while (episodes_.find(id) != episodes_.end()) {
      id = EpisodeId::from_value(combine_hash(id.value(), salt));
      ++salt;
      if (id.is_nil()) {
        id = EpisodeId::from_value(1);
      }
    }
    episode.id = id;
    episode.flow = flow;
    episode.path = path;
    episode.generation = generation;
    episode.state = EpisodeState::Open;
    episode.peak_class = classification.klass;
    episode.granularity = Granularity::Unknown;
    episode.started_at = now;
    episode.last_observed_at = now;
    episode.lost_total = classification.lost_total;
    episode.offered_total = classification.offered_total;
    episode.ratio_defined = classification.ratio_defined;
    episode.peak_ratio_bp = classification.ratio_bp;
    episode.last_ratio_bp = classification.ratio_bp;
    episode.totals_saturated = classification.totals_saturated;
    episode.observation_count = classification.evidence_admissible;
    for (const SourceClaim& claim : classification.claims) {
      if (episode.sources.size() >= limits_.max_sources_per_episode) {
        break;
      }
      if (std::find(episode.sources.begin(), episode.sources.end(), claim.source) ==
          episode.sources.end()) {
        episode.sources.push_back(claim.source);
      }
    }
    for (const MeasurementId id_value : classification.evidence_ids) {
      if (episode.evidence_ids.size() >= limits_.max_evidence_ids_per_episode) {
        episode.evidence_ids_truncated = true;
        break;
      }
      episode.evidence_ids.push_back(id_value);
    }
    episodes_.emplace(episode.id, episode);
    open_by_flow_.emplace(key, episode.id);
    evict_locked();
    return episode.id;
  }

  auto episode_it = episodes_.find(open_it->second);
  if (episode_it == episodes_.end()) {
    open_by_flow_.erase(open_it);
    return EpisodeId{};
  }
  LossEpisode& episode = episode_it->second;
  if (now > episode.last_observed_at) {
    episode.last_observed_at = now;
  }
  episode.lost_total = saturating_add(episode.lost_total, classification.lost_total, episode.totals_saturated);
  episode.offered_total =
      saturating_add(episode.offered_total, classification.offered_total, episode.totals_saturated);
  episode.peak_class = worse_class(episode.peak_class, classification.klass);
  if (classification.ratio_defined && classification.ratio_bp > episode.peak_ratio_bp) {
    episode.peak_ratio_bp = classification.ratio_bp;
  }
  episode.last_ratio_bp = classification.ratio_bp;
  episode.ratio_defined = episode.ratio_defined || classification.ratio_defined;
  const std::uint64_t observations = static_cast<std::uint64_t>(classification.evidence_admissible);
  if (std::numeric_limits<std::uint64_t>::max() - episode.observation_count < observations) {
    episode.observation_count = std::numeric_limits<std::size_t>::max();
  } else {
    episode.observation_count += static_cast<std::size_t>(observations);
  }
  for (const SourceClaim& claim : classification.claims) {
    if (episode.sources.size() >= limits_.max_sources_per_episode) {
      break;
    }
    if (std::find(episode.sources.begin(), episode.sources.end(), claim.source) == episode.sources.end()) {
      episode.sources.push_back(claim.source);
    }
  }
  for (const MeasurementId id_value : classification.evidence_ids) {
    if (episode.evidence_ids.size() >= limits_.max_evidence_ids_per_episode) {
      episode.evidence_ids_truncated = true;
      break;
    }
    episode.evidence_ids.push_back(id_value);
  }
  return episode.id;
}

void EpisodeTracker::evict_locked() {
  while (episodes_.size() > limits_.max_episodes) {
    auto oldest = episodes_.begin();
    for (auto it = episodes_.begin(); it != episodes_.end(); ++it) {
      if (it->second.started_at < oldest->second.started_at ||
          (it->second.started_at == oldest->second.started_at && it->second.id < oldest->second.id)) {
        oldest = it;
      }
    }
    if (oldest->second.open()) {
      // The oldest episode is open: close it as idle rather than deleting an
      // episode that the runtime is still tracking. If it is also the only
      // open one, the bound cannot be honoured without losing live state, so
      // the bound is exceeded and reported instead.
      bool has_closed = false;
      for (const auto& entry : episodes_) {
        if (is_closed(entry.second.state)) {
          has_closed = true;
          break;
        }
      }
      if (!has_closed) {
        break;
      }
      auto closed = episodes_.begin();
      bool found = false;
      for (auto it = episodes_.begin(); it != episodes_.end(); ++it) {
        if (is_closed(it->second.state)) {
          closed = it;
          found = true;
          break;
        }
      }
      if (!found) {
        break;
      }
      oldest = closed;
    }
    open_by_flow_.erase(std::make_pair(oldest->second.flow, oldest->second.generation));
    episodes_.erase(oldest);
    ++evicted_;
  }
}

std::size_t EpisodeTracker::close_idle(Timestamp now) {
  std::lock_guard<std::mutex> guard(mutex_);
  std::size_t closed = 0;
  for (auto& entry : episodes_) {
    LossEpisode& episode = entry.second;
    if (!episode.open()) {
      continue;
    }
    if (now - episode.last_observed_at > limits_.idle_timeout) {
      episode.state = EpisodeState::ClosedByIdle;
      episode.closed_at = episode.last_observed_at;
      episode.state_detail = "no loss observation arrived inside the idle window";
      open_by_flow_.erase(std::make_pair(episode.flow, episode.generation));
      ++closed;
    }
  }
  return closed;
}

std::size_t EpisodeTracker::seal_all(EpisodeState state, Timestamp at, std::string detail) {
  std::lock_guard<std::mutex> guard(mutex_);
  std::size_t sealed = 0;
  for (auto& entry : episodes_) {
    LossEpisode& episode = entry.second;
    if (!episode.open()) {
      continue;
    }
    episode.state = state;
    if (state == EpisodeState::ClosedByRestart) {
      // A restart cannot claim the episode continued while the runtime was
      // down, so it ends at the last instant loss was actually observed.
      episode.closed_at = episode.last_observed_at;
    } else {
      episode.closed_at = at > episode.last_observed_at ? at : episode.last_observed_at;
    }
    episode.state_detail = detail;
    ++sealed;
  }
  open_by_flow_.clear();
  return sealed;
}

Result<LossEpisode> EpisodeTracker::find(EpisodeId id) const {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto it = episodes_.find(id);
  if (it == episodes_.end()) {
    return make_status(StatusCode::NotFound, "episode is not recorded");
  }
  return it->second;
}

std::vector<LossEpisode> EpisodeTracker::episodes() const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<LossEpisode> result;
  result.reserve(episodes_.size());
  for (const auto& entry : episodes_) {
    result.push_back(entry.second);
  }
  return result;
}

std::vector<LossEpisode> EpisodeTracker::open_episodes() const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<LossEpisode> result;
  for (const auto& entry : episodes_) {
    if (entry.second.open()) {
      result.push_back(entry.second);
    }
  }
  return result;
}

EpisodeQueryResult EpisodeTracker::query(const EpisodeQuery& request) const {
  std::lock_guard<std::mutex> guard(mutex_);
  EpisodeQueryResult result{};
  std::vector<LossEpisode> matching;
  for (const auto& entry : episodes_) {
    const LossEpisode& episode = entry.second;
    if (request.flow.has_value() && episode.flow != request.flow.value()) {
      continue;
    }
    if (request.path.has_value() && episode.path != request.path.value()) {
      continue;
    }
    if (!request.from.is_zero() && episode.started_at < request.from) {
      continue;
    }
    if (!request.to.is_zero() && episode.started_at > request.to) {
      continue;
    }
    if (episode.open() && !request.include_open) {
      continue;
    }
    if (!episode.open() && !request.include_closed) {
      continue;
    }
    matching.push_back(episode);
  }
  std::sort(matching.begin(), matching.end(), [](const LossEpisode& lhs, const LossEpisode& rhs) {
    if (lhs.started_at != rhs.started_at) {
      return lhs.started_at < rhs.started_at;
    }
    return lhs.id < rhs.id;
  });
  result.total_matching = matching.size();
  result.evicted_episodes = evicted_;
  const std::size_t limit = request.max_results == 0 ? limits_.max_query_results : request.max_results;
  result.truncated = matching.size() > limit;
  if (result.truncated) {
    matching.resize(limit);
    result.bounds.push_back(BoundNote{BoundKind::HistoryEpisodes, static_cast<std::uint64_t>(limit),
                                      result.total_matching, "episode-query"});
  }
  result.episodes = std::move(matching);
  result.summary = result.to_string();
  return result;
}

std::size_t EpisodeTracker::episode_count() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return episodes_.size();
}

std::uint64_t EpisodeTracker::evicted_total() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return evicted_;
}

void EpisodeTracker::restore(LossEpisode episode) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (episode.open()) {
    // A restored open episode is history the moment it is loaded. It can never
    // become current again, so no later observation can extend it.
    episode.state = EpisodeState::ClosedByRestart;
    episode.closed_at = episode.last_observed_at;
    episode.state_detail = "open at the moment of the previous shutdown; sealed during recovery";
  }
  const auto existing = episodes_.find(episode.id);
  if (existing != episodes_.end()) {
    return;
  }
  episodes_.emplace(episode.id, std::move(episode));
  evict_locked();
}

}  // namespace loss_observatory
