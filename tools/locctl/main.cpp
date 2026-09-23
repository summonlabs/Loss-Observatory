// locctl -- inspection and scripting tool for a Loss Observatory store.
//
// The tool is deliberately thin: every operation it performs is expressed as a
// command in the same line language that the runtime parses, so the CLI cannot
// drift away from the library semantics.

#include <chrono>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "loss_observatory/engine.hpp"
#include "loss_observatory/report.hpp"
#include "loss_observatory/script.hpp"
#include "loss_observatory/version.hpp"

namespace {

using namespace loss_observatory;

constexpr int kExitOk = 0;
constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;

struct Options {
  std::string command{};
  std::string store{};
  std::string instance{"default"};
  std::string script{};
  std::string out{};
  std::string flow{};
  std::string at{};
  std::string now{};
  std::string scope{};
  std::string format{};
  std::string gran{};
  std::string max_age{};
  std::string max_items{};
  std::string lines{};
  bool synced_clock{false};
  bool quiet{false};
};

void print_usage() {
  std::cout << kProductName << " locctl " << version_string() << "\n\n"
            << "usage: locctl <command> [options]\n\n"
            << "commands:\n"
            << "  version                                  print version and proof-surface notes\n"
            << "  run        --store DIR [--script FILE]   apply a scenario, then persist\n"
            << "  classify   --store DIR --flow ID         classify a flow from stored evidence\n"
            << "  localize   --store DIR --flow ID         localize loss for a flow\n"
            << "  history    --store DIR [--flow ID]       list recorded loss episodes\n"
            << "  explain    --store DIR --flow ID         deterministic explanation\n"
            << "  export     --store DIR --out FILE        export the store in a canonical format\n"
            << "\noptions:\n"
            << "  --instance NAME     store instance name (default: default)\n"
            << "  --now TIME          freeze the engine clock at an ISO-8601 instant\n"
            << "  --at TIME           evaluation instant (ISO-8601 or +Nms/-Ns)\n"
            << "  --max-age SECONDS   freshness window for evidence (default: 30)\n"
            << "  --scope SCOPE       topology|sources|evidence|episodes|aggregates|all\n"
            << "  --format FORMAT     script-text|report|binary\n"
            << "  --gran GRANULARITY  unknown|flow|path|hop|link|queue\n"
            << "  --max N             maximum items in the result\n"
            << "  --lines N           maximum explanation lines\n"
            << "  --synced-clock      declare a synchronised clock domain\n"
            << "  --quiet             suppress the recovery banner\n"
            << "  --script -          read the scenario from standard input\n";
}

[[nodiscard]] bool parse_options(int argc, char** argv, Options& options, std::string& error) {
  if (argc < 2) {
    error = "missing command";
    return false;
  }
  options.command = argv[1];
  for (int i = 2; i < argc; ++i) {
    std::string argument = argv[i];
    std::string value;
    const std::size_t equals = argument.find('=');
    if (equals != std::string::npos) {
      value = argument.substr(equals + 1);
      argument = argument.substr(0, equals);
    } else if (argument == "--quiet" || argument == "--synced-clock") {
      value = "true";
    } else if (argument.rfind("--", 0) == 0) {
      if (i + 1 >= argc) {
        error = "option requires a value: " + argument;
        return false;
      }
      value = argv[++i];
    } else {
      error = "unexpected argument: " + argument;
      return false;
    }
    if (argument == "--store") {
      options.store = value;
    } else if (argument == "--instance") {
      options.instance = value;
    } else if (argument == "--script") {
      options.script = value;
    } else if (argument == "--out") {
      options.out = value;
    } else if (argument == "--flow") {
      options.flow = value;
    } else if (argument == "--at") {
      options.at = value;
    } else if (argument == "--now") {
      options.now = value;
    } else if (argument == "--scope") {
      options.scope = value;
    } else if (argument == "--format") {
      options.format = value;
    } else if (argument == "--gran") {
      options.gran = value;
    } else if (argument == "--max-age") {
      options.max_age = value;
    } else if (argument == "--max") {
      options.max_items = value;
    } else if (argument == "--lines") {
      options.lines = value;
    } else if (argument == "--synced-clock") {
      options.synced_clock = value == "true" || value == "1" || value.empty();
    } else if (argument == "--quiet") {
      options.quiet = value == "true" || value == "1" || value.empty();
    } else if (argument == "--help" || argument == "-h") {
      print_usage();
      std::exit(kExitOk);
    } else {
      error = "unknown option: " + argument;
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::string read_stream(std::istream& stream) {
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

struct Session {
  std::unique_ptr<SystemClock> system{};
  std::unique_ptr<ManualClock> manual{};
  std::unique_ptr<ObservatoryEngine> engine{};
  RecoveryReport recovery{};
  std::string clock_instant{};
};

[[nodiscard]] bool open_session(const Options& options, Session& session, std::string& error) {
  if (options.store.empty()) {
    error = "--store is required";
    return false;
  }
  EngineConfig config{};
  config.instance_name = options.instance;
  config.worker_count = 0;
  config.persist.enabled = true;
  config.persist.directory = options.store;
  config.persist.instance = options.instance;
  config.clock_synchronized = options.synced_clock;
  if (!options.max_age.empty()) {
    auto seconds = parse_i64(options.max_age);
    if (!seconds.ok()) {
      error = "invalid --max-age: " + seconds.status().message();
      return false;
    }
    config.loss.freshness.max_age = Duration::from_seconds(seconds.value());
  }

  Timestamp start{};
  if (!options.now.empty()) {
    auto parsed = Timestamp::parse_iso8601(options.now);
    if (!parsed.ok()) {
      error = "invalid --now: " + parsed.status().message();
      return false;
    }
    start = parsed.value();
    session.clock_instant = options.now;
    session.manual = std::make_unique<ManualClock>(start);
  } else {
    session.system = std::make_unique<SystemClock>();
    start = session.system->now();
  }

  const Clock& clock = session.manual ? static_cast<const Clock&>(*session.manual)
                                      : static_cast<const Clock&>(*session.system);
  session.engine = std::make_unique<ObservatoryEngine>(std::move(config), clock);
  const Result<void> started = session.engine->start();
  if (!started.ok()) {
    error = "engine start failed: " + started.status().to_string();
    return false;
  }
  const Result<RecoveryReport> recovery = session.engine->load();
  if (!recovery.ok()) {
    error = "recovery failed: " + recovery.status().to_string();
    return false;
  }
  session.recovery = recovery.value();
  return true;
}

[[nodiscard]] std::string build_operation(const Options& options) {
  std::string line = "op " + options.command;
  if (!options.flow.empty()) {
    line += " flow=" + options.flow;
  }
  if (!options.at.empty()) {
    line += " at=" + options.at;
  }
  if (!options.gran.empty()) {
    line += " gran=" + options.gran;
  }
  if (!options.scope.empty()) {
    line += " scope=" + options.scope;
  }
  if (!options.format.empty()) {
    line += " format=" + options.format;
  }
  if (!options.max_items.empty()) {
    line += " max=" + options.max_items;
  }
  if (!options.lines.empty()) {
    line += " lines=" + options.lines;
  }
  if (!options.out.empty()) {
    line += " out=" + options.out;
  }
  return line;
}

[[nodiscard]] std::string render_failures(const ScriptRunResult& run) {
  std::string report;
  for (const ScriptStepResult& step : run.steps) {
    if (step.code == StatusCode::Ok) {
      continue;
    }
    report += "line " + std::to_string(step.line) + " " + step.verb + " -> " +
              std::string(to_string(step.code));
    if (!step.detail.empty()) {
      report += " (" + step.detail + ")";
    }
    report += "\n";
  }
  for (const ScriptDiagnostic& diagnostic : run.diagnostics) {
    if (diagnostic.severity == "error") {
      report += diagnostic.to_string() + "\n";
    }
  }
  return report;
}

[[nodiscard]] int run_program(ObservatoryEngine& engine, const std::string& text, bool quiet,
                              std::string& rendered) {
  auto program = parse_script(text);
  if (!program.ok()) {
    std::cerr << "scenario did not parse: " << program.status().to_string() << "\n";
    return kExitFailure;
  }
  const ScriptRunResult run = apply_script(engine, program.value());
  rendered = run.render();
  if (!quiet) {
    std::cout << rendered;
    if (rendered.empty() || rendered.back() != '\n') {
      std::cout << "\n";
    }
  } else if (!run.ok()) {
    // Quiet suppresses progress, not problems.
    std::cerr << render_failures(run);
  }
  return run.ok() ? kExitOk : kExitFailure;
}

}  // namespace

int main(int argc, char** argv) {
  Options options{};
  std::string error;
  if (!parse_options(argc, argv, options, error)) {
    std::cerr << "locctl: " << error << "\n";
    print_usage();
    return kExitUsage;
  }
  if (options.command == "version") {
    std::cout << full_version_string() << "\n";
    std::cout << "scope: observation, attribution, localization, history, persistence\n";
    std::cout << "not-owned: path repair, rerouting, congestion control, blackhole declaration\n";
    std::cout << "telemetry: no transmission; this tool only reads and writes local files\n";
    return kExitOk;
  }
  if (options.command == "help" || options.command == "--help") {
    print_usage();
    return kExitOk;
  }
  if (options.store.empty()) {
    std::cerr << "locctl: --store is required for " << options.command << "\n";
    print_usage();
    return kExitUsage;
  }

  Session session{};
  if (!open_session(options, session, error)) {
    std::cerr << "locctl: " << error << "\n";
    return kExitFailure;
  }
  if (!options.quiet) {
    std::cout << "recovery " << session.recovery.to_string() << "\n";
    const EngineEpoch epoch = session.engine->epoch();
    std::cout << "session epoch=" << epoch.id.to_string() << " booted=" << epoch.booted_at.to_string()
              << "\n";
  }

  std::string text;
  if (options.command == "run") {
    if (options.script.empty()) {
      std::cerr << "locctl: run requires --script FILE (or --script -)\n";
      return kExitUsage;
    }
    if (options.script == "-") {
      text = read_stream(std::cin);
    } else {
      std::ifstream file(options.script, std::ios::binary);
      if (!file.good()) {
        std::cerr << "locctl: cannot read scenario file: " << options.script << "\n";
        return kExitFailure;
      }
      text = read_stream(file);
    }
  } else if (options.command == "classify" || options.command == "localize" ||
             options.command == "history" || options.command == "explain" ||
             options.command == "export") {
    text = build_operation(options);
  } else {
    std::cerr << "locctl: unknown command: " << options.command << "\n";
    print_usage();
    return kExitUsage;
  }

  std::string rendered;
  const int result = run_program(*session.engine, text, options.quiet, rendered);
  const Result<void> persisted = session.engine->persist();
  if (!persisted.ok()) {
    std::cerr << "locctl: persist failed: " << persisted.status().to_string() << "\n";
  }
  const Result<void> stopped = session.engine->stop();
  if (!stopped.ok()) {
    std::cerr << "locctl: engine stop reported a problem: " << stopped.status().to_string() << "\n";
    return kExitFailure;
  }
  if (!options.quiet && options.out.empty()) {
    std::cout << "stats " << session.engine->stats().store.accepted << " accepted, "
              << session.engine->stats().store.rejected_malformed << " malformed, "
              << session.engine->stats().store.rejected_replay << " replayed\n";
  }
  return result;
}
