// Export the declared store in the canonical line format and read it back with
// the same parser the runtime uses, proving the format round-trips.

#include <iostream>
#include <string>

#include "loss_observatory/engine.hpp"
#include "loss_observatory/report.hpp"
#include "loss_observatory/script.hpp"

int main() {
  using namespace loss_observatory;

  ManualClock clock{Timestamp::from_unix_seconds(1767225600)};
  EngineConfig config{};
  config.instance_name = "example-export";

  ObservatoryEngine engine(config, clock);
  (void)engine.start();

  const std::string scenario =
      "source id=s1 name=leaf-a kind=counter-telemetry authority=primary\n"
      "incarnation source=s1 epoch=E1 name=boot-1 at=2026-01-01T00:00:00Z active=true\n"
      "path id=p1 kind=synthetic hops=n1:0->n2:0,n2:0->n3:0\n"
      "flow id=f1 src=n1:0 dst=n3:0 proto=tcp path=p1 gen=7\n"
      "queue id=q1 node=n2 port=0 priority=3 kind=egress-port-queue\n"
      "counter source=s1 epoch=E1 gen=7 seq=1 counter=c1 scope=dropped-packets value=10 bits=32 queue=q1 observed=2026-01-01T00:00:00Z\n"
      "counter source=s1 epoch=E1 gen=7 seq=2 counter=c1 scope=dropped-packets value=25 bits=32 queue=q1 observed=2026-01-01T00:00:10Z\n";
  auto program = parse_script(scenario);
  if (!program.ok()) {
    std::cerr << "scenario did not parse\n";
    return 1;
  }
  (void)apply_script(engine, program.value());

  ExportRequest request{};
  request.scope = ExportScope::Evidence;
  request.format = ExportFormat::ScriptText;
  auto bundle = engine.export_bundle(request);
  if (!bundle.ok()) {
    std::cerr << "export failed\n";
    return 1;
  }
  std::cout << bundle.value().text;

  auto reparsed = parse_script(bundle.value().text);
  if (!reparsed.ok()) {
    std::cerr << "exported text did not re-parse: " << reparsed.status().to_string() << "\n";
    return 1;
  }
  std::cout << "re-parsed commands: " << reparsed.value().commands.size() << "\n";
  std::cout << "diagnostics      : " << reparsed.value().error_count() << "\n";
  (void)engine.stop();
  return reparsed.value().commands.empty() ? 1 : 0;
}
