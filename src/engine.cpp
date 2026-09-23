#include "loss_observatory/engine.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>

#include "loss_observatory/core/checked.hpp"
#include "loss_observatory/script.hpp"
#include "loss_observatory/version.hpp"

namespace loss_observatory {
namespace {

[[nodiscard]] std::string make_incarnation_name(const std::string& instance, Timestamp booted_at) {
  return instance + "@" + std::to_string(booted_at.unix_nanos());
}

/// Per-observation class used by the time-bucket aggregator. The subject-level
/// classification remains the authoritative conclusion; a bucket records what
/// a single observation asserted so that history can be read back cheaply.
[[nodiscard]] LossClass classify_single_observation(const LossObservation& observation) {
  switch (observation.validity) {
    case ObservationValidity::Discontinuity:
      return LossClass::Discontinuity;
    case ObservationValidity::Implausible:
      return LossClass::ImplausibleEvidence;
    case ObservationValidity::Insufficient:
    case ObservationValidity::UndefinedRatio:
      return LossClass::IncompleteEvidence;
    case ObservationValidity::UnsupportedMethod:
    case ObservationValidity::UnsupportedPrecondition:
      return LossClass::UnsupportedMethod;
    case ObservationValidity::Valid:
      break;
  }
  switch (observation.semantics) {
    case LossSemantics::DirectLoss:
      return observation.lost > 0 ? LossClass::ConfirmedLoss : LossClass::NoLossObserved;
    case LossSemantics::RatioLoss:
      if (!observation.ratio_defined) {
        return LossClass::IncompleteEvidence;
      }
      return observation.ratio_bp > 0 ? LossClass::SuspectedLoss : LossClass::NoLossObserved;
    case LossSemantics::NonLoss:
      // A throughput observation is not a loss conclusion in either direction.
      return LossClass::IncompleteEvidence;
  }
  return LossClass::Unknown;
}

}  // namespace

std::string_view to_string(EngineState state) noexcept {
  switch (state) {
    case EngineState::Created:
      return "created";
    case EngineState::Running:
      return "running";
    case EngineState::Draining:
      return "draining";
    case EngineState::Stopped:
      return "stopped";
    case EngineState::Failed:
      return "failed";
  }
  return "created";
}

ObservatoryEngine::ObservatoryEngine(EngineConfig config, const Clock& clock)
    : config_(std::move(config)), clock_(&clock) {
  topology_ = std::make_unique<TopologyRegistry>(config_.topology);
  sources_ = std::make_unique<SourceRegistry>();
  generations_ = std::make_unique<GenerationRegistry>();
  evidence_ = std::make_unique<EvidenceStore>(*sources_, config_.store);
  episodes_ = std::make_unique<EpisodeTracker>(config_.episodes);
  aggregate_ = std::make_unique<BoundedAggregator>(config_.aggregate);
  if (config_.persist.enabled) {
    persistence_ = std::make_unique<PersistenceStore>(config_.persist);
  }
  if (config_.worker_count > 0) {
    inbox_ = std::make_unique<BoundedQueue<WorkItem>>(config_.inbox_capacity == 0 ? 1
                                                                                  : config_.inbox_capacity);
  }
}

ObservatoryEngine::~ObservatoryEngine() {
  if (state() == EngineState::Running || state() == EngineState::Draining) {
    const Result<void> stopped = stop();
    (void)stopped;
  }
}

Result<void> ObservatoryEngine::start() {
  const EngineState current = state();
  if (current == EngineState::Running) {
    return {};
  }
  if (current != EngineState::Created && current != EngineState::Stopped) {
    return make_status(StatusCode::Conflict, "engine cannot be started from its current state");
  }
  const Timestamp now = clock_->now();
  if (persistence_) {
    const Result<EngineEpoch> epoch = persistence_->open_session(now, make_incarnation_name(config_.instance_name, now));
    if (!epoch.ok()) {
      state_.store(static_cast<int>(EngineState::Failed), std::memory_order_release);
      return epoch.status();
    }
  }
  if (inbox_) {
    for (std::size_t i = 0; i < config_.worker_count; ++i) {
      workers_.emplace_back([this] { worker_main(stop_source_.get_token()); });
    }
  }
  state_.store(static_cast<int>(EngineState::Running), std::memory_order_release);
  return {};
}

void ObservatoryEngine::request_stop() noexcept {
  stop_source_.request_stop();
  if (inbox_) {
    inbox_->close();
  }
  const EngineState current = state();
  if (current == EngineState::Running) {
    state_.store(static_cast<int>(EngineState::Draining), std::memory_order_release);
  }
}

Result<void> ObservatoryEngine::stop() {
  const EngineState current = state();
  if (current == EngineState::Stopped) {
    return {};
  }
  if (current == EngineState::Created) {
    state_.store(static_cast<int>(EngineState::Stopped), std::memory_order_release);
    return {};
  }
  state_.store(static_cast<int>(EngineState::Draining), std::memory_order_release);

  bool stopped_cleanly = true;
  if (!stop_source_.stop_requested()) {
    // Graceful path: let workers finish what is already queued, then join.
    if (inbox_) {
      inbox_->close_after_drain();
    }
  }
  for (std::jthread& worker : workers_) {
    worker.join();
  }
  workers_.clear();
  stop_source_.request_stop();
  if (inbox_) {
    inbox_->close();
  }

  if (inbox_) {
    // Wake any caller still waiting on a batch; its own state records the
    // submissions that never ran.
    std::lock_guard<std::mutex> guard(inbox_waiters_mutex_);
    for (const std::weak_ptr<BatchState>& weak : inbox_waiters_) {
      const std::shared_ptr<BatchState> state = weak.lock();
      if (!state) {
        continue;
      }
      std::lock_guard<std::mutex> state_guard(state->mutex);
      for (std::size_t i = 0; i < state->done.size(); ++i) {
        if (state->done[i] == 0) {
          state->results[i].accepted = false;
          state->results[i].reason = RejectionReason::Cancelled;
          state->results[i].detail = "engine stopped before the submission was processed";
          state->done[i] = 1;
          ++cancelled_;
        }
      }
      state->outstanding = 0;
      state->cv.notify_all();
    }
    inbox_waiters_.clear();
  }

  const Timestamp now = clock_->now();
  const std::size_t idle = episodes_->close_idle(now);
  const std::size_t sealed = episodes_->seal_all(EpisodeState::ClosedExplicitly, now,
                                                "sealed because the engine stopped");
  restart_seals_.fetch_add(sealed, std::memory_order_acq_rel);
  (void)idle;
  if (persistence_) {
    const Result<void> declared = persist();
    if (!declared.ok()) {
      ++persistence_failures_;
      stopped_cleanly = false;
    }
    for (const LossEpisode& episode : episodes_->episodes()) {
      const Result<void> written = persistence_->append_episode(episode);
      if (!written.ok()) {
        ++persistence_failures_;
        stopped_cleanly = false;
      }
    }
    const Result<void> flushed = persistence_->flush();
    if (!flushed.ok()) {
      ++persistence_failures_;
      stopped_cleanly = false;
    }
    const Result<void> closed = persistence_->close();
    if (!closed.ok()) {
      ++persistence_failures_;
      stopped_cleanly = false;
    }
  }
  state_.store(static_cast<int>(stopped_cleanly ? EngineState::Stopped : EngineState::Failed),
               std::memory_order_release);
  return stopped_cleanly ? Result<void>{}
                         : Result<void>{make_status(StatusCode::Internal,
                                                    "engine stopped but persistence did not close cleanly")};
}

void ObservatoryEngine::worker_main(std::stop_token token) {
  WorkItem item{};
  while (inbox_ && inbox_->pop(item, token)) {
    const IngestOutcome outcome = submit_sync(std::move(item.item));
    if (item.batch) {
      std::lock_guard<std::mutex> guard(item.batch->mutex);
      if (item.slot < item.batch->results.size() && item.batch->done[item.slot] == 0) {
        item.batch->results[item.slot] = outcome;
        item.batch->done[item.slot] = 1;
        if (item.batch->outstanding > 0) {
          --item.batch->outstanding;
        }
      }
      item.batch->cv.notify_all();
    }
    completions_.fetch_add(1, std::memory_order_acq_rel);
  }
}

void ObservatoryEngine::record_generations(const EvidenceItem& item) {
  const SubjectRef subject = item.header.subject;
  const Timestamp observed = item.header.observed_at.value;
  switch (subject.kind()) {
    case SubjectKind::Flow:
      generations_->observe_flow(FlowId::from_value(subject.raw_id()), item.header.generation, observed);
      return;
    case SubjectKind::Path:
      generations_->observe_path(PathId::from_value(subject.raw_id()), item.header.generation, observed);
      return;
    case SubjectKind::Hop:
    case SubjectKind::Link:
    case SubjectKind::Queue:
      break;
    case SubjectKind::Source:
    case SubjectKind::Unknown:
      return;
  }
  // Evidence about an entity on a path is also evidence about the generations
  // of every flow that uses that path. Only declared bindings are considered:
  // the runtime never guesses which flow an entity belongs to.
  for (const FlowBinding& flow : topology_->flows()) {
    const Result<std::vector<Hop>> hops = topology_->hops_of(flow.path);
    if (!hops.ok()) {
      continue;
    }
    bool matches = false;
    for (const Hop& hop : hops.value()) {
      if (subject.kind() == SubjectKind::Hop && hop.id.value() == subject.raw_id()) {
        matches = true;
        break;
      }
      if (subject.kind() == SubjectKind::Link &&
          (hop.ingress_link.value() == subject.raw_id() || hop.egress_link.value() == subject.raw_id())) {
        matches = true;
        break;
      }
      if (subject.kind() == SubjectKind::Queue) {
        const Result<QueueEntity> queue = topology_->find_queue(QueueId::from_value(subject.raw_id()));
        if (queue.ok() && queue.value().node == hop.node &&
            (queue.value().port == hop.egress_port || queue.value().port == hop.ingress_port)) {
          matches = true;
          break;
        }
      }
    }
    if (matches) {
      generations_->observe_flow(flow.id, item.header.generation, observed);
    }
  }
}

IngestOutcome ObservatoryEngine::submit_sync(EvidenceItem item) {
  record_generations(item);
  EvidenceItem persisted_copy = item;
  const IngestOutcome outcome = evidence_->submit(std::move(item));
  if (outcome.accepted && persistence_ != nullptr && config_.persist_evidence_on_ingest) {
    const Result<void> written = persistence_->append_evidence(persisted_copy);
    if (!written.ok()) {
      persistence_failures_.fetch_add(1, std::memory_order_acq_rel);
    }
  }
  return outcome;
}

Result<IngestOutcome> ObservatoryEngine::ingest(EvidenceItem item) {
  const EngineState current = state();
  if (current != EngineState::Running) {
    return make_status(StatusCode::ShuttingDown, "engine is not running");
  }
  submissions_.fetch_add(1, std::memory_order_acq_rel);
  return submit_sync(std::move(item));
}

Result<std::vector<IngestOutcome>> ObservatoryEngine::ingest_batch(std::vector<EvidenceItem> items) {
  const EngineState current = state();
  if (current != EngineState::Running) {
    return make_status(StatusCode::ShuttingDown, "engine is not running");
  }
  if (items.size() > config_.max_batch_items) {
    return make_status(StatusCode::CapacityExceeded, "batch exceeds the configured item bound");
  }
  if (!inbox_) {
    std::vector<IngestOutcome> outcomes;
    outcomes.reserve(items.size());
    for (EvidenceItem& item : items) {
      submissions_.fetch_add(1, std::memory_order_acq_rel);
      outcomes.push_back(submit_sync(std::move(item)));
      completions_.fetch_add(1, std::memory_order_acq_rel);
    }
    return outcomes;
  }

  const std::size_t count = items.size();
  auto state = std::make_shared<BatchState>();
  state->results.assign(count, IngestOutcome{});
  state->done.assign(count, 0);
  state->outstanding = count;
  {
    std::lock_guard<std::mutex> guard(inbox_waiters_mutex_);
    inbox_waiters_.erase(
        std::remove_if(inbox_waiters_.begin(), inbox_waiters_.end(),
                       [](const std::weak_ptr<BatchState>& weak) { return weak.expired(); }),
        inbox_waiters_.end());
    inbox_waiters_.push_back(state);
  }

  for (std::size_t i = 0; i < count; ++i) {
    WorkItem work{};
    work.item = std::move(items[i]);
    work.slot = i;
    work.submission = submissions_.fetch_add(1, std::memory_order_acq_rel) + 1;
    work.batch = state;
    const PushOutcome pushed = inbox_->push(std::move(work), stop_source_.get_token());
    if (pushed != PushOutcome::Accepted) {
      std::lock_guard<std::mutex> guard(state->mutex);
      state->results[i].accepted = false;
      state->results[i].reason = RejectionReason::Cancelled;
      state->results[i].detail = std::string("submission refused: ") + std::string(to_string(pushed));
      state->done[i] = 1;
      if (state->outstanding > 0) {
        --state->outstanding;
      }
      cancelled_.fetch_add(1, std::memory_order_acq_rel);
    }
  }
  {
    std::unique_lock<std::mutex> lock(state->mutex);
    state->cv.wait(lock, stop_source_.get_token(), [&state] { return state->outstanding == 0; });
    // Whatever the reason for waking, every slot is given a definite outcome so
    // a caller can never mistake "not processed" for "processed".
    for (std::size_t i = 0; i < state->done.size(); ++i) {
      if (state->done[i] == 0) {
        state->results[i].accepted = false;
        state->results[i].reason = RejectionReason::Cancelled;
        state->results[i].detail = "submission was not processed before the engine stopped";
        state->done[i] = 1;
        cancelled_.fetch_add(1, std::memory_order_acq_rel);
      }
    }
    state->outstanding = 0;
    std::vector<IngestOutcome> outcomes = state->results;
    return outcomes;
  }
}

FreshnessContext ObservatoryEngine::freshness_context() const noexcept {
  FreshnessContext context{};
  context.sources = sources_.get();
  context.generations = generations_.get();
  context.topology = topology_.get();
  return context;
}

DerivationResult ObservatoryEngine::derive_snapshot() const {
  DerivationContext context{};
  context.topology = topology_.get();
  context.generations = generations_.get();
  context.sources = sources_.get();
  context.clock_synchronized = config_.clock_synchronized;
  derivations_.fetch_add(1, std::memory_order_acq_rel);
  return derive_observations(evidence_->snapshot(), context, config_.derive);
}

void ObservatoryEngine::record_aggregates(const DerivationResult& derivation) {
  std::lock_guard<std::mutex> guard(mutex_);
  for (const LossObservation& observation : derivation.observations) {
    if (aggregated_ids_.find(observation.id) != aggregated_ids_.end()) {
      continue;
    }
    if (aggregated_ids_.size() >= config_.store.max_items) {
      aggregation_bound_hit_ = true;
      continue;
    }
    aggregated_ids_.insert(observation.id);
    aggregate_->add(observation, classify_single_observation(observation), observation.observed_at.value);
  }
}

void ObservatoryEngine::record_episode(const Classification& classification, SubjectRef subject,
                                       Timestamp at) {
  if (subject.kind() != SubjectKind::Flow) {
    return;
  }
  const FlowId flow = FlowId::from_value(subject.raw_id());
  PathId path{};
  GenerationId generation{};
  const Result<FlowBinding> binding = topology_->find_flow(flow);
  if (binding.ok()) {
    path = binding.value().path;
    generation = binding.value().generation;
  }
  const EpisodeId id = episodes_->observe(classification, flow, path, generation, at);
  if (id.is_nil()) {
    return;
  }
  episodes_recorded_.fetch_add(1, std::memory_order_acq_rel);
  if (persistence_ == nullptr) {
    return;
  }
  // The journal carries episode evolution, so a restart observes the episode as
  // it actually stood rather than only as it was first opened.
  const Result<LossEpisode> episode = episodes_->find(id);
  if (!episode.ok()) {
    return;
  }
  const Result<void> written = persistence_->append_episode(episode.value());
  if (!written.ok()) {
    persistence_failures_.fetch_add(1, std::memory_order_acq_rel);
  }
}

Result<DerivationResult> ObservatoryEngine::derive_all() const { return derive_snapshot(); }

Classification ObservatoryEngine::classify_observations(SubjectRef subject,
                                                      const std::vector<LossObservation>& observations,
                                                      const DerivationResult& derivation,
                                                      Timestamp at) const {
  ClassificationInput input{};
  input.subject = subject;
  input.observations = observations;
  input.evaluated_at = at;
  input.bounds = derivation.bounds.notes();
  if (derivation.bounds.truncated()) {
    input.bounds.push_back(BoundNote{BoundKind::ResultSet, 0, 0, "derivation-bounds"});
  }
  const Classifier classifier(freshness_context(), config_.loss);
  Classification classification = classifier.classify(input);
  if (classification.klass == LossClass::AbsentEvidence &&
      classification.rejections.empty()) {
    classification.rejections = derivation.rejections;
    if (classification.rejections.size() > config_.max_result_items) {
      classification.rejections.resize(config_.max_result_items);
    }
  }
  return classification;
}

Result<Classification> ObservatoryEngine::classify(SubjectRef subject, Timestamp at) {
  if (subject.is_unknown()) {
    return make_status(StatusCode::InvalidArgument, "classification requires a subject");
  }
  const Timestamp instant = at.is_zero() ? clock_->now() : at;
  const DerivationResult derivation = derive_snapshot();
  std::vector<LossObservation> selected;
  for (const LossObservation& observation : derivation.observations) {
    if (observation.subject == subject) {
      if (selected.size() >= config_.max_result_items) {
        break;
      }
      selected.push_back(observation);
    }
  }
  if (config_.aggregate_on_classify) {
    record_aggregates(derivation);
  }
  classifications_.fetch_add(1, std::memory_order_acq_rel);
  Classification classification = classify_observations(subject, selected, derivation, instant);
  record_episode(classification, subject, instant);
  return classification;
}

Result<Classification> ObservatoryEngine::classify_flow(FlowId flow, Timestamp at) {
  return classify(SubjectRef::flow(flow), at);
}

Result<LocalizationResult> ObservatoryEngine::localize(const LocalizationRequest& request) {
  const Timestamp instant = request.at.is_zero() ? clock_->now() : request.at;
  const DerivationResult derivation = derive_snapshot();
  LocalizationRequest effective = request;
  effective.at = instant;
  if (effective.max_segments == 0 || effective.max_segments > config_.max_result_items) {
    effective.max_segments = config_.max_result_items;
  }
  Localizer localizer(*topology_, config_.localize);
  if (config_.aggregate_on_classify) {
    record_aggregates(derivation);
  }
  localizations_.fetch_add(1, std::memory_order_acq_rel);
  return localizer.localize(effective, derivation.observations, instant, config_.loss,
                            freshness_context());
}

Result<EpisodeQueryResult> ObservatoryEngine::history(const EpisodeQuery& request) {
  EpisodeQuery effective = request;
  if (effective.max_results == 0 || effective.max_results > config_.max_result_items) {
    effective.max_results = config_.max_result_items;
  }
  return episodes_->query(effective);
}

Result<Explanation> ObservatoryEngine::explain(const ExplainRequest& request) {
  if (request.subject.is_unknown()) {
    return make_status(StatusCode::InvalidArgument, "explanation requires a subject");
  }
  const Timestamp instant = request.at.is_zero() ? clock_->now() : request.at;
  const DerivationResult derivation = derive_snapshot();

  std::vector<LossObservation> selected;
  for (const LossObservation& observation : derivation.observations) {
    if (observation.subject == request.subject) {
      if (selected.size() >= config_.max_result_items) {
        break;
      }
      selected.push_back(observation);
    }
  }

  if (config_.aggregate_on_classify) {
    record_aggregates(derivation);
  }

  Explanation explanation{};
  explanation.subject = request.subject;
  explanation.at = instant;
  explanation.classification = classify_observations(request.subject, selected, derivation, instant);
  explanation.klass = explanation.classification.klass;
  explanation.evidence_ids = explanation.classification.evidence_ids;
  explanation.rejections = explanation.classification.rejections;
  explanation.bounds = explanation.classification.bounds;

  if (request.include_localization && request.subject.kind() == SubjectKind::Flow) {
    LocalizationRequest localization_request{};
    localization_request.flow = FlowId::from_value(request.subject.raw_id());
    localization_request.at = instant;
    localization_request.requested_max_granularity = request.requested_max_granularity;
    localization_request.max_segments = std::min<std::size_t>(config_.max_result_items, 64);
    Localizer localizer(*topology_, config_.localize);
    explanation.localization =
        localizer.localize(localization_request, derivation.observations, instant, config_.loss,
                           freshness_context());
    explanation.has_localization = true;
  }

  if (request.include_history && request.subject.kind() == SubjectKind::Flow) {
    EpisodeQuery query{};
    query.flow = FlowId::from_value(request.subject.raw_id());
    query.max_results = std::min(request.max_history_episodes, config_.max_result_items);
    explanation.history = episodes_->query(query);
  }

  if (config_.aggregate_on_classify) {
    std::lock_guard<std::mutex> guard(mutex_);
    explanation.aggregates = aggregate_->buckets();
    if (explanation.aggregates.size() > config_.max_result_items) {
      explanation.aggregates.resize(config_.max_result_items);
      explanation.truncated = true;
    }
  }

  std::string text = render_explanation(request, explanation);
  const std::size_t limit = request.max_lines == 0 ? 512 : request.max_lines;
  std::size_t lines = 1;
  for (const char ch : text) {
    if (ch == '\n') {
      ++lines;
    }
  }
  if (lines > limit) {
    std::size_t seen = 0;
    std::size_t cutoff = text.size();
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (text[i] == '\n') {
        ++seen;
        if (seen + 1 > limit) {
          cutoff = i + 1;
          break;
        }
      }
    }
    text.resize(cutoff);
    text.append("  note           : output truncated at the configured line bound\n");
    explanation.truncated = true;
  }
  explanation.lines.reserve(lines);
  {
    std::string current;
    for (const char ch : text) {
      if (ch == '\n') {
        explanation.lines.push_back(current);
        current.clear();
        continue;
      }
      current.push_back(ch);
    }
    if (!current.empty()) {
      explanation.lines.push_back(current);
    }
  }
  explanation.text = std::move(text);
  return explanation;
}

Result<ExportBundle> ObservatoryEngine::export_bundle(const ExportRequest& request) {
  ExportBundle bundle{};
  bundle.scope = request.scope;
  bundle.format = request.format;
  bundle.provenance_lines.push_back("product=" + full_version_string());
  bundle.provenance_lines.push_back("persist-format=" + std::to_string(kPersistFormatVersion));
  bundle.provenance_lines.push_back("semantics-revision=" + std::to_string(kSemanticsRevision));
  bundle.provenance_lines.push_back(std::string("scope=") + std::string(to_string(request.scope)));
  bundle.provenance_lines.push_back(std::string("format=") + std::string(to_string(request.format)));

  const std::size_t limit = request.max_items == 0 ? 1 : request.max_items;
  std::size_t items = 0;
  auto append_text = [&](const std::string& piece) {
    if (items >= limit) {
      bundle.truncated = true;
      return;
    }
    bundle.text.append(piece);
    ++items;
  };
  auto append_binary = [&](const std::vector<std::uint8_t>& payload) {
    if (items >= limit) {
      bundle.truncated = true;
      return;
    }
    std::uint8_t frame[8] = {};
    ByteWriter writer{MutableByteSpan{frame, 8}};
    writer.u32(static_cast<std::uint32_t>(payload.size()));
    writer.u32(crc32c(ByteSpan{payload}));
    bundle.binary.insert(bundle.binary.end(), frame, frame + 8);
    bundle.binary.insert(bundle.binary.end(), payload.begin(), payload.end());
    ++items;
  };

  const bool want_topology = request.scope == ExportScope::Topology || request.scope == ExportScope::All;
  const bool want_sources = request.scope == ExportScope::Sources || request.scope == ExportScope::All;
  const bool want_evidence = request.scope == ExportScope::Evidence || request.scope == ExportScope::All;
  const bool want_episodes = request.scope == ExportScope::Episodes || request.scope == ExportScope::All;
  const bool want_aggregates = request.scope == ExportScope::Aggregates || request.scope == ExportScope::All;

  if (request.format != ExportFormat::Binary) {
    for (const std::string& line : bundle.provenance_lines) {
      append_text("# " + line + "\n");
    }
  }
  if (want_topology) {
    if (request.format == ExportFormat::Binary) {
      for (const Link& link : topology_->links()) {
        append_binary(encode_topology_entity(link));
      }
      for (const Path& path : topology_->paths()) {
        append_binary(encode_topology_entity(path));
      }
      for (const QueueEntity& queue : topology_->queues()) {
        append_binary(encode_topology_entity(queue));
      }
      for (const FlowBinding& flow : topology_->flows()) {
        append_binary(encode_topology_entity(flow));
      }
    } else {
      append_text(render_topology(*topology_));
    }
  }
  if (want_sources) {
    if (request.format == ExportFormat::Binary) {
      for (const SourceDescriptor& descriptor : sources_->sources()) {
        append_binary(encode_source(descriptor));
      }
      for (const SourceIncarnation& incarnation : sources_->incarnations()) {
        append_binary(encode_incarnation(incarnation));
      }
    } else {
      append_text(render_sources(*sources_));
    }
  }
  if (want_evidence) {
    for (const EvidenceItem& item : evidence_->snapshot()) {
      if (!request.include_recovered && item.header.recovered_from_persistence) {
        continue;
      }
      if (request.flow.has_value() && !(item.header.subject.kind() == SubjectKind::Flow &&
                                        item.header.subject.raw_id() == request.flow.value().value())) {
        continue;
      }
      if (!request.from.is_zero() && item.header.observed_at.value < request.from) {
        continue;
      }
      if (!request.to.is_zero() && item.header.observed_at.value > request.to) {
        continue;
      }
      if (!request.include_synthetic && semantics_of(item.header.method).synthetic) {
        continue;
      }
      if (request.format == ExportFormat::Binary) {
        append_binary(encode_evidence(item));
      } else {
        append_text(render_evidence(item) + "\n");
      }
      if (bundle.truncated) {
        break;
      }
    }
  }
  if (want_episodes) {
    for (const LossEpisode& episode : episodes_->episodes()) {
      if (request.format == ExportFormat::Binary) {
        append_binary(encode_episode(episode));
      } else {
        // Historical, not declared: commented so the export stays a scenario.
        append_text("# " + render_episode(episode) + "\n");
      }
      if (bundle.truncated) {
        break;
      }
    }
  }
  if (want_aggregates) {
    for (const AggregateBucket& bucket : aggregate_->buckets()) {
      // Derived, not declared: commented so the export stays a valid scenario.
      append_text("# " + render_aggregate(bucket) + "\n");
      if (bundle.truncated) {
        break;
      }
    }
  }
  bundle.item_count = items;
  if (bundle.truncated) {
    bundle.bounds.push_back(BoundNote{BoundKind::ExportItems, static_cast<std::uint64_t>(limit),
                                      static_cast<std::uint64_t>(limit + 1), "export"});
  }
  if (aggregation_bound_hit_) {
    bundle.bounds.push_back(BoundNote{BoundKind::AggregationWindows,
                                      static_cast<std::uint64_t>(config_.store.max_items),
                                      static_cast<std::uint64_t>(config_.store.max_items + 1),
                                      "aggregation-tracking"});
  }
  return bundle;
}

Result<RecoveryReport> ObservatoryEngine::load() {
  if (!persistence_) {
    RecoveryReport report{};
    report.opened = false;
    report.detail = "persistence is not configured; nothing to recover";
    return report;
  }
  const Timestamp now = clock_->now();
  Result<RecoveryReport> report = persistence_->load(*topology_, *sources_, *evidence_, *episodes_, now);
  if (!report.ok()) {
    return report;
  }
  restart_seals_.fetch_add(report.value().episodes_sealed, std::memory_order_acq_rel);
  return report;
}

Result<void> ObservatoryEngine::persist() {
  if (!persistence_) {
    return {};
  }
  // Declarations are snapshotted whenever they changed since the last write, so
  // a restart recovers the topology and source registration that the evidence
  // refers to rather than evidence alone.
  if (topology_->revision() != persisted_topology_revision_) {
    const Result<void> written = persistence_->append_topology(*topology_);
    if (!written.ok()) {
      ++persistence_failures_;
      return written;
    }
    persisted_topology_revision_ = topology_->revision();
  }

  const std::vector<SourceDescriptor> descriptors = sources_->sources();
  const std::vector<SourceIncarnation> incarnations = sources_->incarnations();
  if (descriptors.size() != persisted_source_count_ ||
      incarnations.size() != persisted_incarnation_count_) {
    for (const SourceDescriptor& descriptor : descriptors) {
      const Result<void> written = persistence_->append_source(descriptor);
      if (!written.ok()) {
        ++persistence_failures_;
        return written;
      }
    }
    for (const SourceIncarnation& incarnation : incarnations) {
      const Result<void> written = persistence_->append_incarnation(incarnation);
      if (!written.ok()) {
        ++persistence_failures_;
        return written;
      }
    }
    persisted_source_count_ = descriptors.size();
    persisted_incarnation_count_ = incarnations.size();
  }

  if (config_.auto_compact && persistence_->should_compact()) {
    const Result<void> compacted = persistence_->compact(*topology_, *sources_, *evidence_, *episodes_);
    if (!compacted.ok()) {
      ++persistence_failures_;
      return compacted;
    }
    persisted_topology_revision_ = topology_->revision();
    persisted_source_count_ = descriptors.size();
    persisted_incarnation_count_ = incarnations.size();
  }
  return persistence_->flush();
}

EngineEpoch ObservatoryEngine::epoch() const noexcept {
  return persistence_ ? persistence_->epoch() : EngineEpoch{};
}

EngineStats ObservatoryEngine::stats() const {
  EngineStats stats{};
  stats.state = state();
  stats.workers = workers_.size();
  if (inbox_) {
    stats.queue = inbox_->stats();
  }
  stats.store = evidence_->stats();
  if (persistence_) {
    stats.persist = persistence_->stats();
  } else {
    stats.persist = PersistStats{};
  }
  stats.submissions = submissions_.load(std::memory_order_acquire);
  stats.completions = completions_.load(std::memory_order_acquire);
  stats.worker_errors = worker_errors_.load(std::memory_order_acquire);
  stats.derivations = derivations_.load(std::memory_order_acquire);
  stats.classifications = classifications_.load(std::memory_order_acquire);
  stats.localizations = localizations_.load(std::memory_order_acquire);
  stats.episodes_recorded = episodes_recorded_.load(std::memory_order_acquire);
  stats.restart_seals = restart_seals_.load(std::memory_order_acquire);
  stats.persistence_failures = persistence_failures_.load(std::memory_order_acquire);
  stats.cancelled = cancelled_.load(std::memory_order_acquire);
  return stats;
}

}  // namespace loss_observatory
