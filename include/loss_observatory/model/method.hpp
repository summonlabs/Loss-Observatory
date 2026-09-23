#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "loss_observatory/model/granularity.hpp"

namespace loss_observatory {

/// How a loss number was obtained. The method is not decoration: it determines
/// what the number can and cannot prove, so it travels with every observation
/// and is restated in every explanation and export.
enum class MeasurementMethod : std::uint8_t {
  Unknown = 0,
  /// Difference of two counter samples of the same counter.
  CounterDelta = 1,
  /// Active round-trip probe; loss is inferred from unanswered probes.
  ProbeRoundTrip = 2,
  /// One-way probe; requires a synchronised clock domain to interpret.
  ProbeOneWay = 3,
  /// Gap analysis over a delivered sequence space.
  SequenceGap = 4,
  /// What one endpoint says it sent versus what the other says it received.
  EndpointComparison = 5,
  /// Deterministic generator in this repository. Synthetic by construction.
  SyntheticInjection = 6,
};

[[nodiscard]] std::string_view to_string(MeasurementMethod method) noexcept;
[[nodiscard]] Result<MeasurementMethod> parse_measurement_method(std::string_view text);

/// Declared capability of a method. This is static, reviewable data, not a
/// runtime guess, so "this method cannot support that claim" is decidable
/// before any evidence is examined.
struct MethodSemantics {
  MeasurementMethod method{MeasurementMethod::Unknown};
  std::string_view name{};
  /// Finest granularity this method can ever justify.
  Granularity max_granularity{Granularity::Unknown};
  /// True when the method needs a synchronised clock domain to mean anything.
  bool requires_synchronized_clocks{false};
  /// True when the method is only meaningful while the generation is stable.
  bool requires_stable_generation{true};
  /// True when the method models counter wrap explicitly.
  bool understands_counter_wrap{false};
  /// True when the method can distinguish direction of loss.
  bool directional{false};
  /// True for generators and replay fixtures in this repository.
  bool synthetic{false};
  /// True when this runtime implements the method end to end. An unimplemented
  /// method never silently degrades to another method; it produces
  /// LossClass::UnsupportedMethod.
  bool implemented{false};
  std::string_view description{};
};

[[nodiscard]] const MethodSemantics& semantics_of(MeasurementMethod method) noexcept;

/// True when the method is implemented and usable without extra capabilities.
[[nodiscard]] bool method_is_usable(MeasurementMethod method, bool clock_synchronized) noexcept;

/// Why a method is unusable, or empty when it is usable. Deterministic text.
[[nodiscard]] std::string_view method_unusable_reason(MeasurementMethod method,
                                                      bool clock_synchronized) noexcept;

[[nodiscard]] std::vector<MeasurementMethod> all_measurement_methods();

}  // namespace loss_observatory
