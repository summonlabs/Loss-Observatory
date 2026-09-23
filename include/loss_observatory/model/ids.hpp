#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "loss_observatory/core/hash.hpp"
#include "loss_observatory/core/status.hpp"

namespace loss_observatory {

// ---------------------------------------------------------------------------
// Tag types. Each tag makes Id<Tag> a distinct, non-interchangeable type.
// ---------------------------------------------------------------------------
struct NodeTag;
struct PortTag;
struct PriorityTag;
struct ProtocolTag;
struct LinkTag;
struct HopTag;
struct PathTag;
struct QueueTag;
struct FlowTag;
struct SourceTag;
struct EpochTag;
struct GenerationTag;
struct RevisionTag;
struct MeasurementTag;
struct EpisodeTag;
struct ClaimTag;
struct WindowTag;
struct ProbeTag;
struct CounterTag;

using NodeId = Id<NodeTag>;
using PortId = Id<PortTag>;
using PriorityId = Id<PriorityTag>;
using ProtocolId = Id<ProtocolTag>;
using LinkId = Id<LinkTag>;
using HopId = Id<HopTag>;
using PathId = Id<PathTag>;
using QueueId = Id<QueueTag>;
using FlowId = Id<FlowTag>;
using SourceId = Id<SourceTag>;
using EpochId = Id<EpochTag>;
using GenerationId = Id<GenerationTag>;
using RevisionId = Id<RevisionTag>;
using MeasurementId = Id<MeasurementTag>;
using EpisodeId = Id<EpisodeTag>;
using ClaimId = Id<ClaimTag>;
using WindowId = Id<WindowTag>;
using ProbeId = Id<ProbeTag>;
using CounterId = Id<CounterTag>;

/// Monotonic per-source emission sequence. Ordering within a source epoch is
/// meaningful; ordering across epochs is not, which is why the epoch is part of
/// the fencing key rather than a global sequence.
class SequenceId {
 public:
  constexpr SequenceId() noexcept = default;
  [[nodiscard]] static constexpr SequenceId from_value(std::uint64_t value) noexcept { return SequenceId{value}; }

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return value_ == 0; }
  [[nodiscard]] std::string to_string() const { return hex_u64(value_); }

  friend constexpr bool operator==(SequenceId lhs, SequenceId rhs) noexcept { return lhs.value_ == rhs.value_; }
  friend constexpr bool operator!=(SequenceId lhs, SequenceId rhs) noexcept { return lhs.value_ != rhs.value_; }
  friend constexpr bool operator<(SequenceId lhs, SequenceId rhs) noexcept { return lhs.value_ < rhs.value_; }
  friend constexpr bool operator>(SequenceId lhs, SequenceId rhs) noexcept { return lhs.value_ > rhs.value_; }
  friend constexpr bool operator<=(SequenceId lhs, SequenceId rhs) noexcept { return lhs.value_ <= rhs.value_; }
  friend constexpr bool operator>=(SequenceId lhs, SequenceId rhs) noexcept { return lhs.value_ >= rhs.value_; }

 private:
  explicit constexpr SequenceId(std::uint64_t value) noexcept : value_(value) {}
  std::uint64_t value_{0};
};

/// Entity classes that a classification can be about. The value ordering is
/// part of the persisted format and must not be reordered.
enum class SubjectKind : std::uint8_t {
  Unknown = 0,
  Flow = 1,
  Path = 2,
  Hop = 3,
  Link = 4,
  Queue = 5,
  Source = 6,
};

[[nodiscard]] std::string_view to_string(SubjectKind kind) noexcept;
[[nodiscard]] Result<SubjectKind> parse_subject_kind(std::string_view text);

/// A type-erased reference to the entity under observation, retaining the
/// identity class so a link can never be silently interpreted as a queue.
class SubjectRef {
 public:
  constexpr SubjectRef() noexcept = default;

  [[nodiscard]] static constexpr SubjectRef flow(FlowId id) noexcept {
    return SubjectRef{SubjectKind::Flow, id.value()};
  }
  [[nodiscard]] static constexpr SubjectRef path(PathId id) noexcept {
    return SubjectRef{SubjectKind::Path, id.value()};
  }
  [[nodiscard]] static constexpr SubjectRef hop(HopId id) noexcept {
    return SubjectRef{SubjectKind::Hop, id.value()};
  }
  [[nodiscard]] static constexpr SubjectRef link(LinkId id) noexcept {
    return SubjectRef{SubjectKind::Link, id.value()};
  }
  [[nodiscard]] static constexpr SubjectRef queue(QueueId id) noexcept {
    return SubjectRef{SubjectKind::Queue, id.value()};
  }
  [[nodiscard]] static constexpr SubjectRef source(SourceId id) noexcept {
    return SubjectRef{SubjectKind::Source, id.value()};
  }
  [[nodiscard]] static constexpr SubjectRef unknown() noexcept { return SubjectRef{}; }

  [[nodiscard]] constexpr SubjectKind kind() const noexcept { return kind_; }
  [[nodiscard]] constexpr std::uint64_t raw_id() const noexcept { return id_; }
  [[nodiscard]] constexpr bool is_unknown() const noexcept { return kind_ == SubjectKind::Unknown; }

  [[nodiscard]] Result<FlowId> as_flow() const;
  [[nodiscard]] Result<PathId> as_path() const;
  [[nodiscard]] Result<HopId> as_hop() const;
  [[nodiscard]] Result<LinkId> as_link() const;
  [[nodiscard]] Result<QueueId> as_queue() const;
  [[nodiscard]] Result<SourceId> as_source() const;

  /// "flow:0123456789abcdef"
  [[nodiscard]] std::string to_string() const;

  friend constexpr bool operator==(SubjectRef lhs, SubjectRef rhs) noexcept {
    return lhs.kind_ == rhs.kind_ && lhs.id_ == rhs.id_;
  }
  friend constexpr bool operator!=(SubjectRef lhs, SubjectRef rhs) noexcept { return !(lhs == rhs); }
  friend constexpr bool operator<(SubjectRef lhs, SubjectRef rhs) noexcept {
    if (lhs.kind_ != rhs.kind_) {
      return static_cast<std::uint8_t>(lhs.kind_) < static_cast<std::uint8_t>(rhs.kind_);
    }
    return lhs.id_ < rhs.id_;
  }

 private:
  constexpr SubjectRef(SubjectKind kind, std::uint64_t id) noexcept : kind_(kind), id_(id) {}

  SubjectKind kind_{SubjectKind::Unknown};
  std::uint64_t id_{0};
};

/// Canonical identity derivation: numeric text is parsed as hexadecimal,
/// anything else is hashed. Used only where a human writes identities by hand;
/// the engine never invents an identity from free text on its own.
[[nodiscard]] Result<std::uint64_t> canonical_identity(std::string_view text);

}  // namespace loss_observatory
