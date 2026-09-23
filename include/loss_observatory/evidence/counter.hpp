#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "loss_observatory/core/time.hpp"
#include "loss_observatory/evidence/evidence.hpp"

namespace loss_observatory {

/// What a pair of counter readings can legitimately be turned into.
enum class CounterDeltaState : std::uint8_t {
  /// Monotonic, plausible delta.
  Valid = 0,
  /// The counter decreased and the reading is consistent with a declared width
  /// wrap. The wrapped delta is reported; this is a discontinuity, not loss.
  WrapDetected,
  /// The counter decreased and cannot be a wrap (no width declared, or the
  /// previous value was nowhere near the top of the range). Not loss.
  ResetDetected,
  /// The counter decreased, a width is declared, but the previous value is not
  /// in the wrap band. Neither wrap nor reset is proven. Not loss.
  DiscontinuityUnresolved,
  /// Second reading is older than the first by source sequence or by time.
  OutOfOrder,
  /// Source epoch changed between the readings: values are not comparable.
  EpochChanged,
  /// Generation changed between the readings: values are not comparable.
  GenerationChanged,
  /// The counter identity, scope, or binding changed between the readings.
  CounterChanged,
  /// Delta exceeds the configured plausibility threshold. Treated as a
  /// discontinuity so an implausible jump can never be reported as loss.
  ImplausibleDelta,
  /// Only one reading exists: no delta can be derived. Explicitly incomplete.
  MissingBaseline,
};

[[nodiscard]] std::string_view to_string(CounterDeltaState state) noexcept;

struct CounterDelta {
  MeasurementId id{};
  MeasurementId from_id{};
  MeasurementId to_id{};
  SequenceId from_sequence{};
  SequenceId to_sequence{};
  std::uint64_t raw_previous{0};
  std::uint64_t raw_current{0};
  std::uint64_t delta{0};
  Duration elapsed{};
  std::uint8_t width_bits{0};
  CounterDeltaState state{CounterDeltaState::Valid};
  std::string detail{};

  [[nodiscard]] bool is_valid() const noexcept { return state == CounterDeltaState::Valid; }
  [[nodiscard]] bool is_discontinuity() const noexcept;
  [[nodiscard]] std::string to_string() const;
};

struct CounterPolicy {
  /// A delta above this is treated as implausible and therefore discontinuous.
  std::uint64_t implausible_delta_threshold{1000000000ULL};
  /// The previous reading must be within (modulus / wrap_band_divisor) of the
  /// top of the declared range for a decrease to be accepted as a wrap.
  std::uint64_t wrap_band_divisor{8};
  bool allow_wrap{true};
  /// When true, the delta recovered across a detected wrap is accepted as
  /// ordinary loss evidence. The default is false: this runtime reports the
  /// wrap as a discontinuity and requires a fresh baseline, because a
  /// discontinuously-sampled counter is exactly the situation in which a
  /// plausible-looking number is least trustworthy. The policy is explicit so
  /// a deployment can choose the other behaviour deliberately.
  bool accept_wrapped_delta{false};
  /// Upper bound on the number of counter series derived in one pass.
  std::size_t max_series{4096};
  /// Upper bound on points consumed per series.
  std::size_t max_points_per_series{4096};
};

/// 2^bits as a u64; 0 is the sentinel for a 64-bit counter whose modulus does
/// not fit in u64.
[[nodiscard]] constexpr std::uint64_t counter_modulus(std::uint8_t width_bits) noexcept {
  if (width_bits == 0 || width_bits >= 64) {
    return 0;
  }
  return 1ULL << width_bits;
}

[[nodiscard]] constexpr bool counter_width_supported(std::uint8_t width_bits) noexcept {
  return width_bits == 16 || width_bits == 32 || width_bits == 64;
}

/// Derives deltas from consecutive readings of the same counter.
///
/// p series must already be in canonical order (source sequence ascending,
/// then measurement id). Readings are never reordered silently: an out-of-order
/// pair produces OutOfOrder rather than a fabricated delta.
[[nodiscard]] std::vector<CounterDelta> derive_counter_deltas(std::span<const EvidenceItem> series,
                                                              const CounterPolicy& policy);

/// Groups items that belong to the same counter series (source, epoch, counter
/// identity, scope, binding) and returns each group in canonical order.
[[nodiscard]] std::vector<std::vector<EvidenceItem>> group_counter_series(std::span<const EvidenceItem> items,
                                                                          std::size_t max_series,
                                                                          BoundNotes& bounds);

/// True when two samples describe the same counter binding: same counter
/// identity, same scope, and same entity binding. A change in any of these
/// makes a delta meaningless and is reported as a discontinuity.
[[nodiscard]] bool counter_binding_differs(const CounterSample& lhs, const CounterSample& rhs) noexcept;

/// True when a counter scope reports losses directly rather than throughput.
[[nodiscard]] constexpr bool scope_reports_loss_directly(CounterScope scope) noexcept {
  return scope == CounterScope::DroppedPackets || scope == CounterScope::DiscardedPackets ||
         scope == CounterScope::ErrorPackets;
}

}  // namespace loss_observatory
