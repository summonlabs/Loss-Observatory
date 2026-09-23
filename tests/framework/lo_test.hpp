#pragma once

// Minimal deterministic test framework.
//
// Deliberate properties:
//   * no external dependency, so the suite builds offline;
//   * no timeouts anywhere: a test either terminates or the process is killed
//     from outside, and the suite never treats "slow" as "failed";
//   * cases run in (suite, name) order, so a run is reproducible byte for byte;
//   * failures are reported with file, line, and both operand renderings.

#include <concepts>
#include <functional>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace lotest {

struct TestCase {
  std::string suite{};
  std::string name{};
  std::function<void()> body{};
};

[[nodiscard]] std::vector<TestCase>& registry();

class Registration {
 public:
  Registration(std::string suite, std::string name, std::function<void()> body);
};

/// Thrown to abort a single case. Caught by the runner.
struct Failure {
  std::string message{};
};

[[noreturn]] void fail_at(const char* file, int line, std::string message);

struct RunOptions {
  std::string filter{};
  bool list_only{false};
  bool verbose{false};
};

struct RunSummary {
  std::size_t total{0};
  std::size_t passed{0};
  std::size_t failed{0};
  std::vector<std::string> failures{};
};

[[nodiscard]] RunSummary run_all(const RunOptions& options);

template <class T>
concept Streamable = requires(std::ostream& stream, const T& value) { stream << value; };

template <class T>
[[nodiscard]] std::string render(const T& value) {
  if constexpr (Streamable<T>) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
  } else if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<long long>(value));
  } else {
    return "<value>";
  }
}

[[nodiscard]] inline std::string render(bool value) { return value ? "true" : "false"; }
[[nodiscard]] inline std::string render(std::string_view value) { return std::string(value); }

}  // namespace lotest

#define LO_TEST(suite_name, case_name)                                                        \
  static void suite_name##_##case_name##_body();                                              \
  static const ::lotest::Registration suite_name##_##case_name##_registration(                \
      #suite_name, #case_name, suite_name##_##case_name##_body);                              \
  static void suite_name##_##case_name##_body()

#define LO_FAIL(message) ::lotest::fail_at(__FILE__, __LINE__, (message))

#define LO_CHECK(condition)                                                                   \
  do {                                                                                        \
    if (!(condition)) {                                                                       \
      ::lotest::fail_at(__FILE__, __LINE__, "expected: " #condition);                         \
    }                                                                                         \
  } while (false)

#define LO_CHECK_EQ(actual, expected)                                                         \
  do {                                                                                        \
    /* Copied, not bound by reference: an operand may be a reference into a */                 \
    /* temporary, and the temporary would not outlive the comparison.       */                 \
    const auto lo_actual = (actual);                                                          \
    const auto lo_expected = (expected);                                                      \
    if (!(lo_actual == lo_expected)) {                                                        \
      ::lotest::fail_at(__FILE__, __LINE__,                                                   \
                        std::string("expected ") + #actual + " == " + #expected +             \
                            "\n    actual:   " + ::lotest::render(lo_actual) +                \
                            "\n    expected: " + ::lotest::render(lo_expected));              \
    }                                                                                         \
  } while (false)

#define LO_CHECK_NE(actual, other)                                                            \
  do {                                                                                        \
    const auto lo_actual = (actual);                                                          \
    const auto lo_other = (other);                                                            \
    if (lo_actual == lo_other) {                                                              \
      ::lotest::fail_at(__FILE__, __LINE__,                                                   \
                        std::string("expected ") + #actual + " != " + #other);                \
    }                                                                                         \
  } while (false)

/// Fails the case immediately if the precondition does not hold; used where a
/// later step would otherwise be meaningless.
#define LO_REQUIRE(condition)                                                                 \
  do {                                                                                        \
    if (!(condition)) {                                                                       \
      ::lotest::fail_at(__FILE__, __LINE__, "required: " #condition);                         \
    }                                                                                         \
  } while (false)
