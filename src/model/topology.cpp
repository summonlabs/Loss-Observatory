#include "loss_observatory/model/topology.hpp"

#include <algorithm>
#include <shared_mutex>
#include <string>

namespace loss_observatory {
namespace {

[[nodiscard]] std::string endpoint_text(const Endpoint& endpoint) {
  std::string result = endpoint.node.to_string();
  result.push_back(':');
  result.append(endpoint.port.to_string());
  return result;
}

}  // namespace

std::string_view to_string(LinkKind kind) noexcept {
  switch (kind) {
    case LinkKind::Unknown:
      return "unknown";
    case LinkKind::PhysicalPort:
      return "physical-port";
    case LinkKind::LogicalInterconnect:
      return "logical-interconnect";
    case LinkKind::VirtualLink:
      return "virtual-link";
    case LinkKind::LagMember:
      return "lag-member";
    case LinkKind::InternalFabric:
      return "internal-fabric";
  }
  return "unknown";
}

std::string_view to_string(PathKind kind) noexcept {
  switch (kind) {
    case PathKind::Unknown:
      return "unknown";
    case PathKind::ForwardingDerived:
      return "forwarding-derived";
    case PathKind::TraceRouteDerived:
      return "traceroute-derived";
    case PathKind::Configured:
      return "configured";
    case PathKind::Synthetic:
      return "synthetic";
  }
  return "unknown";
}

std::string_view to_string(QueueKind kind) noexcept {
  switch (kind) {
    case QueueKind::Unknown:
      return "unknown";
    case QueueKind::EgressPortQueue:
      return "egress-port-queue";
    case QueueKind::IngressPortQueue:
      return "ingress-port-queue";
    case QueueKind::VirtualOutputQueue:
      return "virtual-output-queue";
    case QueueKind::PriorityGroup:
      return "priority-group";
  }
  return "unknown";
}

Result<LinkKind> parse_link_kind(std::string_view text) {
  if (text == "unknown") {
    return LinkKind::Unknown;
  }
  if (text == "physical-port" || text == "physical") {
    return LinkKind::PhysicalPort;
  }
  if (text == "logical-interconnect" || text == "logical") {
    return LinkKind::LogicalInterconnect;
  }
  if (text == "virtual-link" || text == "virtual") {
    return LinkKind::VirtualLink;
  }
  if (text == "lag-member" || text == "lag") {
    return LinkKind::LagMember;
  }
  if (text == "internal-fabric" || text == "internal") {
    return LinkKind::InternalFabric;
  }
  return make_status(StatusCode::InvalidArgument, "unknown link kind");
}

Result<PathKind> parse_path_kind(std::string_view text) {
  if (text == "unknown") {
    return PathKind::Unknown;
  }
  if (text == "forwarding-derived" || text == "forwarding") {
    return PathKind::ForwardingDerived;
  }
  if (text == "traceroute-derived" || text == "traceroute") {
    return PathKind::TraceRouteDerived;
  }
  if (text == "configured") {
    return PathKind::Configured;
  }
  if (text == "synthetic") {
    return PathKind::Synthetic;
  }
  return make_status(StatusCode::InvalidArgument, "unknown path kind");
}

Result<QueueKind> parse_queue_kind(std::string_view text) {
  if (text == "unknown") {
    return QueueKind::Unknown;
  }
  if (text == "egress-port-queue" || text == "egress") {
    return QueueKind::EgressPortQueue;
  }
  if (text == "ingress-port-queue" || text == "ingress") {
    return QueueKind::IngressPortQueue;
  }
  if (text == "virtual-output-queue" || text == "voq") {
    return QueueKind::VirtualOutputQueue;
  }
  if (text == "priority-group" || text == "pg") {
    return QueueKind::PriorityGroup;
  }
  return make_status(StatusCode::InvalidArgument, "unknown queue kind");
}

std::string Endpoint::to_string() const { return endpoint_text(*this); }

std::string Link::to_string() const {
  std::string result = "link id=";
  result.append(id.to_string());
  result.append(" a=");
  result.append(endpoint_text(a));
  result.append(" b=");
  result.append(endpoint_text(b));
  result.append(" kind=");
  result.append(loss_observatory::to_string(kind));
  return result;
}

std::string Hop::to_string() const {
  std::string result = "hop id=";
  result.append(id.to_string());
  result.append(" index=");
  result.append(std::to_string(index));
  result.append(" node=");
  result.append(node.to_string());
  result.append(" in=");
  result.append(ingress_port.to_string());
  result.append(" out=");
  result.append(egress_port.to_string());
  result.append(" inlink=");
  result.append(ingress_link.to_string());
  result.append(" outlink=");
  result.append(egress_link.to_string());
  return result;
}

bool operator==(const Path& lhs, const Path& rhs) {
  return lhs.id == rhs.id && lhs.kind == rhs.kind && lhs.revision == rhs.revision && lhs.hops == rhs.hops;
}

std::string Path::to_string() const {
  // Emitted in the exact form the script reader accepts, with hop identities
  // spelled out, so declared topology survives an export/import round trip
  // without hop identities being silently re-derived.
  std::string result = "path id=";
  result.append(id.to_string());
  result.append(" kind=");
  result.append(loss_observatory::to_string(kind));
  result.append(" rev=");
  result.append(revision.to_string());
  result.append(" hops=");
  for (std::size_t i = 0; i < hops.size(); ++i) {
    if (i != 0) {
      result.push_back(',');
    }
    result.append(hops[i].id.to_string());
    result.push_back(':');
    result.append(hops[i].node.to_string());
    result.push_back(':');
    result.append(hops[i].ingress_port.to_string());
    result.push_back(':');
    result.append(hops[i].egress_port.to_string());
    result.push_back(':');
    result.append(hops[i].ingress_link.to_string());
    result.push_back(':');
    result.append(hops[i].egress_link.to_string());
  }
  return result;
}

std::string QueueEntity::to_string() const {
  std::string result = "queue id=";
  result.append(id.to_string());
  result.append(" node=");
  result.append(node.to_string());
  result.append(" port=");
  result.append(port.to_string());
  result.append(" priority=");
  result.append(priority.to_string());
  result.append(" kind=");
  result.append(loss_observatory::to_string(kind));
  return result;
}

std::string FlowBinding::to_string() const {
  std::string result = "flow id=";
  result.append(id.to_string());
  result.append(" src=");
  result.append(endpoint_text(source));
  result.append(" dst=");
  result.append(endpoint_text(destination));
  result.append(" proto=");
  result.append(protocol.to_string());
  result.append(" path=");
  result.append(path.to_string());
  result.append(" gen=");
  result.append(generation.to_string());
  result.append(" rev=");
  result.append(binding_revision.to_string());
  return result;
}

UpsertOutcome TopologyRegistry::classify(std::size_t existing_count, bool present, bool equal,
                                         bool allow_replace) const {
  (void)existing_count;
  if (!present) {
    return UpsertOutcome::Inserted;
  }
  if (equal) {
    return UpsertOutcome::Unchanged;
  }
  return allow_replace ? UpsertOutcome::Replaced : UpsertOutcome::Conflicted;
}

Result<UpsertResult> TopologyRegistry::upsert_link(Link link, bool allow_replace) {
  if (link.id.is_nil()) {
    return make_status(StatusCode::InvalidArgument, "link identity is nil");
  }
  std::unique_lock lock(mutex_);
  const auto it = links_.find(link.id);
  const bool present = it != links_.end();
  const bool equal = present && it->second == link;
  if (!present && links_.size() >= limits_.max_links) {
    return make_status(StatusCode::CapacityExceeded, "link limit reached");
  }
  const UpsertOutcome outcome = classify(links_.size(), present, equal, allow_replace);
  if (outcome == UpsertOutcome::Conflicted) {
    return make_status(StatusCode::Conflict, "link identity already declared with different content");
  }
  if (outcome == UpsertOutcome::Inserted || outcome == UpsertOutcome::Replaced) {
    register_nodes_locked({link.a.node, link.b.node});
    links_[link.id] = link;
    revision_ = RevisionId::from_value(revision_.value() + 1);
  }
  return UpsertResult{outcome, revision_};
}

void TopologyRegistry::register_nodes_locked(const std::vector<NodeId>& nodes) {
  for (const NodeId node : nodes) {
    if (node.is_nil()) {
      continue;
    }
    const auto it = nodes_.find(node);
    if (it == nodes_.end()) {
      if (nodes_.size() >= limits_.max_nodes) {
        continue;
      }
      nodes_.emplace(node, 1);
      continue;
    }
    ++it->second;
  }
}

Result<UpsertResult> TopologyRegistry::upsert_path(Path path, bool allow_replace) {
  if (path.id.is_nil()) {
    return make_status(StatusCode::InvalidArgument, "path identity is nil");
  }
  if (path.hops.size() > limits_.max_hops_per_path) {
    return make_status(StatusCode::CapacityExceeded, "path hop limit reached");
  }
  std::sort(path.hops.begin(), path.hops.end(),
            [](const Hop& lhs, const Hop& rhs) { return lhs.index < rhs.index; });
  for (std::size_t i = 1; i < path.hops.size(); ++i) {
    if (path.hops[i - 1].index == path.hops[i].index) {
      return make_status(StatusCode::InvalidArgument, "path declares the same hop index twice");
    }
  }
  for (const Hop& hop : path.hops) {
    if (hop.id.is_nil()) {
      return make_status(StatusCode::InvalidArgument, "hop identity is nil");
    }
  }

  std::unique_lock lock(mutex_);
  const auto it = paths_.find(path.id);
  const bool present = it != paths_.end();
  const bool equal = present && it->second == path;
  if (!present && paths_.size() >= limits_.max_paths) {
    return make_status(StatusCode::CapacityExceeded, "path limit reached");
  }
  const UpsertOutcome outcome = classify(paths_.size(), present, equal, allow_replace);
  if (outcome == UpsertOutcome::Conflicted) {
    return make_status(StatusCode::Conflict, "path identity already declared with different content");
  }
  if (outcome == UpsertOutcome::Inserted || outcome == UpsertOutcome::Replaced) {
    revision_ = RevisionId::from_value(revision_.value() + 1);
    // A path that does not carry an explicit revision gets the revision it was
    // created at, so evidence can be tied to the path shape it observed. A
    // replacement always takes the new revision even when the caller repeats
    // the old one: otherwise a changed path shape would keep validating
    // evidence recorded against a shape it no longer has.
    if (outcome == UpsertOutcome::Replaced || path.revision.is_nil()) {
      path.revision = revision_;
    }
    std::vector<NodeId> nodes;
    nodes.reserve(path.hops.size());
    for (const Hop& hop : path.hops) {
      nodes.push_back(hop.node);
    }
    register_nodes_locked(nodes);
    paths_[path.id] = std::move(path);
  }
  return UpsertResult{outcome, revision_};
}

Result<UpsertResult> TopologyRegistry::upsert_queue(QueueEntity queue, bool allow_replace) {
  if (queue.id.is_nil()) {
    return make_status(StatusCode::InvalidArgument, "queue identity is nil");
  }
  std::unique_lock lock(mutex_);
  const auto it = queues_.find(queue.id);
  const bool present = it != queues_.end();
  const bool equal = present && it->second == queue;
  if (!present && queues_.size() >= limits_.max_queues) {
    return make_status(StatusCode::CapacityExceeded, "queue limit reached");
  }
  const UpsertOutcome outcome = classify(queues_.size(), present, equal, allow_replace);
  if (outcome == UpsertOutcome::Conflicted) {
    return make_status(StatusCode::Conflict, "queue identity already declared with different content");
  }
  if (outcome == UpsertOutcome::Inserted || outcome == UpsertOutcome::Replaced) {
    register_nodes_locked({queue.node});
    queues_[queue.id] = queue;
    revision_ = RevisionId::from_value(revision_.value() + 1);
  }
  return UpsertResult{outcome, revision_};
}

Result<UpsertResult> TopologyRegistry::upsert_flow(FlowBinding flow, bool allow_replace) {
  if (flow.id.is_nil()) {
    return make_status(StatusCode::InvalidArgument, "flow identity is nil");
  }
  std::unique_lock lock(mutex_);
  const auto it = flows_.find(flow.id);
  const bool present = it != flows_.end();
  const bool equal = present && it->second == flow;
  if (!present && flows_.size() >= limits_.max_flows) {
    return make_status(StatusCode::CapacityExceeded, "flow limit reached");
  }
  const UpsertOutcome outcome = classify(flows_.size(), present, equal, allow_replace);
  if (outcome == UpsertOutcome::Conflicted) {
    return make_status(StatusCode::Conflict, "flow identity already declared with different content");
  }
  if (outcome == UpsertOutcome::Inserted || outcome == UpsertOutcome::Replaced) {
    register_nodes_locked({flow.source.node, flow.destination.node});
    flows_[flow.id] = flow;
    revision_ = RevisionId::from_value(revision_.value() + 1);
  }
  return UpsertResult{outcome, revision_};
}

Result<Link> TopologyRegistry::find_link(LinkId id) const {
  std::shared_lock lock(mutex_);
  const auto it = links_.find(id);
  if (it == links_.end()) {
    return make_status(StatusCode::NotFound, "link is not declared");
  }
  return it->second;
}

Result<Path> TopologyRegistry::find_path(PathId id) const {
  std::shared_lock lock(mutex_);
  const auto it = paths_.find(id);
  if (it == paths_.end()) {
    return make_status(StatusCode::NotFound, "path is not declared");
  }
  return it->second;
}

Result<QueueEntity> TopologyRegistry::find_queue(QueueId id) const {
  std::shared_lock lock(mutex_);
  const auto it = queues_.find(id);
  if (it == queues_.end()) {
    return make_status(StatusCode::NotFound, "queue is not declared");
  }
  return it->second;
}

Result<FlowBinding> TopologyRegistry::find_flow(FlowId id) const {
  std::shared_lock lock(mutex_);
  const auto it = flows_.find(id);
  if (it == flows_.end()) {
    return make_status(StatusCode::NotFound, "flow is not declared");
  }
  return it->second;
}

Result<std::vector<Hop>> TopologyRegistry::hops_of(PathId path) const {
  std::shared_lock lock(mutex_);
  const auto it = paths_.find(path);
  if (it == paths_.end()) {
    return make_status(StatusCode::NotFound, "path is not declared");
  }
  return it->second.hops;
}

Result<Hop> TopologyRegistry::hop_at(PathId path, std::uint32_t index) const {
  std::shared_lock lock(mutex_);
  const auto it = paths_.find(path);
  if (it == paths_.end()) {
    return make_status(StatusCode::NotFound, "path is not declared");
  }
  for (const Hop& hop : it->second.hops) {
    if (hop.index == index) {
      return hop;
    }
  }
  return make_status(StatusCode::NotFound, "path has no hop at that index");
}

std::vector<FlowBinding> TopologyRegistry::flows() const {
  std::shared_lock lock(mutex_);
  std::vector<FlowBinding> result;
  result.reserve(flows_.size());
  for (const auto& entry : flows_) {
    result.push_back(entry.second);
  }
  return result;
}

std::vector<Path> TopologyRegistry::paths() const {
  std::shared_lock lock(mutex_);
  std::vector<Path> result;
  result.reserve(paths_.size());
  for (const auto& entry : paths_) {
    result.push_back(entry.second);
  }
  return result;
}

std::vector<Link> TopologyRegistry::links() const {
  std::shared_lock lock(mutex_);
  std::vector<Link> result;
  result.reserve(links_.size());
  for (const auto& entry : links_) {
    result.push_back(entry.second);
  }
  return result;
}

std::vector<QueueEntity> TopologyRegistry::queues() const {
  std::shared_lock lock(mutex_);
  std::vector<QueueEntity> result;
  result.reserve(queues_.size());
  for (const auto& entry : queues_) {
    result.push_back(entry.second);
  }
  return result;
}

RevisionId TopologyRegistry::revision() const {
  std::shared_lock lock(mutex_);
  return revision_;
}

std::size_t TopologyRegistry::flow_count() const {
  std::shared_lock lock(mutex_);
  return flows_.size();
}

std::size_t TopologyRegistry::path_count() const {
  std::shared_lock lock(mutex_);
  return paths_.size();
}

std::size_t TopologyRegistry::link_count() const {
  std::shared_lock lock(mutex_);
  return links_.size();
}

std::size_t TopologyRegistry::queue_count() const {
  std::shared_lock lock(mutex_);
  return queues_.size();
}

std::string TopologyRegistry::canonical_dump() const {
  std::shared_lock lock(mutex_);
  std::string result;
  // Emitted as a comment: it describes the declared model rather than
  // declaring anything, and the export must re-parse as a scenario.
  result.append("# topology revision ");
  result.append(revision_.to_string());
  result.push_back('\n');
  for (const auto& entry : links_) {
    result.append(entry.second.to_string());
    result.push_back('\n');
  }
  for (const auto& entry : paths_) {
    result.append(entry.second.to_string());
    result.push_back('\n');
  }
  for (const auto& entry : queues_) {
    result.append(entry.second.to_string());
    result.push_back('\n');
  }
  for (const auto& entry : flows_) {
    result.append(entry.second.to_string());
    result.push_back('\n');
  }
  return result;
}

}  // namespace loss_observatory
