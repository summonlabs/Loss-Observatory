// Independent downstream consumer of the installed Loss Observatory package.
//
// It links only against the exported target and uses only public headers, so a
// successful build and run demonstrates that the package is genuinely
// installable and consumable. The scenario it drives is SYNTHETIC.

#include <cstdio>
#include <string>

#include "loss_observatory/engine.hpp"
#include "loss_observatory/report.hpp"
#include "loss_observatory/script.hpp"
#include "loss_observatory/version.hpp"

namespace {

const char* kScenario =
    "source id=downstream name=consumer kind=counter-telemetry authority=primary\n"
    "incarnation source=downstream epoch=E1 name=boot at=2026-01-01T00:00:00Z active=true\n"
    "path id=p1 kind=synthetic hops=n1:0->n2:0,n2:0->n3:0\n"
    "flow id=f1 src=n1:0 dst=n3:0 proto=tcp path=p1 gen=7\n"
    "counter source=downstream epoch=E1 gen=7 seq=1 counter=c1 scope=dropped-packets value=10 "
    "bits=32 class=flow subject=flow:f1\n"
    "counter source=downstream epoch=E1 gen=7 seq=2 counter=c1 scope=dropped-packets value=45 "
    "bits=32 class=flow subject=flow:f1\n";

}  // namespace

int main() {
  using namespace loss_observatory;

  std::printf("%s\n", full_version_string().c_str());

  ManualClock clock{Timestamp::from_unix_seconds(1767225600)};
  EngineConfig config{};
  config.instance_name = "downstream";
  config.loss.freshness.max_age = Duration::from_seconds(3600);

  ObservatoryEngine engine(config, clock);
  const Result<void> started = engine.start();
  if (!started.ok()) {
    std::printf("start failed: %s\n", started.status().to_string().c_str());
    return 1;
  }

  auto program = parse_script(kScenario);
  if (!program.ok() || !program.value().ok()) {
    std::printf("scenario did not parse\n");
    return 1;
  }
  const ScriptRunResult run = apply_script(engine, program.value());
  if (!run.ok()) {
    std::printf("scenario failed:\n%s\n", run.render().c_str());
    return 1;
  }

  auto classification = engine.classify_flow(FlowId::from_canonical_text("f1"), clock.now());
  if (!classification.ok()) {
    std::printf("classify failed\n");
    return 1;
  }
  std::printf("classification: %s\n", classification.value().to_string().c_str());
  const bool confirmed = classification.value().klass == LossClass::ConfirmedLoss;

  LocalizationRequest request{};
  request.flow = FlowId::from_canonical_text("f1");
  request.at = clock.now();
  request.requested_max_granularity = Granularity::Queue;
  auto localization = engine.localize(request);
  if (!localization.ok()) {
    std::printf("localize failed\n");
    return 1;
  }
  std::printf("localization: %s\n", localization.value().to_string().c_str());

  (void)engine.stop();
  std::printf("downstream consumer result: %s\n", confirmed ? "confirmed-loss" : "unexpected");
  return confirmed ? 0 : 1;
}
