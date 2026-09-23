// End-to-end tests that drive the real locctl executable as an independent
// operating-system process.
//
// Handoff between processes happens through files, not through a shared
// library: the only thing the two processes have in common is the on-disk
// journal and the canonical line format. Nothing here is simulated.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "fixtures.hpp"

#ifndef LO_LOCCTL_PATH
#error "LO_LOCCTL_PATH must be defined by the build system"
#endif

using namespace loss_observatory;

namespace {

std::string fresh_directory(std::string_view name) {
  const std::filesystem::path base =
      std::filesystem::temp_directory_path() / ("loss-observatory-e2e-" + std::string(name));
  std::error_code error;
  std::filesystem::remove_all(base, error);
  std::filesystem::create_directories(base, error);
  return base.string();
}

void write_text(const std::string& path, const std::string& text) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file.good()) {
    throw lotest::Failure{"cannot write " + path};
  }
  file << text;
}

std::string read_text(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file.good()) {
    throw lotest::Failure{"cannot read " + path};
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

int run_cli(const std::string& arguments) {
  // cmd.exe strips the outer quotes when a command begins with one; wrapping
  // the whole line keeps a program path containing spaces intact.
  const std::string command =
      "cmd /c \"\"" + std::string(LO_LOCCTL_PATH) + "\" " + arguments + "\"";
  return std::system(command.c_str());
}

std::string path_in(const std::string& directory, const std::string& name) {
  return (std::filesystem::path(directory) / name).string();
}

const char* kDeclarations =
    "source id=s1 name=leaf-a kind=counter-telemetry authority=primary\n"
    "incarnation source=s1 epoch=E1 name=boot-1 at=2026-01-01T00:00:00Z active=true\n"
    "path id=p1 kind=synthetic hops=n1:0->n2:0,n2:0->n3:0\n"
    "queue id=q1 node=n2 port=0 priority=3 kind=egress-port-queue\n"
    "flow id=f1 src=n1:0 dst=n3:0 proto=tcp path=p1 gen=7\n"
    "counter source=s1 epoch=E1 gen=7 seq=1 counter=c1 scope=dropped-packets value=100 bits=32 "
    "class=flow subject=flow:f1\n"
    "counter source=s1 epoch=E1 gen=7 seq=2 counter=c1 scope=dropped-packets value=175 bits=32 "
    "class=flow subject=flow:f1\n";

}  // namespace

LO_TEST(e2e, independent_process_export_and_reingest) {
  const std::string directory = fresh_directory("round-trip");
  const std::string store = path_in(directory, "store");
  const std::string scenario = path_in(directory, "scenario.txt");
  const std::string classify_out = path_in(directory, "classify.txt");
  const std::string export_out = path_in(directory, "evidence.txt");

  write_text(scenario, std::string(kDeclarations) + "op classify flow=f1 at=+1s out=" + classify_out +
                           "\n" + "op export scope=evidence format=script-text out=" + export_out +
                           "\n");

  const int first = run_cli("run --store \"" + store + "\" --instance e2e --script \"" + scenario +
                            "\" --now 2026-01-01T00:00:00Z --max-age 3600 --quiet");
  LO_CHECK_EQ(first, 0);

  const std::string classification = read_text(classify_out);
  LO_CHECK(classification.find("class=confirmed-loss") != std::string::npos);
  LO_CHECK(classification.find("lost=75") != std::string::npos);

  const std::string exported = read_text(export_out);
  LO_CHECK(exported.find("counter method=counter-delta") != std::string::npos);
  LO_CHECK(exported.find("subject=flow:") != std::string::npos);

  // Second process: nothing in memory, everything from the journal.
  const std::string after_out = path_in(directory, "after-restart.txt");
  const int second = run_cli("classify --store \"" + store + "\" --instance e2e --flow f1 --now " +
                             "2026-01-01T00:00:10Z --max-age 3600 --quiet --out \"" + after_out + "\"");
  LO_CHECK_EQ(second, 0);
  const std::string after = read_text(after_out);
  LO_CHECK(after.find("class=stale-evidence") != std::string::npos);
  LO_CHECK(after.find("confirmed-loss") == std::string::npos);

  // Third and fourth processes: export the whole store, import it into a fresh
  // store, and classify there. The conclusion must survive the round trip.
  const std::string full_export = path_in(directory, "full.txt");
  const int third = run_cli("export --store \"" + store + "\" --instance e2e --scope all --format " +
                            "script-text --quiet --out \"" + full_export + "\"");
  LO_CHECK_EQ(third, 0);

  // Loading a store retires the incarnations it recovers, so the export states
  // the previous source incarnation as inactive. Re-importing therefore takes
  // the explicit activation step an operator would take, and the runtime never
  // decides on its own that an old epoch is current again.
  const std::string reimport_out = path_in(directory, "reimported.txt");
  // The activation must precede the evidence that cites the epoch, so it is
  // spliced in directly after the source declaration.
  const std::string activation =
      "incarnation source=s1 epoch=E1 name=reimported at=2026-01-01T00:00:00Z active=true\n";
  std::string full_with_op;
  {
    const std::string exported_lines = read_text(full_export);
    std::size_t cursor = 0;
    bool inserted = false;
    while (cursor < exported_lines.size()) {
      const std::size_t end = exported_lines.find('\n', cursor);
      const std::string line =
          exported_lines.substr(cursor, end == std::string::npos ? std::string::npos : end - cursor);
      full_with_op += line + "\n";
      if (!inserted && line.rfind("source ", 0) == 0) {
        full_with_op += activation;
        inserted = true;
      }
      if (end == std::string::npos) {
        break;
      }
      cursor = end + 1;
    }
    LO_REQUIRE(inserted);
  }
  full_with_op += "op classify flow=f1 at=+1s out=" + reimport_out + "\n";
  const std::string reimport_script = path_in(directory, "reimport.txt");
  write_text(reimport_script, full_with_op);

  const std::string store_two = path_in(directory, "store-two");
  const std::string reimport_log = path_in(directory, "reimport.log");
  const int fourth =
      run_cli("run --store \"" + store_two + "\" --instance e2e2 --script \"" + reimport_script +
              "\" --now 2026-01-01T00:00:00Z --max-age 3600 --quiet > \"" + reimport_log + "\" 2>&1");
  if (fourth != 0) {
    LO_FAIL("re-import failed with " + std::to_string(fourth) + "\n" + read_text(reimport_log));
  }
  LO_CHECK_EQ(fourth, 0);
  const std::string reimported = read_text(reimport_out);
  LO_CHECK(reimported.find("class=confirmed-loss") != std::string::npos);
  LO_CHECK(reimported.find("lost=75") != std::string::npos);

  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

LO_TEST(e2e, version_reports_the_boundary_and_the_absence_of_telemetry) {
  const std::string directory = fresh_directory("version");
  const std::string out = path_in(directory, "version.txt");
  const int result = run_cli("version > \"" + out + "\"");
  LO_CHECK_EQ(result, 0);
  const std::string text = read_text(out);
  LO_CHECK(text.find("Loss Observatory") != std::string::npos);
  LO_CHECK(text.find("no transmission") != std::string::npos);
  LO_CHECK(text.find("not-owned") != std::string::npos);
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

LO_TEST(e2e, a_failing_scenario_exits_non_zero) {
  const std::string directory = fresh_directory("failure");
  const std::string store = path_in(directory, "store");
  const std::string scenario = path_in(directory, "bad.txt");
  write_text(scenario, "source id=s1 name=a kind=synthetic authority=primary\nop teleport flow=f1\n");
  const int result = run_cli("run --store \"" + store + "\" --instance e2e --script \"" + scenario +
                             "\" --now 2026-01-01T00:00:00Z --quiet");
  LO_CHECK_NE(result, 0);
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

LO_TEST(e2e, a_missing_store_argument_is_a_usage_error) {
  const int result = run_cli("classify --flow f1");
  LO_CHECK_EQ(result, 2);
}

LO_TEST(e2e, boolean_flags_do_not_consume_the_next_argument) {
  const std::string directory = fresh_directory("flags");
  const std::string store = path_in(directory, "store");
  const std::string scenario = path_in(directory, "scenario.txt");
  write_text(scenario, std::string(kDeclarations) + "op classify flow=f1 at=+1s\n");
  const int result = run_cli("run --store \"" + store + "\" --instance e2e --script \"" + scenario +
                             "\" --now 2026-01-01T00:00:00Z --max-age 3600 --quiet --synced-clock");
  LO_CHECK_EQ(result, 0);
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}
