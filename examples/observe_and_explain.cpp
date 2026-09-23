// Observe a synthetic loss scenario end to end and print the explanation.
//
// Every number in this example comes from this repository's own deterministic
// generator: it is a SYNTHETIC proof surface. Nothing here contacts a switch,
// an ASIC, a NIC, or any external system.

#include <cstdio>
#include <iostream>

#include "loss_observatory/engine.hpp"
#include "loss_observatory/report.hpp"
#include "loss_observatory/testkit/testkit.hpp"
#include "loss_observatory/version.hpp"

int main() {
  using namespace loss_observatory;

  ManualClock clock{testkit::ScenarioConfig{}.start};
  EngineConfig config{};
  config.instance_name = "example-observe";
  config.worker_count = 0;
  config.loss.freshness.max_age = Duration::from_seconds(3600);

  ObservatoryEngine engine(config, clock);
  const Result<void> started = engine.start();
  if (!started.ok()) {
    std::cerr << "start failed: " << started.status().to_string() << "\n";
    return 1;
  }

  testkit::ScenarioConfig scenario_config{};
  scenario_config.seed = 7;
  scenario_config.hop_count = 4;
  scenario_config.sample_count = 12;
  scenario_config.loss_ratio_bp = 250;
  scenario_config.start = clock.now();

  auto scenario = testkit::make_drop_counter_scenario(engine, scenario_config);
  if (!scenario.ok()) {
    std::cerr << "scenario failed: " << scenario.status().to_string() << "\n";
    return 1;
  }

  const Timestamp evaluated_at =
      scenario_config.start + Duration::from_nanos(scenario_config.sample_interval.nanos() * 20);
  ExplainRequest request{};
  request.subject = SubjectRef::flow(scenario.value().flow);
  request.at = evaluated_at;

  auto explanation = engine.explain(request);
  if (!explanation.ok()) {
    std::cerr << "explain failed: " << explanation.status().to_string() << "\n";
    return 1;
  }
  std::cout << explanation.value().text;

  std::cout << "\nsynthetic drops injected by the generator: " << scenario.value().injected_drops << "\n";
  std::cout << "runtime reported lost total               : "
            << explanation.value().classification.lost_total << "\n";
  std::cout << "proof surface                             : SYNTHETIC\n";
  (void)engine.stop();
  return 0;
}
