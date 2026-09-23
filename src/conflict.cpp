#include "loss_observatory/conflict.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>

namespace loss_observatory {
namespace {

[[nodiscard]] bool loss_positive(LossClass klass) noexcept { return asserts_loss(klass); }
[[nodiscard]] bool loss_negative(LossClass klass) noexcept { return asserts_absence(klass); }

}  // namespace

std::string_view to_string(ConflictBasis basis) noexcept {
  switch (basis) {
    case ConflictBasis::None:
      return "none";
    case ConflictBasis::RatioDivergence:
      return "ratio-divergence";
    case ConflictBasis::ClassDivergence:
      return "class-divergence";
    case ConflictBasis::EpochDivergence:
      return "epoch-divergence";
    case ConflictBasis::MethodDivergence:
      return "method-divergence";
  }
  return "none";
}

bool ConflictDetector::incompatible(const SourceClaim& lhs, const SourceClaim& rhs,
                                    std::uint32_t tolerance_bp) {
  if (lhs.freshness != Freshness::Fresh || rhs.freshness != Freshness::Fresh) {
    return false;
  }
  const bool lhs_positive = loss_positive(lhs.klass);
  const bool rhs_positive = loss_positive(rhs.klass);
  const bool lhs_negative = loss_negative(lhs.klass);
  const bool rhs_negative = loss_negative(rhs.klass);
  if ((lhs_positive && rhs_negative) || (lhs_negative && rhs_positive)) {
    return true;
  }
  if (lhs.ratio_defined && rhs.ratio_defined) {
    const std::uint32_t low = std::min(lhs.ratio_bp, rhs.ratio_bp);
    const std::uint32_t high = std::max(lhs.ratio_bp, rhs.ratio_bp);
    if (high - low > tolerance_bp) {
      return true;
    }
  }
  return false;
}

ConflictResolution ConflictDetector::evaluate(std::span<const SourceClaim> claims) const {
  ConflictResolution result{};
  std::vector<SourceClaim> fresh;
  fresh.reserve(claims.size());
  for (const SourceClaim& claim : claims) {
    if (claim.freshness == Freshness::Fresh) {
      fresh.push_back(claim);
    }
  }
  std::sort(fresh.begin(), fresh.end(), [](const SourceClaim& lhs, const SourceClaim& rhs) {
    if (lhs.authority != rhs.authority) {
      return static_cast<std::uint8_t>(lhs.authority) > static_cast<std::uint8_t>(rhs.authority);
    }
    return lhs.source < rhs.source;
  });
  result.claims = fresh;

  // Distinct sources only: two epochs of the same source are not independent
  // witnesses, and comparing them would manufacture disagreement.
  std::vector<SourceClaim> distinct;
  for (const SourceClaim& claim : fresh) {
    const bool already = std::any_of(distinct.begin(), distinct.end(), [&claim](const SourceClaim& other) {
      return other.source == claim.source;
    });
    if (!already) {
      distinct.push_back(claim);
    }
  }
  if (distinct.size() < 2) {
    return result;
  }

  bool class_divergence = false;
  bool ratio_divergence = false;
  std::uint32_t lowest = 10000;
  std::uint32_t highest = 0;
  bool any_ratio = false;
  for (std::size_t i = 0; i < distinct.size(); ++i) {
    for (std::size_t j = i + 1; j < distinct.size(); ++j) {
      if (!incompatible(distinct[i], distinct[j], policy_.conflict_tolerance_bp)) {
        continue;
      }
      const bool class_pair = (loss_positive(distinct[i].klass) && loss_negative(distinct[j].klass)) ||
                              (loss_negative(distinct[i].klass) && loss_positive(distinct[j].klass));
      if (class_pair) {
        class_divergence = true;
      } else {
        ratio_divergence = true;
      }
    }
    if (distinct[i].ratio_defined) {
      any_ratio = true;
      lowest = std::min(lowest, distinct[i].ratio_bp);
      highest = std::max(highest, distinct[i].ratio_bp);
    }
  }
  if (!class_divergence && !ratio_divergence) {
    return result;
  }

  result.conflicted = true;
  result.basis = class_divergence ? ConflictBasis::ClassDivergence : ConflictBasis::RatioDivergence;
  result.ratio_spread_bp = any_ratio ? highest - lowest : 0;

  const SourceAuthority top = distinct.front().authority;
  std::size_t top_count = 0;
  for (const SourceClaim& claim : distinct) {
    if (claim.authority == top) {
      ++top_count;
    }
  }
  if (top_count == 1 && top != SourceAuthority::None) {
    result.resolved = true;
    result.authoritative = distinct.front().source;
    for (std::size_t i = 1; i < distinct.size(); ++i) {
      result.retained_lower_authority.push_back(distinct[i]);
    }
    result.rationale = "resolved by authority: source " + distinct.front().source.to_string() +
                       " outranks the disagreeing sources; their claims are retained";
  } else {
    result.resolved = false;
    result.rationale = top_count > 1
                           ? "unresolved: the most authoritative sources are tied"
                           : "unresolved: no source carries authority to break the disagreement";
  }
  return result;
}

std::string ConflictResolution::to_string() const {
  std::string result("conflict=");
  result.append(conflicted ? "true" : "false");
  result.append(" basis=");
  result.append(loss_observatory::to_string(basis));
  result.append(" resolved=");
  result.append(resolved ? "true" : "false");
  result.append(" spread_bp=");
  result.append(std::to_string(ratio_spread_bp));
  if (resolved) {
    result.append(" authoritative=");
    result.append(authoritative.to_string());
  }
  result.append(" claims=");
  result.append(std::to_string(claims.size()));
  result.append(" retained_lower_authority=");
  result.append(std::to_string(retained_lower_authority.size()));
  if (!rationale.empty()) {
    result.append(" rationale=");
    result.append(rationale);
  }
  return result;
}

}  // namespace loss_observatory
