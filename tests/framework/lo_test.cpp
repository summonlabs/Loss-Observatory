#include "lo_test.hpp"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <iostream>
#include <string>

namespace lotest {

std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

Registration::Registration(std::string suite, std::string name, std::function<void()> body) {
  registry().push_back(TestCase{std::move(suite), std::move(name), std::move(body)});
}

void fail_at(const char* file, int line, std::string message) {
  throw Failure{std::string(file) + ":" + std::to_string(line) + ": " + std::move(message)};
}

RunSummary run_all(const RunOptions& options) {
  std::vector<TestCase> cases = registry();
  std::sort(cases.begin(), cases.end(), [](const TestCase& lhs, const TestCase& rhs) {
    if (lhs.suite != rhs.suite) {
      return lhs.suite < rhs.suite;
    }
    return lhs.name < rhs.name;
  });

  RunSummary summary{};
  if (options.list_only) {
    for (const TestCase& test : cases) {
      std::cout << test.suite << "." << test.name << "\n";
    }
    return summary;
  }

  for (const TestCase& test : cases) {
    const std::string qualified = test.suite + "." + test.name;
    if (!options.filter.empty() && qualified.find(options.filter) == std::string::npos) {
      continue;
    }
    ++summary.total;
    // Each result is flushed immediately: if a case terminates the process
    // rather than failing, the run up to that point is still readable.
    try {
      test.body();
      ++summary.passed;
      std::cout << "PASS " << qualified << "\n";
      std::cout.flush();
    } catch (const Failure& failure) {
      ++summary.failed;
      summary.failures.push_back(qualified + "\n    " + failure.message);
      std::cout << "FAIL " << qualified << "\n    " << failure.message << "\n";
      std::cout.flush();
    } catch (const std::exception& error) {
      ++summary.failed;
      summary.failures.push_back(qualified + "\n    unexpected exception: " + error.what());
      std::cout << "FAIL " << qualified << "\n    unexpected exception: " << error.what() << "\n";
      std::cout.flush();
    } catch (...) {
      ++summary.failed;
      summary.failures.push_back(qualified + "\n    unexpected non-standard exception");
      std::cout << "FAIL " << qualified << "\n    unexpected non-standard exception\n";
      std::cout.flush();
    }
  }

  std::cout << "\n" << summary.passed << " passed, " << summary.failed << " failed, " << summary.total
            << " run\n";
  if (!summary.failures.empty()) {
    std::cout << "\nfailures:\n";
    for (const std::string& failure : summary.failures) {
      std::cout << "  " << failure << "\n";
    }
  }
  return summary;
}

}  // namespace lotest

int main(int argc, char** argv) {
  lotest::RunOptions options{};
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument.rfind("--filter=", 0) == 0) {
      options.filter = argument.substr(9);
    } else if (argument == "--list") {
      options.list_only = true;
    } else if (argument == "--verbose") {
      options.verbose = true;
    } else {
      std::cout << "unknown argument: " << argument << "\n";
      return 2;
    }
  }
  const lotest::RunSummary summary = lotest::run_all(options);
  if (options.list_only) {
    return 0;
  }
  return summary.failed == 0 ? 0 : 1;
}
