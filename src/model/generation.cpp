#include "loss_observatory/model/generation.hpp"

#include <shared_mutex>
#include <utility>

namespace loss_observatory {

std::string_view to_string(GenerationMatch match) noexcept {
  switch (match) {
    case GenerationMatch::Current:
      return "current";
    case GenerationMatch::Superseded:
      return "superseded";
    case GenerationMatch::UnknownGeneration:
      return "unknown-generation";
    case GenerationMatch::NoGenerationRecorded:
      return "no-generation-recorded";
  }
  return "no-generation-recorded";
}

std::string GenerationStatus::to_string() const {
  std::string result("generation=");
  result.append(loss_observatory::to_string(match));
  result.append(" observed=");
  result.append(observed.to_string());
  result.append(" current=");
  result.append(current.to_string());
  if (generation_changed) {
    result.append(" changed=true");
  }
  return result;
}

void GenerationRegistry::observe(std::map<std::uint64_t, std::map<GenerationId, GenerationRecord>>& table,
                                 std::uint64_t entity, GenerationId generation, Timestamp observed_at,
                                 std::size_t max_entities) {
  auto entity_it = table.find(entity);
  if (entity_it == table.end()) {
    if (table.size() >= max_entities) {
      // Bounded: the lowest identity is dropped rather than letting the table
      // grow without limit. A dropped entity later reports UnknownGeneration,
      // which is conservative: it can never be mistaken for current.
      table.erase(table.begin());
    }
    entity_it = table.emplace(entity, std::map<GenerationId, GenerationRecord>{}).first;
  }
  auto record_it = entity_it->second.find(generation);
  if (record_it == entity_it->second.end()) {
    entity_it->second.emplace(generation, GenerationRecord{observed_at, observed_at});
    return;
  }
  if (observed_at < record_it->second.first_seen) {
    record_it->second.first_seen = observed_at;
  }
  if (observed_at > record_it->second.last_seen) {
    record_it->second.last_seen = observed_at;
  }
}

GenerationStatus GenerationRegistry::correlate(
    const std::map<std::uint64_t, std::map<GenerationId, GenerationRecord>>& table, std::uint64_t entity,
    GenerationId generation, Timestamp at) {
  GenerationStatus status{};
  status.observed = generation;
  status.evidence_time = at;

  const auto entity_it = table.find(entity);
  if (entity_it == table.end() || entity_it->second.empty()) {
    status.match = GenerationMatch::NoGenerationRecorded;
    return status;
  }

  // The generation current at an instant is:
  //   * among generations that already existed then (first_seen at or before
  //     the instant), one whose observation window covers it, if any;
  //   * otherwise the most recently started of those;
  //   * and UnknownGeneration when nothing had started yet.
  // Ties break on the larger generation identity, so the answer is a pure
  // function of the recorded set and cannot depend on arrival order.
  bool found = false;
  bool best_covers = false;
  GenerationId best{};
  Timestamp best_first{};
  for (const auto& entry : entity_it->second) {
    const GenerationId candidate = entry.first;
    const GenerationRecord& record = entry.second;
    if (record.first_seen > at) {
      continue;
    }
    const bool covers = record.last_seen >= at;
    if (!found || (covers && !best_covers) ||
        (covers == best_covers &&
         (record.first_seen > best_first ||
          (record.first_seen == best_first && candidate > best)))) {
      best = candidate;
      best_first = record.first_seen;
      best_covers = covers;
      found = true;
    }
  }
  if (!found) {
    status.match = GenerationMatch::UnknownGeneration;
    status.current = GenerationId{};
    return status;
  }
  status.current = best;
  status.current_since = best_first;
  if (best == generation) {
    status.match = GenerationMatch::Current;
    return status;
  }
  status.match = GenerationMatch::Superseded;
  status.generation_changed = true;
  return status;
}

void GenerationRegistry::observe_flow(FlowId flow, GenerationId generation, Timestamp observed_at) {
  std::unique_lock lock(mutex_);
  observe(flows_, flow.value(), generation, observed_at, max_entities_);
}

void GenerationRegistry::observe_path(PathId path, GenerationId generation, Timestamp observed_at) {
  std::unique_lock lock(mutex_);
  observe(paths_, path.value(), generation, observed_at, max_entities_);
}

GenerationStatus GenerationRegistry::correlate_flow(FlowId flow, GenerationId generation,
                                                    Timestamp observation_time) const {
  std::shared_lock lock(mutex_);
  return correlate(flows_, flow.value(), generation, observation_time);
}

GenerationStatus GenerationRegistry::correlate_path(PathId path, GenerationId generation,
                                                    Timestamp observation_time) const {
  std::shared_lock lock(mutex_);
  return correlate(paths_, path.value(), generation, observation_time);
}

std::vector<std::pair<GenerationId, std::pair<Timestamp, Timestamp>>> GenerationRegistry::flow_generations(
    FlowId flow) const {
  std::shared_lock lock(mutex_);
  std::vector<std::pair<GenerationId, std::pair<Timestamp, Timestamp>>> result;
  const auto it = flows_.find(flow.value());
  if (it == flows_.end()) {
    return result;
  }
  result.reserve(it->second.size());
  for (const auto& entry : it->second) {
    result.emplace_back(entry.first, std::make_pair(entry.second.first_seen, entry.second.last_seen));
  }
  return result;
}

std::size_t GenerationRegistry::tracked_flows() const {
  std::shared_lock lock(mutex_);
  return flows_.size();
}

std::size_t GenerationRegistry::tracked_paths() const {
  std::shared_lock lock(mutex_);
  return paths_.size();
}

}  // namespace loss_observatory
