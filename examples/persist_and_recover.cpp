// Persist evidence, restart the runtime, and show that recovered evidence is
// history rather than current proof.
//
// This is the executable form of the guarantee "restart does not revive old
// loss evidence as current".

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>

#include "loss_observatory/engine.hpp"
#include "loss_observatory/report.hpp"
#include "loss_observatory/script.hpp"

namespace {

const char* kScenario = R"SCENARIO(
source id=s1 name=leaf-a kind=counter-telemetry authority=primary
incarnation source=s1 epoch=E1 name=boot-1 at=2026-01-01T00:00:00Z active=true
path id=p1 kind=synthetic hops=n1:0->n2:0,n2:0->n3:0
flow id=f1 src=n1:0 dst=n3:0 proto=tcp path=p1 gen=7
counter source=s1 epoch=E1 gen=7 seq=1 counter=c1 scope=dropped-packets value=1000 bits=32 class=flow subject=flow:f1 observed=2026-01-01T00:00:00Z
counter source=s1 epoch=E1 gen=7 seq=2 counter=c1 scope=dropped-packets value=1030 bits=32 class=flow subject=flow:f1 observed=2026-01-01T00:00:10Z
op classify flow=f1 at=2026-01-01T00:00:11Z
)SCENARIO";

}  // namespace

int main() {
  using namespace loss_observatory;

  const std::string directory =
      (std::filesystem::temp_directory_path() / "loss-observatory-example-persist").string();
  std::error_code error;
  std::filesystem::remove_all(directory, error);
  std::filesystem::create_directories(directory, error);

  ManualClock clock{Timestamp::from_unix_seconds(1767225611)};

  {
    EngineConfig config{};
    config.instance_name = "example-persist";
    config.persist.enabled = true;
    config.persist.directory = directory;
    config.persist.instance = "example";
    config.loss.freshness.max_age = Duration::from_seconds(60);

    ObservatoryEngine engine(config, clock);
    (void)engine.start();
    auto program = parse_script(kScenario);
    if (!program.ok()) {
      std::cerr << "parse failed\n";
      return 1;
    }
    auto run = apply_script(engine, program.value());
    std::cout << "first process (live evidence):\n" << run.render() << "\n";
    (void)engine.persist();
    (void)engine.stop();
  }

  // Second process in the same executable: everything it knows came from disk.
  {
    EngineConfig config{};
    config.instance_name = "example-persist";
    config.persist.enabled = true;
    config.persist.directory = directory;
    config.persist.instance = "example";
    config.loss.freshness.max_age = Duration::from_seconds(60);

    ObservatoryEngine engine(config, clock);
    (void)engine.start();
    auto recovery = engine.load();
    if (!recovery.ok()) {
      std::cerr << "load failed\n";
      return 1;
    }
    std::cout << "second process recovery: " << recovery.value().to_string() << "\n";
    auto classification = engine.classify_flow(FlowId::from_canonical_text("f1"), clock.now());
    if (!classification.ok()) {
      std::cerr << "classify failed\n";
      return 1;
    }
    std::cout << "classification after restart: " << classification.value().to_string() << "\n";
    std::cout << "expected: stale-evidence -- recovered observations are never current\n";
    (void)engine.stop();
  }

  std::filesystem::remove_all(directory, error);
  return 0;
}
