#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "loss_observatory/classify.hpp"
#include "loss_observatory/model/topology.hpp"

namespace loss_observatory {

struct LocalizePolicy {
  /// Maximum number of segments returned. The bound is reported when it bites.
  std::size_t max_segments{256};
  /// Maximum number of ambiguity notes retained.
  std::size_t max_ambiguity_notes{64};
  /// Maximum hops walked when grouping a path.
  std::size_t max_path_hops{256};
  /// Minimum ratio, in basis points, for a segment to be reported as a loss
  /// candidate. Below this the segment is reported as observing no loss.
  std::uint32_t min_segment_ratio_bp{1};
};

enum class SegmentKind : std::uint8_t {
  /// One declared entity covered by evidence at its own granularity.
  Observed = 0,
  /// The whole flow, when nothing finer is supported.
  FlowWide,
  /// A contiguous run of path hops with no evidence at all. Explicitly not
  /// "clean": the runtime has no idea what happened there.
  UnobservedSpan,
  /// The subject named by the request could not be resolved in the declared
  /// topology.
  Unresolved,
};

[[nodiscard]] std::string_view to_string(SegmentKind kind) noexcept;

struct LocalizedSegment {
  SegmentKind kind{SegmentKind::Observed};
  Granularity granularity{Granularity::Unknown};
  SubjectRef subject{};
  /// Hop range covered, when the segment is expressed in path positions.
  std::uint32_t first_hop{0};
  std::uint32_t last_hop{0};
  bool hop_range_known{false};
  LossClass klass{LossClass::Unknown};
  ConfidenceBand confidence{ConfidenceBand::None};
  std::uint64_t lost{0};
  std::uint64_t offered{0};
  bool ratio_defined{false};
  std::uint32_t ratio_bp{0};
  std::size_t observation_count{0};
  std::vector<SourceId> sources{};
  std::vector<MeasurementId> evidence_ids{};
  bool evidence_ids_truncated{false};
  std::vector<ReasonCode> reasons{};

  [[nodiscard]] std::string to_string() const;
};

/// Ambiguity is a first-class output. A localization that could not
/// distinguish between candidates says so and lists what remains possible.
struct AmbiguityNote {
  Granularity widest_possible{Granularity::Unknown};
  Granularity achieved{Granularity::Unknown};
  ReasonCode reason{ReasonCode::AmbiguityRetained};
  std::size_t candidate_count{0};
  std::string detail{};

  [[nodiscard]] std::string to_string() const;
};

struct LocalizationRequest {
  FlowId flow{};
  Timestamp at{};
  /// Finest granularity the caller is willing to receive. The result is never
  /// finer than this and never finer than the evidence supports.
  Granularity requested_max_granularity{Granularity::Queue};
  std::size_t max_segments{256};
};

struct LocalizationResult {
  SubjectRef subject{};
  FlowId flow{};
  PathId path{};
  Timestamp at{};
  bool path_known{false};
  LossClass aggregate_class{LossClass::Unknown};
  /// Finest granularity actually delivered.
  Granularity achieved_granularity{Granularity::Unknown};
  /// Finest granularity the evidence could support, before the caller cap.
  Granularity evidence_granularity{Granularity::Unknown};
  /// True when more than one entity remains consistent with the evidence.
  bool ambiguous{false};
  bool truncated{false};
  std::size_t hops_in_path{0};
  std::size_t hops_with_evidence{0};
  std::vector<LocalizedSegment> segments{};
  std::vector<AmbiguityNote> ambiguity{};
  std::vector<ReasonCode> reasons{};
  std::vector<BoundNote> bounds{};
  std::string summary{};

  [[nodiscard]] bool localized_finer_than_flow() const noexcept {
    return is_finer_than(achieved_granularity, Granularity::Flow);
  }
  [[nodiscard]] std::string to_string() const;
};

/// Localizes loss for a flow from normalized observations.
///
/// Hard invariant, checked at runtime and asserted by tests: the granularity of
/// every emitted segment is never finer than the granularity of the evidence
/// that produced it, and never finer than the caller's cap. A flow-scoped
/// observation can produce a flow-scoped answer and nothing more, however many
/// hops the declared path happens to contain.
class Localizer {
 public:
  Localizer(const TopologyRegistry& topology, LocalizePolicy policy = {})
      : topology_(&topology), policy_(policy) {}

  [[nodiscard]] LocalizationResult localize(const LocalizationRequest& request,
                                            std::span<const LossObservation> observations,
                                            Timestamp now, const LossPolicy& policy,
                                            const FreshnessContext& context) const;

  [[nodiscard]] const LocalizePolicy& policy() const noexcept { return policy_; }
  void set_policy(LocalizePolicy policy) noexcept { policy_ = policy; }
  void set_topology(const TopologyRegistry& topology) noexcept { topology_ = &topology; }

 private:
  const TopologyRegistry* topology_{nullptr};
  LocalizePolicy policy_{};
};

}  // namespace loss_observatory
