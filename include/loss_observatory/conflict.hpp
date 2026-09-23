#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "loss_observatory/classify.hpp"

namespace loss_observatory {

enum class ConflictBasis : std::uint8_t {
  None = 0,
  /// Sources agree that loss happened but disagree on magnitude.
  RatioDivergence,
  /// Sources disagree about whether loss happened at all.
  ClassDivergence,
  /// Sources report for different source epochs of the same logical source.
  EpochDivergence,
  /// Sources used methods with incompatible capability.
  MethodDivergence,
};

[[nodiscard]] std::string_view to_string(ConflictBasis basis) noexcept;

struct ConflictResolution {
  bool conflicted{false};
  ConflictBasis basis{ConflictBasis::None};
  /// Deterministically ordered: (authority descending, source id ascending).
  /// Conflicting claims are retained even when a winner is chosen: the runtime
  /// never erases the losing claim.
  std::vector<SourceClaim> claims{};
  std::vector<SourceClaim> retained_lower_authority{};
  bool resolved{false};
  SourceId authoritative{};
  std::uint32_t ratio_spread_bp{0};
  std::string rationale{};

  [[nodiscard]] std::string to_string() const;
};

/// Detects and resolves disagreement between admissible sources.
///
/// Resolution only ever happens through declared authority. When no source
/// strictly outranks the others, the result stays unresolved and the caller
/// must report ConflictingEvidence rather than pick a favourite.
class ConflictDetector {
 public:
  explicit ConflictDetector(LossPolicy policy) : policy_(policy) {}

  [[nodiscard]] ConflictResolution evaluate(std::span<const SourceClaim> claims) const;

  /// True when the two claims are incompatible under p tolerance_bp.
  [[nodiscard]] static bool incompatible(const SourceClaim& lhs, const SourceClaim& rhs,
                                         std::uint32_t tolerance_bp);

  void set_policy(LossPolicy policy) noexcept { policy_ = policy; }
  [[nodiscard]] const LossPolicy& policy() const noexcept { return policy_; }

 private:
  LossPolicy policy_{};
};

}  // namespace loss_observatory
