#include <string>
#include <vector>

#include "fixtures.hpp"
#include "loss_observatory/model/generation.hpp"
#include "loss_observatory/model/method.hpp"
#include "loss_observatory/model/source.hpp"
#include "loss_observatory/model/topology.hpp"

using namespace loss_observatory;

namespace {

Endpoint endpoint(const char* node, const char* port) {
  return Endpoint{NodeId::from_canonical_text(node), PortId::from_canonical_text(port)};
}

Link make_link(const char* id, const char* a_node, const char* b_node) {
  Link link{};
  link.id = LinkId::from_canonical_text(id);
  link.a = endpoint(a_node, "0");
  link.b = endpoint(b_node, "0");
  link.kind = LinkKind::PhysicalPort;
  return link;
}

Hop make_hop(const char* id, std::uint32_t index, const char* node) {
  Hop hop{};
  hop.id = HopId::from_canonical_text(id);
  hop.index = index;
  hop.node = NodeId::from_canonical_text(node);
  hop.ingress_port = PortId::from_canonical_text("0");
  hop.egress_port = PortId::from_canonical_text("1");
  return hop;
}

}  // namespace

LO_TEST(topology, upsert_reports_inserted_unchanged_and_conflicted) {
  TopologyRegistry topology{};
  const Link link = make_link("l1", "n1", "n2");
  auto first = topology.upsert_link(link);
  LO_REQUIRE(first.ok());
  LO_CHECK(first.value().outcome == UpsertOutcome::Inserted);

  auto again = topology.upsert_link(link);
  LO_REQUIRE(again.ok());
  LO_CHECK(again.value().outcome == UpsertOutcome::Unchanged);

  Link changed = link;
  changed.kind = LinkKind::VirtualLink;
  auto conflicted = topology.upsert_link(changed, false);
  LO_CHECK(!conflicted.ok());
  LO_CHECK(conflicted.code() == StatusCode::Conflict);

  auto replaced = topology.upsert_link(changed, true);
  LO_REQUIRE(replaced.ok());
  LO_CHECK(replaced.value().outcome == UpsertOutcome::Replaced);
  LO_CHECK_EQ(topology.link_count(), 1ULL);
  LO_REQUIRE(topology.find_link(link.id).ok());
  LO_CHECK(topology.find_link(link.id).value().kind == LinkKind::VirtualLink);
}

LO_TEST(topology, revision_advances_only_on_real_mutation) {
  TopologyRegistry topology{};
  const RevisionId before = topology.revision();
  const Link link = make_link("l1", "n1", "n2");
  (void)topology.upsert_link(link);
  const RevisionId after_insert = topology.revision();
  LO_CHECK(after_insert != before);
  (void)topology.upsert_link(link);
  LO_CHECK(topology.revision() == after_insert);
}

LO_TEST(topology, nil_identity_and_duplicate_hop_index_are_rejected) {
  TopologyRegistry topology{};
  Link nil_link{};
  LO_CHECK(!topology.upsert_link(nil_link).ok());

  Path path{};
  path.id = PathId::from_canonical_text("p1");
  path.hops.push_back(make_hop("h1", 0, "n1"));
  path.hops.push_back(make_hop("h2", 0, "n2"));
  auto result = topology.upsert_path(path);
  LO_CHECK(!result.ok());
  LO_CHECK(result.code() == StatusCode::InvalidArgument);
}

LO_TEST(topology, hops_are_returned_in_index_order_and_unknown_paths_fail) {
  TopologyRegistry topology{};
  Path path{};
  path.id = PathId::from_canonical_text("p1");
  path.hops.push_back(make_hop("h2", 1, "n2"));
  path.hops.push_back(make_hop("h1", 0, "n1"));
  (void)topology.upsert_path(path);

  auto hops = topology.hops_of(path.id);
  LO_REQUIRE(hops.ok());
  LO_REQUIRE(hops.value().size() == 2ULL);
  LO_CHECK_EQ(hops.value()[0].index, 0U);
  LO_CHECK_EQ(hops.value()[1].index, 1U);

  LO_CHECK(!topology.hops_of(PathId::from_canonical_text("missing")).ok());
  LO_CHECK(!topology.hop_at(path.id, 5).ok());
  LO_REQUIRE(topology.hop_at(path.id, 1).ok());
  LO_CHECK(topology.hop_at(path.id, 1).value().id == HopId::from_canonical_text("h2"));
}

LO_TEST(topology, path_revision_is_assigned_when_absent) {
  TopologyRegistry topology{};
  Path path{};
  path.id = PathId::from_canonical_text("p1");
  path.hops.push_back(make_hop("h1", 0, "n1"));
  (void)topology.upsert_path(path);
  auto stored = topology.find_path(path.id);
  LO_REQUIRE(stored.ok());
  LO_CHECK(!stored.value().revision.is_nil());
}

LO_TEST(topology, declared_limits_are_enforced) {
  TopologyLimits limits{};
  limits.max_links = 2;
  TopologyRegistry topology{limits};
  LO_CHECK(topology.upsert_link(make_link("l1", "n1", "n2")).ok());
  LO_CHECK(topology.upsert_link(make_link("l2", "n2", "n3")).ok());
  auto third = topology.upsert_link(make_link("l3", "n3", "n4"));
  LO_CHECK(!third.ok());
  LO_CHECK(third.code() == StatusCode::CapacityExceeded);
}

LO_TEST(source, registration_is_idempotent_and_conflict_aware) {
  SourceRegistry registry{};
  SourceDescriptor descriptor{};
  descriptor.id = SourceId::from_canonical_text("s1");
  descriptor.name = "leaf-a";
  descriptor.kind = SourceKind::CounterTelemetry;
  descriptor.authority = SourceAuthority::Primary;
  auto first = registry.register_source(descriptor);
  LO_REQUIRE(first.ok());
  LO_CHECK(first.value() == UpsertOutcome::Inserted);
  auto again = registry.register_source(descriptor);
  LO_REQUIRE(again.ok());
  LO_CHECK(again.value() == UpsertOutcome::Unchanged);

  SourceDescriptor changed = descriptor;
  changed.authority = SourceAuthority::Advisory;
  LO_CHECK(!registry.register_source(changed).ok());
  LO_REQUIRE(registry.register_source(changed, true).ok());
  LO_REQUIRE(registry.authority_of(descriptor.id).ok());
  LO_CHECK(registry.authority_of(descriptor.id).value() == SourceAuthority::Advisory);
}

LO_TEST(source, activating_an_incarnation_retires_the_previous_one) {
  SourceRegistry registry{};
  SourceDescriptor descriptor{};
  descriptor.id = SourceId::from_canonical_text("s1");
  descriptor.name = "leaf-a";
  descriptor.kind = SourceKind::CounterTelemetry;
  descriptor.authority = SourceAuthority::Primary;
  (void)registry.register_source(descriptor);

  const Timestamp start = Timestamp::from_unix_seconds(100);
  auto first = registry.activate_incarnation(descriptor.id, "boot-1", start);
  LO_REQUIRE(first.ok());
  auto second = registry.activate_incarnation(descriptor.id, "boot-2", start + Duration::from_seconds(10));
  LO_REQUIRE(second.ok());
  LO_CHECK(first.value() != second.value());

  auto old_incarnation = registry.find_incarnation(descriptor.id, first.value());
  LO_REQUIRE(old_incarnation.ok());
  LO_CHECK(old_incarnation.value().retired);
  auto current = registry.current_incarnation(descriptor.id);
  LO_REQUIRE(current.ok());
  LO_CHECK(current.value().epoch == second.value());
  LO_CHECK(!current.value().retired);
}

LO_TEST(source, sequence_fence_accepts_advances_and_rejects_replays) {
  SourceRegistry registry{};
  SourceDescriptor descriptor{};
  descriptor.id = SourceId::from_canonical_text("s1");
  descriptor.name = "leaf-a";
  descriptor.kind = SourceKind::CounterTelemetry;
  descriptor.authority = SourceAuthority::Primary;
  (void)registry.register_source(descriptor);
  auto epoch = registry.activate_incarnation(descriptor.id, "boot-1", Timestamp::from_unix_seconds(1));
  LO_REQUIRE(epoch.ok());

  auto first = registry.observe_sequence(descriptor.id, epoch.value(), SequenceId::from_value(5));
  LO_REQUIRE(first.ok());
  LO_CHECK(first.value().outcome == FenceOutcome::Accepted);

  auto advanced = registry.observe_sequence(descriptor.id, epoch.value(), SequenceId::from_value(9));
  LO_REQUIRE(advanced.ok());
  LO_CHECK(advanced.value().outcome == FenceOutcome::Accepted);
  LO_CHECK(advanced.value().high_water == SequenceId::from_value(9));

  auto replay = registry.observe_sequence(descriptor.id, epoch.value(), SequenceId::from_value(9));
  LO_REQUIRE(replay.ok());
  LO_CHECK(replay.value().outcome == FenceOutcome::Replayed);
  LO_CHECK(!replay.value().admissible);

  auto older = registry.observe_sequence(descriptor.id, epoch.value(), SequenceId::from_value(7));
  LO_REQUIRE(older.ok());
  LO_CHECK(older.value().outcome == FenceOutcome::Reordered);
  LO_CHECK(older.value().admissible);
}

LO_TEST(source, unknown_source_and_stale_epoch_are_distinguished) {
  SourceRegistry registry{};
  auto unknown = registry.observe_sequence(SourceId::from_canonical_text("nope"),
                                           EpochId::from_canonical_text("e1"), SequenceId::from_value(1));
  LO_REQUIRE(unknown.ok());
  LO_CHECK(unknown.value().outcome == FenceOutcome::UnknownSource);
  LO_CHECK(!unknown.value().admissible);

  SourceDescriptor descriptor{};
  descriptor.id = SourceId::from_canonical_text("s1");
  descriptor.name = "leaf-a";
  descriptor.kind = SourceKind::CounterTelemetry;
  descriptor.authority = SourceAuthority::Primary;
  (void)registry.register_source(descriptor);

  auto no_incarnation = registry.observe_sequence(descriptor.id, EpochId::from_canonical_text("e1"),
                                                  SequenceId::from_value(1));
  LO_REQUIRE(no_incarnation.ok());
  LO_CHECK(no_incarnation.value().outcome == FenceOutcome::NoActiveIncarnation);

  auto first = registry.activate_incarnation(descriptor.id, "boot-1", Timestamp::from_unix_seconds(1));
  LO_REQUIRE(first.ok());
  auto second = registry.activate_incarnation(descriptor.id, "boot-2", Timestamp::from_unix_seconds(2));
  LO_REQUIRE(second.ok());
  auto stale = registry.observe_sequence(descriptor.id, first.value(), SequenceId::from_value(1));
  LO_REQUIRE(stale.ok());
  LO_CHECK(stale.value().outcome == FenceOutcome::StaleEpoch);
  LO_CHECK(!stale.value().admissible);
}

LO_TEST(source, restored_incarnations_are_retired_on_arrival) {
  SourceRegistry registry{};
  SourceDescriptor descriptor{};
  descriptor.id = SourceId::from_canonical_text("s1");
  descriptor.name = "leaf-a";
  descriptor.kind = SourceKind::CounterTelemetry;
  descriptor.authority = SourceAuthority::Primary;
  (void)registry.register_source(descriptor);

  SourceIncarnation incarnation{};
  incarnation.source = descriptor.id;
  incarnation.epoch = EpochId::from_canonical_text("epoch/old");
  incarnation.incarnation = "boot-old";
  incarnation.activated_at = Timestamp::from_unix_seconds(50);
  LO_CHECK(registry.restore_incarnation(incarnation).ok());

  auto stored = registry.find_incarnation(descriptor.id, incarnation.epoch);
  LO_REQUIRE(stored.ok());
  LO_CHECK(stored.value().retired);
  auto fenced = registry.observe_sequence(descriptor.id, incarnation.epoch, SequenceId::from_value(1));
  LO_REQUIRE(fenced.ok());
  LO_CHECK(fenced.value().outcome == FenceOutcome::StaleEpoch);
}

LO_TEST(generation, correlation_is_a_function_of_the_recorded_set) {
  GenerationRegistry forward{};
  GenerationRegistry reverse{};
  const FlowId flow = FlowId::from_canonical_text("flow/a");
  const Timestamp t0 = Timestamp::from_unix_seconds(100);
  const Timestamp t1 = Timestamp::from_unix_seconds(200);
  const GenerationId first = GenerationId::from_canonical_text("gen/1");
  const GenerationId second = GenerationId::from_canonical_text("gen/2");

  forward.observe_flow(flow, first, t0);
  forward.observe_flow(flow, second, t1);
  reverse.observe_flow(flow, second, t1);
  reverse.observe_flow(flow, first, t0);

  const GenerationStatus a = forward.correlate_flow(flow, first, t0);
  const GenerationStatus b = reverse.correlate_flow(flow, first, t0);
  LO_CHECK(a.match == GenerationMatch::Current);
  LO_CHECK(b.match == GenerationMatch::Current);
  LO_CHECK(a.current == b.current);

  const GenerationStatus superseded = forward.correlate_flow(flow, first, t1);
  LO_CHECK(superseded.match == GenerationMatch::Superseded);
  LO_CHECK(superseded.generation_changed);
  LO_CHECK(superseded.current == second);

  const GenerationStatus early = forward.correlate_flow(flow, first, Timestamp::from_unix_seconds(50));
  LO_CHECK(early.match == GenerationMatch::UnknownGeneration);
}

LO_TEST(generation, unknown_entities_report_no_record) {
  GenerationRegistry registry{};
  const GenerationStatus status =
      registry.correlate_flow(FlowId::from_canonical_text("flow/missing"),
                              GenerationId::from_canonical_text("gen/1"), Timestamp::from_unix_seconds(1));
  LO_CHECK(status.match == GenerationMatch::NoGenerationRecorded);
}

LO_TEST(method, declared_capabilities_are_enforced) {
  const MethodSemantics& counter = semantics_of(MeasurementMethod::CounterDelta);
  LO_CHECK(counter.implemented);
  LO_CHECK(!is_finer_than(counter.max_granularity, Granularity::Queue));
  LO_CHECK(!counter.requires_synchronized_clocks);

  const MethodSemantics& rtt = semantics_of(MeasurementMethod::ProbeRoundTrip);
  LO_CHECK(rtt.max_granularity == Granularity::Path);
  LO_CHECK(rtt.directional);

  const MethodSemantics& owd = semantics_of(MeasurementMethod::ProbeOneWay);
  LO_CHECK(!owd.implemented);
  LO_CHECK(owd.requires_synchronized_clocks);
  LO_CHECK(!method_is_usable(MeasurementMethod::ProbeOneWay, true));
  LO_CHECK(!method_unusable_reason(MeasurementMethod::ProbeOneWay, true).empty());

  const MethodSemantics& sequence = semantics_of(MeasurementMethod::SequenceGap);
  LO_CHECK(sequence.max_granularity == Granularity::Flow);
  LO_CHECK(sequence.requires_stable_generation);

  const MethodSemantics& synthetic = semantics_of(MeasurementMethod::SyntheticInjection);
  LO_CHECK(synthetic.synthetic);
}

LO_TEST(method, every_method_round_trips_through_text) {
  for (const MeasurementMethod method : all_measurement_methods()) {
    const std::string_view text = to_string(method);
    auto parsed = parse_measurement_method(text);
    LO_REQUIRE(parsed.ok());
    LO_CHECK(parsed.value() == method);
  }
  LO_CHECK(!parse_measurement_method("made-up").ok());
}
