#include <algorithm>
#include <string>
#include <vector>

#include "fixtures.hpp"
#include "loss_observatory/report.hpp"
#include "loss_observatory/script.hpp"

using namespace loss_observatory;

namespace {

const char* kScenario =
    "# a declared scenario\n"
    "source id=s1 name=leaf-a kind=counter-telemetry authority=primary\n"
    "incarnation source=s1 epoch=E1 name=boot-1 at=2026-01-01T00:00:00Z active=true\n"
    "path id=p1 kind=synthetic hops=n1:0->n2:0,n2:0->n3:0\n"
    "queue id=q1 node=n2 port=0 priority=3 kind=egress-port-queue\n"
    "flow id=f1 src=n1:0 dst=n3:0 proto=tcp path=p1 gen=7\n"
    "counter source=s1 epoch=E1 gen=7 seq=1 counter=c1 scope=dropped-packets value=10 bits=32 "
    "class=flow subject=flow:f1\n"
    "counter source=s1 epoch=E1 gen=7 seq=2 counter=c1 scope=dropped-packets value=28 bits=32 "
    "class=flow subject=flow:f1\n"
    "op classify flow=f1\n";

}  // namespace

LO_TEST(script, parses_a_declared_scenario) {
  auto program = parse_script(kScenario);
  LO_REQUIRE(program.ok());
  LO_CHECK_EQ(program.value().error_count(), 0ULL);
  LO_CHECK(program.value().commands.size() >= 8ULL);
  LO_CHECK_EQ(program.value().commands.front().verb, std::string("source"));
  LO_CHECK_EQ(program.value().commands.back().verb, std::string("op"));
  LO_CHECK_EQ(program.value().commands.back().get("op"), std::string("classify"));
}

LO_TEST(script, rejects_unknown_verbs_keys_and_malformed_tokens) {
  auto unknown_verb = parse_script("banana id=1\n");
  LO_REQUIRE(unknown_verb.ok());
  LO_CHECK_EQ(unknown_verb.value().error_count(), 1ULL);
  LO_CHECK_EQ(unknown_verb.value().commands.size(), 0ULL);

  auto unknown_key = parse_script("source id=s1 name=x kind=synthetic authority=primary colour=red\n");
  LO_REQUIRE(unknown_key.ok());
  LO_CHECK_EQ(unknown_key.value().error_count(), 1ULL);

  auto duplicate_key = parse_script("source id=s1 id=s2 kind=synthetic authority=primary\n");
  LO_REQUIRE(duplicate_key.ok());
  LO_CHECK_EQ(duplicate_key.value().error_count(), 1ULL);

  auto no_equals = parse_script("source id\n");
  LO_REQUIRE(no_equals.ok());
  LO_CHECK_EQ(no_equals.value().error_count(), 1ULL);
}

LO_TEST(script, comments_and_trailing_notes_are_ignored) {
  auto program = parse_script("# leading comment\nsource id=s1 name=a kind=synthetic authority=primary # tail\n");
  LO_REQUIRE(program.ok());
  LO_CHECK_EQ(program.value().error_count(), 0ULL);
  LO_CHECK_EQ(program.value().commands.size(), 1ULL);
  LO_CHECK_EQ(program.value().commands.front().fields.size(), 4ULL);
}

LO_TEST(script, bounds_are_enforced_and_reported) {
  ScriptLimits limits{};
  limits.max_commands = 2;
  auto program = parse_script("source id=a name=a kind=synthetic authority=primary\n"
                              "source id=b name=b kind=synthetic authority=primary\n"
                              "source id=c name=c kind=synthetic authority=primary\n",
                              limits);
  LO_REQUIRE(program.ok());
  LO_CHECK(program.value().truncated);
  LO_CHECK_EQ(program.value().commands.size(), 2ULL);
  LO_CHECK_EQ(program.value().error_count(), 1ULL);

  ScriptLimits small_lines{};
  small_lines.max_line_bytes = 4;
  auto long_line = parse_script("source id=aaaaaaaaaa name=a kind=synthetic authority=primary\n", small_lines);
  LO_REQUIRE(long_line.ok());
  LO_CHECK_EQ(long_line.value().error_count(), 1ULL);
}

LO_TEST(script, value_helpers_behave_strictly) {
  LO_CHECK_EQ(parse_u64("0").value(), 0ULL);
  LO_CHECK_EQ(parse_u64("18446744073709551615").value(), 18446744073709551615ULL);
  LO_CHECK(!parse_u64("18446744073709551616").ok());
  LO_CHECK(!parse_u64("-1").ok());
  LO_CHECK(!parse_u64("").ok());
  LO_CHECK(!parse_u64("12a").ok());
  LO_CHECK_EQ(parse_i64("-42").value(), -42);
  LO_CHECK(!parse_i64("99999999999999999999").ok());
  LO_CHECK(parse_bool("true").value());
  LO_CHECK(!parse_bool("false").value());
  LO_CHECK(!parse_bool("maybe").ok());

  const Timestamp reference = Timestamp::from_unix_seconds(1000);
  LO_CHECK_EQ(parse_time_or_offset("+1500ms", reference).value().unix_nanos(), 1001500000000LL);
  LO_CHECK_EQ(parse_time_or_offset("-2s", reference).value().unix_nanos(), 998000000000LL);
  LO_CHECK_EQ(parse_time_or_offset("+5", reference).value().unix_nanos(), 1005000000000LL);
  LO_CHECK(!parse_time_or_offset("+5weeks", reference).ok());
  LO_CHECK_EQ(parse_time_or_offset("2026-01-01T00:00:00Z", reference).value().unix_nanos(),
              1767225600000000000LL);

  auto endpoint = parse_endpoint("n1:0");
  LO_REQUIRE(endpoint.ok());
  LO_CHECK(endpoint.value().node == NodeId::from_canonical_text("n1"));
  LO_CHECK(!parse_endpoint("n1").ok());

  auto derived = parse_hops("n1:0->n2:0,n2:0->n3:0");
  LO_REQUIRE(derived.ok());
  LO_CHECK_EQ(derived.value().size(), 2ULL);
  LO_CHECK_EQ(derived.value()[0].index, 0U);
  LO_CHECK(!parse_hops("").ok());
  LO_CHECK(!parse_hops("nonsense").ok());
}

LO_TEST(script, hop_identities_round_trip_through_the_explicit_form) {
  Path path{};
  path.id = PathId::from_canonical_text("p-rt");
  path.kind = PathKind::Configured;
  path.revision = RevisionId::from_canonical_text("rev-rt");
  Hop hop{};
  hop.id = HopId::from_canonical_text("hop-rt");
  hop.index = 0;
  hop.node = NodeId::from_canonical_text("node-rt");
  hop.ingress_port = PortId::from_canonical_text("in-rt");
  hop.egress_port = PortId::from_canonical_text("out-rt");
  hop.ingress_link = LinkId::from_canonical_text("il-rt");
  hop.egress_link = LinkId::from_canonical_text("el-rt");
  path.hops.push_back(hop);

  auto parsed = parse_script(path.to_string() + "\n");
  LO_REQUIRE(parsed.ok());
  LO_CHECK_EQ(parsed.value().error_count(), 0ULL);
  LO_REQUIRE(parsed.value().commands.size() == 1ULL);

  ManualClock clock{lofixture::base_time()};
  auto engine = lofixture::make_engine(clock, lofixture::default_config("script-rt"));
  const ScriptRunResult run = apply_script(*engine, parsed.value());
  LO_CHECK(run.ok());
  auto stored = engine->topology().find_path(path.id);
  LO_REQUIRE(stored.ok());
  LO_REQUIRE(stored.value().hops.size() == 1ULL);
  LO_CHECK(stored.value().hops[0].id == hop.id);
  LO_CHECK(stored.value().hops[0].node == hop.node);
  LO_CHECK(stored.value().hops[0].ingress_link == hop.ingress_link);
  LO_CHECK(stored.value().revision == path.revision);
}

LO_TEST(script, applying_a_scenario_feeds_the_classifier) {
  ManualClock clock{lofixture::base_time()};
  auto engine = lofixture::make_engine(clock, lofixture::default_config("script-apply"));
  const ScriptRunResult run = run_script_ok(*engine, kScenario);
  LO_CHECK_EQ(run.failed, 0ULL);
  LO_CHECK_EQ(run.applied, 8ULL);

  bool saw_classification = false;
  for (const ScriptStepResult& step : run.steps) {
    if (step.verb == "op" && step.output.find("class=confirmed-loss") != std::string::npos) {
      saw_classification = true;
    }
  }
  LO_CHECK(saw_classification);
}

LO_TEST(script, rejected_evidence_is_reported_as_a_failed_step) {
  ManualClock clock{lofixture::base_time()};
  auto engine = lofixture::make_engine(clock, lofixture::default_config("script-reject"));
  const std::string text =
      "source id=s1 name=a kind=counter-telemetry authority=primary\n"
      "incarnation source=s1 epoch=E1 name=boot at=2026-01-01T00:00:00Z active=true\n"
      "path id=p1 kind=synthetic hops=n1:0->n2:0\n"
      "flow id=f1 src=n1:0 dst=n2:0 proto=tcp path=p1 gen=7\n"
      "counter source=s1 epoch=missing-epoch gen=7 counter=c1 scope=dropped-packets value=1 bits=32 class=flow subject=flow:f1\n";
  auto program = parse_script(text);
  LO_REQUIRE(program.ok());
  const ScriptRunResult run = apply_script(*engine, program.value());
  LO_CHECK(!run.ok());
  LO_CHECK_EQ(run.failed, 1ULL);
  LO_CHECK(run.steps.back().detail.find("stale-epoch") != std::string::npos);
}

LO_TEST(script, a_cancelled_token_stops_the_run) {
  ManualClock clock{lofixture::base_time()};
  auto engine = lofixture::make_engine(clock, lofixture::default_config("script-cancel"));
  auto program = parse_script(kScenario);
  LO_REQUIRE(program.ok());
  std::stop_source source{};
  source.request_stop();
  const ScriptRunResult run = apply_script(*engine, program.value(), source.get_token());
  LO_CHECK(run.cancelled);
  LO_CHECK_EQ(run.applied, 0ULL);
}

LO_TEST(script, export_of_evidence_re_parses_and_reingests_identically) {
  ManualClock clock{lofixture::base_time()};
  auto first = lofixture::make_engine(clock, lofixture::default_config("script-export-a"));
  run_script_ok(*first, kScenario);

  ExportRequest request{};
  request.scope = ExportScope::All;
  request.format = ExportFormat::ScriptText;
  auto bundle = first->export_bundle(request);
  LO_REQUIRE(bundle.ok());

  auto reparsed = parse_script(bundle.value().text);
  LO_REQUIRE(reparsed.ok());
  LO_CHECK_EQ(reparsed.value().error_count(), 0ULL);

  auto second = lofixture::make_engine(clock, lofixture::default_config("script-export-b"));
  const ScriptRunResult run = apply_script(*second, reparsed.value());
  LO_CHECK(run.ok());

  LO_CHECK_EQ(second->topology().path_count(), first->topology().path_count());
  LO_CHECK_EQ(second->topology().flow_count(), first->topology().flow_count());
  LO_CHECK_EQ(second->evidence().item_count(), first->evidence().item_count());

  const FlowId flow = FlowId::from_canonical_text("f1");
  auto a = first->classify_flow(flow, clock.now());
  auto b = second->classify_flow(flow, clock.now());
  LO_REQUIRE(a.ok());
  LO_REQUIRE(b.ok());
  LO_CHECK(a.value().klass == b.value().klass);
  LO_CHECK_EQ(a.value().lost_total, b.value().lost_total);
  LO_CHECK_EQ(a.value().summary, b.value().summary);
}

LO_TEST(script, unknown_operations_fail_without_stopping_the_run) {
  ManualClock clock{lofixture::base_time()};
  auto engine = lofixture::make_engine(clock, lofixture::default_config("script-badop"));
  auto program = parse_script("op teleport flow=f1\n");
  LO_REQUIRE(program.ok());
  const ScriptRunResult run = apply_script(*engine, program.value());
  LO_CHECK(!run.ok());
  LO_CHECK_EQ(run.failed, 1ULL);
  LO_CHECK(run.steps[0].detail.find("unknown operation") != std::string::npos);
}

LO_TEST(script, rendered_run_text_is_stable) {
  ManualClock clock{lofixture::base_time()};
  auto engine = lofixture::make_engine(clock, lofixture::default_config("script-render"));
  const ScriptRunResult first = run_script_ok(*engine, kScenario);
  auto engine2 = lofixture::make_engine(clock, lofixture::default_config("script-render"));
  const ScriptRunResult second = run_script_ok(*engine2, kScenario);
  LO_CHECK_EQ(first.render(), second.render());
}
