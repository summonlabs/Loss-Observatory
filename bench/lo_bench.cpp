// Benchmarks that measure completed work.
//
// Every phase runs a fixed number of complete operations and reports the count
// it actually finished, so a partial run can never be mistaken for a fast one.
// All inputs are SYNTHETIC: this benchmark makes no hardware or fabric claim.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "loss_observatory/engine.hpp"
#include "loss_observatory/report.hpp"
#include "loss_observatory/script.hpp"
#include "loss_observatory/version.hpp"

namespace {

using namespace loss_observatory;
using SteadyClock = std::chrono::steady_clock;

struct Measurement {
  std::string phase{};
  std::size_t operations{0};
  double total_ms{0.0};
};

std::vector<Measurement> g_measurements{};

void report(const std::string& phase, std::size_t operations, double total_ms) {
  Measurement measurement{};
  measurement.phase = phase;
  measurement.operations = operations;
  measurement.total_ms = total_ms;
  g_measurements.push_back(measurement);
  const double per_operation_us = operations == 0 ? 0.0 : (total_ms * 1000.0) / static_cast<double>(operations);
  std::printf("%-28s ops=%-8zu total=%9.3f ms  per-op=%9.3f us  throughput=%12.1f op/s\n",
              phase.c_str(), operations, total_ms, per_operation_us,
              total_ms <= 0.0 ? 0.0 : (static_cast<double>(operations) * 1000.0) / total_ms);
}

[[nodiscard]] std::string scenario_text(std::size_t samples, std::uint64_t drop_step) {
  std::string text;
  text += "source id=s1 name=bench kind=counter-telemetry authority=primary\n";
  text += "incarnation source=s1 epoch=E1 name=boot-1 at=2026-01-01T00:00:00Z active=true\n";
  text += "path id=p1 kind=synthetic hops=n1:0->n2:0,n2:0->n3:0,n3:0->n4:0\n";
  text += "flow id=f1 src=n1:0 dst=n4:0 proto=tcp path=p1 gen=7\n";
  text += "queue id=q1 node=n2 port=0 priority=3 kind=egress-port-queue\n";
  std::uint64_t value = 1000;
  for (std::size_t index = 0; index < samples; ++index) {
    // Observation instants default to the engine clock, so the generated
    // scenario carries no hidden wall-clock dependency.
    text += "counter source=s1 epoch=E1 gen=7 seq=" + std::to_string(index + 1) +
            " counter=c1 scope=dropped-packets value=" + std::to_string(value) +
            " bits=64 queue=q1\n";
    value += drop_step;
  }
  return text;
}

}  // namespace

int main(int argc, char** argv) {
  std::size_t samples = 2000;
  std::size_t classifications = 200;
  std::size_t localizations = 200;
  std::size_t round_trips = 20;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--quick") {
      samples = 500;
      classifications = 50;
      localizations = 50;
      round_trips = 5;
    }
  }

  std::cout << full_version_string() << "\n";
  std::cout << "proof surface: SYNTHETIC (generated inputs, no hardware involved)\n\n";

  // ---- ingest ------------------------------------------------------------
  {
    std::size_t completed = 0;
    double elapsed = 0.0;
    for (std::size_t repetition = 0; repetition < 3; ++repetition) {
      ManualClock clock{Timestamp::from_unix_seconds(1767225600)};
      EngineConfig config{};
      config.instance_name = "bench";
      config.loss.freshness.max_age = Duration::from_seconds(100000);
      ObservatoryEngine engine(config, clock);
      (void)engine.start();
      auto program = parse_script(scenario_text(samples, 3));
      if (!program.ok()) {
        std::cerr << "benchmark scenario did not parse\n";
        return 1;
      }
      const auto begin = SteadyClock::now();
      auto run = apply_script(engine, program.value());
      const auto end = SteadyClock::now();
      completed += run.applied;
      elapsed += std::chrono::duration<double, std::milli>(end - begin).count();
      (void)engine.stop();
    }
    report("ingest+script", completed, elapsed);
  }

  // ---- classify / localize / explain over a fixed store -------------------
  {
    ManualClock clock{Timestamp::from_unix_seconds(1767225600)};
    EngineConfig config{};
    config.instance_name = "bench";
    config.loss.freshness.max_age = Duration::from_seconds(100000);
    ObservatoryEngine engine(config, clock);
    (void)engine.start();
    auto program = parse_script(scenario_text(samples, 3));
    (void)apply_script(engine, program.value());
    const FlowId flow = FlowId::from_canonical_text("f1");
    const Timestamp at = clock.now() + Duration::from_seconds(1);

    std::size_t completed = 0;
    const auto classify_begin = SteadyClock::now();
    for (std::size_t i = 0; i < classifications; ++i) {
      auto result = engine.classify_flow(flow, at);
      if (result.ok()) {
        ++completed;
      }
    }
    const auto classify_end = SteadyClock::now();
    report("classify", completed,
           std::chrono::duration<double, std::milli>(classify_end - classify_begin).count());

    completed = 0;
    const auto localize_begin = SteadyClock::now();
    for (std::size_t i = 0; i < localizations; ++i) {
      LocalizationRequest request{};
      request.flow = flow;
      request.at = at;
      auto result = engine.localize(request);
      if (result.ok()) {
        ++completed;
      }
    }
    const auto localize_end = SteadyClock::now();
    report("localize", completed,
           std::chrono::duration<double, std::milli>(localize_end - localize_begin).count());

    completed = 0;
    const auto explain_begin = SteadyClock::now();
    for (std::size_t i = 0; i < localizations; ++i) {
      ExplainRequest request{};
      request.subject = SubjectRef::flow(flow);
      request.at = at;
      auto result = engine.explain(request);
      if (result.ok()) {
        ++completed;
      }
    }
    const auto explain_end = SteadyClock::now();
    report("explain", completed,
           std::chrono::duration<double, std::milli>(explain_end - explain_begin).count());
    (void)engine.stop();
  }

  // ---- persistence round trip -------------------------------------------
  {
    std::size_t completed = 0;
    double elapsed = 0.0;
    for (std::size_t repetition = 0; repetition < round_trips; ++repetition) {
      const std::string directory =
          "lo_bench_store_" + std::to_string(repetition);
      std::error_code error;
      std::filesystem::remove_all(directory, error);
      ManualClock clock{Timestamp::from_unix_seconds(1767225600)};
      EngineConfig config{};
      config.instance_name = "bench";
      config.persist.enabled = true;
      config.persist.directory = directory;
      config.persist.instance = "bench";
      config.loss.freshness.max_age = Duration::from_seconds(100000);
      ObservatoryEngine engine(config, clock);
      (void)engine.start();
      auto program = parse_script(scenario_text(samples / 4, 3));
      (void)apply_script(engine, program.value());
      const auto begin = SteadyClock::now();
      (void)engine.persist();
      (void)engine.stop();

      ObservatoryEngine reopened(config, clock);
      (void)reopened.start();
      auto recovered = reopened.load();
      const auto end = SteadyClock::now();
      if (recovered.ok()) {
        ++completed;
      }
      (void)reopened.stop();
      elapsed += std::chrono::duration<double, std::milli>(end - begin).count();
      std::filesystem::remove_all(directory, error);
    }
    report("persist+recover", completed, elapsed);
  }

  std::cout << "\ncompleted phases: " << g_measurements.size() << "\n";
  return 0;
}
