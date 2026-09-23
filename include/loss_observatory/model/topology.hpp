#pragma once

#include <cstdint>
#include <map>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include "loss_observatory/core/bounded.hpp"
#include "loss_observatory/model/granularity.hpp"
#include "loss_observatory/model/ids.hpp"

namespace loss_observatory {

/// Topology is declared, never inferred. Every entity here was supplied by an
/// operator or a configuration source; the runtime never fabricates a hop, a
/// link, or a queue to make a localization look more precise than it is.

enum class LinkKind : std::uint8_t {
  Unknown = 0,
  PhysicalPort = 1,
  LogicalInterconnect = 2,
  VirtualLink = 3,
  LagMember = 4,
  InternalFabric = 5,
};

enum class PathKind : std::uint8_t {
  Unknown = 0,
  ForwardingDerived = 1,
  TraceRouteDerived = 2,
  Configured = 3,
  Synthetic = 4,
};

enum class QueueKind : std::uint8_t {
  Unknown = 0,
  EgressPortQueue = 1,
  IngressPortQueue = 2,
  VirtualOutputQueue = 3,
  PriorityGroup = 4,
};

[[nodiscard]] std::string_view to_string(LinkKind kind) noexcept;
[[nodiscard]] std::string_view to_string(PathKind kind) noexcept;
[[nodiscard]] std::string_view to_string(QueueKind kind) noexcept;
[[nodiscard]] Result<LinkKind> parse_link_kind(std::string_view text);
[[nodiscard]] Result<PathKind> parse_path_kind(std::string_view text);
[[nodiscard]] Result<QueueKind> parse_queue_kind(std::string_view text);

struct Endpoint {
  NodeId node{};
  PortId port{};

  friend bool operator==(const Endpoint& lhs, const Endpoint& rhs) {
    return lhs.node == rhs.node && lhs.port == rhs.port;
  }
  friend bool operator<(const Endpoint& lhs, const Endpoint& rhs) {
    if (lhs.node != rhs.node) {
      return lhs.node < rhs.node;
    }
    return lhs.port < rhs.port;
  }
  [[nodiscard]] std::string to_string() const;
};

struct Link {
  LinkId id{};
  Endpoint a{};
  Endpoint b{};
  LinkKind kind{LinkKind::Unknown};

  friend bool operator==(const Link& lhs, const Link& rhs) {
    return lhs.id == rhs.id && lhs.a == rhs.a && lhs.b == rhs.b && lhs.kind == rhs.kind;
  }
  [[nodiscard]] std::string to_string() const;
};

/// One forwarding step. p index is the position along the path and is the
/// ordering key everywhere, so two paths that visit the same node at different
/// positions remain distinguishable.
struct Hop {
  HopId id{};
  std::uint32_t index{0};
  NodeId node{};
  PortId ingress_port{};
  PortId egress_port{};
  LinkId ingress_link{};
  LinkId egress_link{};

  friend bool operator==(const Hop& lhs, const Hop& rhs) {
    return lhs.id == rhs.id && lhs.index == rhs.index && lhs.node == rhs.node &&
           lhs.ingress_port == rhs.ingress_port && lhs.egress_port == rhs.egress_port &&
           lhs.ingress_link == rhs.ingress_link && lhs.egress_link == rhs.egress_link;
  }
  [[nodiscard]] std::string to_string() const;
};

struct Path {
  PathId id{};
  PathKind kind{PathKind::Unknown};
  RevisionId revision{};
  std::vector<Hop> hops{};

  friend bool operator==(const Path& lhs, const Path& rhs);
  [[nodiscard]] std::string to_string() const;
};

struct QueueEntity {
  QueueId id{};
  NodeId node{};
  PortId port{};
  PriorityId priority{};
  QueueKind kind{QueueKind::Unknown};

  friend bool operator==(const QueueEntity& lhs, const QueueEntity& rhs) {
    return lhs.id == rhs.id && lhs.node == rhs.node && lhs.port == rhs.port &&
           lhs.priority == rhs.priority && lhs.kind == rhs.kind;
  }
  [[nodiscard]] std::string to_string() const;
};

struct FlowBinding {
  FlowId id{};
  Endpoint source{};
  Endpoint destination{};
  ProtocolId protocol{};
  PathId path{};
  GenerationId generation{};
  /// Topology revision in force when this binding was recorded. Evidence that
  /// cites an older revision is treated as stale rather than as current loss.
  RevisionId binding_revision{};

  friend bool operator==(const FlowBinding& lhs, const FlowBinding& rhs) {
    return lhs.id == rhs.id && lhs.source == rhs.source && lhs.destination == rhs.destination &&
           lhs.protocol == rhs.protocol && lhs.path == rhs.path && lhs.generation == rhs.generation &&
           lhs.binding_revision == rhs.binding_revision;
  }
  [[nodiscard]] std::string to_string() const;
};

struct TopologyLimits {
  std::size_t max_links{4096};
  std::size_t max_paths{1024};
  std::size_t max_hops_per_path{256};
  std::size_t max_queues{4096};
  std::size_t max_flows{4096};
  std::size_t max_nodes{2048};
};

struct UpsertResult {
  UpsertOutcome outcome{UpsertOutcome::Inserted};
  RevisionId revision{};
};

/// Declared network model. Read-mostly, thread-safe, revisioned.
///
/// The global revision increments on every accepted mutation. Entities carry
/// their own revision so that evidence can be tied to the exact shape of the
/// path or binding it observed.
class TopologyRegistry {
 public:
  explicit TopologyRegistry(TopologyLimits limits = {}) : limits_(limits) {}

  TopologyRegistry(const TopologyRegistry&) = delete;
  TopologyRegistry& operator=(const TopologyRegistry&) = delete;

  [[nodiscard]] Result<UpsertResult> upsert_link(Link link, bool allow_replace = false);
  [[nodiscard]] Result<UpsertResult> upsert_path(Path path, bool allow_replace = false);
  [[nodiscard]] Result<UpsertResult> upsert_queue(QueueEntity queue, bool allow_replace = false);
  [[nodiscard]] Result<UpsertResult> upsert_flow(FlowBinding flow, bool allow_replace = false);

  [[nodiscard]] Result<Link> find_link(LinkId id) const;
  [[nodiscard]] Result<Path> find_path(PathId id) const;
  [[nodiscard]] Result<QueueEntity> find_queue(QueueId id) const;
  [[nodiscard]] Result<FlowBinding> find_flow(FlowId id) const;

  /// Hops of p path ordered by index. Fails when the path is unknown: an
  /// unknown path is never treated as an empty path.
  [[nodiscard]] Result<std::vector<Hop>> hops_of(PathId path) const;

  /// Hop at a specific index. Absent index is NotFound, not a zero-hop.
  [[nodiscard]] Result<Hop> hop_at(PathId path, std::uint32_t index) const;

  [[nodiscard]] std::vector<FlowBinding> flows() const;
  [[nodiscard]] std::vector<Path> paths() const;
  [[nodiscard]] std::vector<Link> links() const;
  [[nodiscard]] std::vector<QueueEntity> queues() const;

  [[nodiscard]] RevisionId revision() const;
  [[nodiscard]] std::size_t flow_count() const;
  [[nodiscard]] std::size_t path_count() const;
  [[nodiscard]] std::size_t link_count() const;
  [[nodiscard]] std::size_t queue_count() const;

  [[nodiscard]] const TopologyLimits& limits() const noexcept { return limits_; }

  /// Canonical serialisation of the declared model, used by export and by the
  /// restart path. Ordered by identity so output is byte-stable.
  [[nodiscard]] std::string canonical_dump() const;

 private:
  [[nodiscard]] UpsertOutcome classify(std::size_t existing_count, bool present, bool equal,
                                       bool allow_replace) const;
  void register_nodes_locked(const std::vector<NodeId>& nodes);

  mutable std::shared_mutex mutex_{};
  TopologyLimits limits_{};
  std::map<LinkId, Link> links_{};
  std::map<PathId, Path> paths_{};
  std::map<QueueId, QueueEntity> queues_{};
  std::map<FlowId, FlowBinding> flows_{};
  std::map<NodeId, std::uint64_t> nodes_{};
  RevisionId revision_{Id<RevisionTag>::from_value(1)};
};

}  // namespace loss_observatory
