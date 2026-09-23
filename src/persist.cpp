#include "loss_observatory/persist.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "loss_observatory/core/bytes.hpp"
#include "loss_observatory/core/checked.hpp"
#include "loss_observatory/version.hpp"

namespace loss_observatory {
namespace {

constexpr std::uint32_t kRecordMagic = 0x4C4F4A31U;  // 'L','O','J','1'
constexpr std::size_t kRecordPrefixBytes = 20;
constexpr std::size_t kRecordHeaderBytes = kRecordPrefixBytes + 4;
constexpr std::uint8_t kEvidenceEncodingVersion = 1;
constexpr std::uint8_t kTopologyEncodingVersion = 1;

[[nodiscard]] bool read_identity(ByteReader& reader, std::uint64_t& value) { return reader.u64(value); }

}  // namespace

std::string_view to_string(RecordType type) noexcept {
  switch (type) {
    case RecordType::Manifest:
      return "manifest";
    case RecordType::TopologyEntity:
      return "topology-entity";
    case RecordType::SourceDescriptorRecord:
      return "source-descriptor";
    case RecordType::SourceIncarnationRecord:
      return "source-incarnation";
    case RecordType::EvidenceRecord:
      return "evidence";
    case RecordType::EpisodeRecord:
      return "episode";
    case RecordType::SessionRecord:
      return "session";
    case RecordType::CompactionMarker:
      return "compaction-marker";
  }
  return "manifest";
}

std::string RecoveryReport::to_string() const {
  std::string result = "recovery opened=";
  result.append(opened ? "true" : "false");
  result.append(" created=");
  result.append(created ? "true" : "false");
  result.append(" version_mismatch=");
  result.append(version_mismatch ? "true" : "false");
  result.append(" semantics_mismatch=");
  result.append(semantics_mismatch ? "true" : "false");
  result.append(" checksum_failure=");
  result.append(checksum_failure ? "true" : "false");
  result.append(" truncated=");
  result.append(truncated ? "true" : "false");
  result.append(" records=");
  result.append(std::to_string(records_accepted));
  result.push_back('/');
  result.append(std::to_string(records_read));
  result.append(" discarded=");
  result.append(std::to_string(records_discarded));
  result.append(" evidence_restored=");
  result.append(std::to_string(evidence_restored));
  result.append(" episodes_restored=");
  result.append(std::to_string(episodes_restored));
  if (!detail.empty()) {
    result.append(" detail=");
    result.append(detail);
  }
  return result;
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> encode_evidence(const EvidenceItem& item) {
  std::vector<std::uint8_t> buffer(4096, 0);
  ByteWriter writer{MutableByteSpan{buffer.data(), buffer.size()}};
  const EvidenceKind kind = kind_of(item.payload);

  writer.u8(kEvidenceEncodingVersion);
  writer.u64(item.header.id.value());
  writer.u64(item.header.source.value());
  writer.u64(item.header.epoch.value());
  writer.u64(item.header.generation.value());
  writer.u64(item.header.topology_revision.value());
  writer.u64(item.header.source_sequence.value());
  writer.i64(item.header.observed_at.value.unix_nanos());
  writer.i64(item.header.received_at.value.unix_nanos());
  writer.u8(static_cast<std::uint8_t>(item.header.method));
  writer.u8(static_cast<std::uint8_t>(item.header.granularity));
  writer.u8(static_cast<std::uint8_t>(item.header.subject.kind()));
  writer.u64(item.header.subject.raw_id());
  writer.text(item.header.note);
  writer.u8(item.header.recovered_from_persistence ? 1 : 0);
  writer.u8(static_cast<std::uint8_t>(kind));

  switch (kind) {
    case EvidenceKind::CounterSample: {
      const auto& sample = std::get<CounterSample>(item.payload);
      writer.u64(sample.counter.value());
      writer.u8(static_cast<std::uint8_t>(sample.scope));
      writer.u64(sample.value);
      writer.u8(sample.width_bits);
      writer.u64(sample.node.value());
      writer.u64(sample.port.value());
      writer.u64(sample.queue.value());
      writer.u64(sample.hop.value());
      writer.u64(sample.link.value());
      break;
    }
    case EvidenceKind::ProbeReport: {
      const auto& report = std::get<ProbeReport>(item.payload);
      writer.u64(report.probe.value());
      writer.u32(report.sent);
      writer.u32(report.received);
      writer.u32(report.timed_out);
      writer.u8(report.ttl_scoped ? 1 : 0);
      writer.u8(report.ttl);
      writer.u8(report.one_way ? 1 : 0);
      break;
    }
    case EvidenceKind::SequenceReport: {
      const auto& report = std::get<SequenceReport>(item.payload);
      writer.u64(report.lowest_sequence);
      writer.u64(report.highest_sequence);
      writer.u64(report.received_count);
      writer.u8(report.sequence_restart ? 1 : 0);
      break;
    }
    case EvidenceKind::EndpointReport: {
      const auto& report = std::get<EndpointReport>(item.payload);
      writer.u8(static_cast<std::uint8_t>(report.role));
      writer.u64(report.node.value());
      writer.u64(report.port.value());
      writer.u64(report.count);
      writer.u8(report.counter_reset ? 1 : 0);
      break;
    }
    case EvidenceKind::Unknown:
      break;
  }
  if (!writer.ok()) {
    return {};
  }
  if (!writer.ok()) {
    return {};
  }
  buffer.resize(writer.size());
  return buffer;
}

Result<EvidenceItem> decode_evidence(ByteSpan bytes) {
  ByteReader reader{bytes};
  std::uint8_t version = 0;
  if (!reader.u8(version) || version != kEvidenceEncodingVersion) {
    return make_status(StatusCode::Corrupt, "unsupported evidence encoding version");
  }
  EvidenceItem item{};
  std::uint64_t raw = 0;
  if (!read_identity(reader, raw)) {
    return make_status(StatusCode::Corrupt, "truncated evidence header");
  }
  item.header.id = MeasurementId::from_value(raw);
  if (!read_identity(reader, raw)) {
    return make_status(StatusCode::Corrupt, "truncated evidence header");
  }
  item.header.source = SourceId::from_value(raw);
  if (!read_identity(reader, raw)) {
    return make_status(StatusCode::Corrupt, "truncated evidence header");
  }
  item.header.epoch = EpochId::from_value(raw);
  if (!read_identity(reader, raw)) {
    return make_status(StatusCode::Corrupt, "truncated evidence header");
  }
  item.header.generation = GenerationId::from_value(raw);
  if (!read_identity(reader, raw)) {
    return make_status(StatusCode::Corrupt, "truncated evidence header");
  }
  item.header.topology_revision = RevisionId::from_value(raw);
  if (!read_identity(reader, raw)) {
    return make_status(StatusCode::Corrupt, "truncated evidence header");
  }
  item.header.source_sequence = SequenceId::from_value(raw);

  std::int64_t observed = 0;
  std::int64_t received = 0;
  if (!reader.i64(observed) || !reader.i64(received)) {
    return make_status(StatusCode::Corrupt, "truncated evidence timestamps");
  }
  item.header.observed_at.value = Timestamp::from_unix_nanos(observed);
  item.header.received_at.value = Timestamp::from_unix_nanos(received);

  std::uint8_t method = 0;
  std::uint8_t granularity = 0;
  std::uint8_t subject_kind = 0;
  if (!reader.u8(method) || !reader.u8(granularity) || !reader.u8(subject_kind)) {
    return make_status(StatusCode::Corrupt, "truncated evidence classification fields");
  }
  if (method > static_cast<std::uint8_t>(MeasurementMethod::SyntheticInjection) ||
      granularity > static_cast<std::uint8_t>(Granularity::Queue) ||
      subject_kind > static_cast<std::uint8_t>(SubjectKind::Source)) {
    return make_status(StatusCode::Corrupt, "evidence classification field out of range");
  }
  item.header.method = static_cast<MeasurementMethod>(method);
  item.header.granularity = static_cast<Granularity>(granularity);
  const auto kind = static_cast<SubjectKind>(subject_kind);
  if (!read_identity(reader, raw)) {
    return make_status(StatusCode::Corrupt, "truncated evidence subject");
  }
  item.header.subject = SubjectRef::unknown();
  switch (kind) {
    case SubjectKind::Flow:
      item.header.subject = SubjectRef::flow(FlowId::from_value(raw));
      break;
    case SubjectKind::Path:
      item.header.subject = SubjectRef::path(PathId::from_value(raw));
      break;
    case SubjectKind::Hop:
      item.header.subject = SubjectRef::hop(HopId::from_value(raw));
      break;
    case SubjectKind::Link:
      item.header.subject = SubjectRef::link(LinkId::from_value(raw));
      break;
    case SubjectKind::Queue:
      item.header.subject = SubjectRef::queue(QueueId::from_value(raw));
      break;
    case SubjectKind::Source:
      item.header.subject = SubjectRef::source(SourceId::from_value(raw));
      break;
    case SubjectKind::Unknown:
      return make_status(StatusCode::Corrupt, "evidence subject kind is unknown");
  }
  if (!reader.text(item.header.note, kMaxEvidenceMetadataBytes)) {
    return make_status(StatusCode::Corrupt, "truncated or oversized evidence note");
  }
  std::uint8_t recovered = 0;
  std::uint8_t payload_kind = 0;
  if (!reader.u8(recovered) || !reader.u8(payload_kind)) {
    return make_status(StatusCode::Corrupt, "truncated evidence payload discriminator");
  }
  item.header.recovered_from_persistence = recovered != 0;

  switch (static_cast<EvidenceKind>(payload_kind)) {
    case EvidenceKind::CounterSample: {
      CounterSample sample{};
      std::uint8_t scope = 0;
      if (!reader.u64(raw)) {
        return make_status(StatusCode::Corrupt, "truncated counter sample");
      }
      sample.counter = CounterId::from_value(raw);
      if (!reader.u8(scope) || !reader.u64(sample.value) || !reader.u8(sample.width_bits)) {
        return make_status(StatusCode::Corrupt, "truncated counter sample");
      }
      if (scope > static_cast<std::uint8_t>(CounterScope::TransmittedBytes)) {
        return make_status(StatusCode::Corrupt, "counter scope out of range");
      }
      sample.scope = static_cast<CounterScope>(scope);
      std::uint64_t node = 0;
      std::uint64_t port = 0;
      std::uint64_t queue = 0;
      std::uint64_t hop = 0;
      std::uint64_t link = 0;
      if (!reader.u64(node) || !reader.u64(port) || !reader.u64(queue) || !reader.u64(hop) ||
          !reader.u64(link)) {
        return make_status(StatusCode::Corrupt, "truncated counter binding");
      }
      sample.node = NodeId::from_value(node);
      sample.port = PortId::from_value(port);
      sample.queue = QueueId::from_value(queue);
      sample.hop = HopId::from_value(hop);
      sample.link = LinkId::from_value(link);
      item.payload = sample;
      break;
    }
    case EvidenceKind::ProbeReport: {
      ProbeReport report{};
      std::uint8_t ttl_scoped = 0;
      std::uint8_t one_way = 0;
      if (!reader.u64(raw)) {
        return make_status(StatusCode::Corrupt, "truncated probe report");
      }
      report.probe = ProbeId::from_value(raw);
      if (!reader.u32(report.sent) || !reader.u32(report.received) || !reader.u32(report.timed_out) ||
          !reader.u8(ttl_scoped) || !reader.u8(report.ttl) || !reader.u8(one_way)) {
        return make_status(StatusCode::Corrupt, "truncated probe report");
      }
      report.ttl_scoped = ttl_scoped != 0;
      report.one_way = one_way != 0;
      item.payload = report;
      break;
    }
    case EvidenceKind::SequenceReport: {
      SequenceReport report{};
      std::uint8_t restart = 0;
      if (!reader.u64(report.lowest_sequence) || !reader.u64(report.highest_sequence) ||
          !reader.u64(report.received_count) || !reader.u8(restart)) {
        return make_status(StatusCode::Corrupt, "truncated sequence report");
      }
      report.sequence_restart = restart != 0;
      item.payload = report;
      break;
    }
    case EvidenceKind::EndpointReport: {
      EndpointReport report{};
      std::uint8_t role = 0;
      std::uint64_t node = 0;
      std::uint64_t port = 0;
      std::uint8_t reset = 0;
      if (!reader.u8(role) || !reader.u64(node) || !reader.u64(port) || !reader.u64(report.count) ||
          !reader.u8(reset)) {
        return make_status(StatusCode::Corrupt, "truncated endpoint report");
      }
      if (role > static_cast<std::uint8_t>(EndpointRole::Receiver)) {
        return make_status(StatusCode::Corrupt, "endpoint role out of range");
      }
      report.role = static_cast<EndpointRole>(role);
      report.node = NodeId::from_value(node);
      report.port = PortId::from_value(port);
      report.counter_reset = reset != 0;
      item.payload = report;
      break;
    }
    case EvidenceKind::Unknown:
      return make_status(StatusCode::Corrupt, "evidence payload kind is unknown");
  }
  if (!reader.at_end()) {
    return make_status(StatusCode::Corrupt, "trailing bytes after evidence payload");
  }
  return item;
}

namespace {

Result<Link> decode_link(ByteReader& reader) {
  std::uint64_t id = 0;
  std::uint64_t a_node = 0;
  std::uint64_t a_port = 0;
  std::uint64_t b_node = 0;
  std::uint64_t b_port = 0;
  std::uint8_t kind = 0;
  if (!reader.u64(id) || !reader.u64(a_node) || !reader.u64(a_port) || !reader.u64(b_node) ||
      !reader.u64(b_port) || !reader.u8(kind)) {
    return make_status(StatusCode::Corrupt, "truncated link record");
  }
  if (kind > static_cast<std::uint8_t>(LinkKind::InternalFabric)) {
    return make_status(StatusCode::Corrupt, "link kind out of range");
  }
  Link link{};
  link.id = LinkId::from_value(id);
  link.a = Endpoint{NodeId::from_value(a_node), PortId::from_value(a_port)};
  link.b = Endpoint{NodeId::from_value(b_node), PortId::from_value(b_port)};
  link.kind = static_cast<LinkKind>(kind);
  return link;
}

Result<Path> decode_path(ByteReader& reader) {
  std::uint64_t id = 0;
  std::uint8_t kind = 0;
  std::uint64_t revision = 0;
  std::uint32_t hop_count = 0;
  if (!reader.u64(id) || !reader.u8(kind) || !reader.u64(revision) || !reader.u32(hop_count)) {
    return make_status(StatusCode::Corrupt, "truncated path record");
  }
  if (kind > static_cast<std::uint8_t>(PathKind::Synthetic)) {
    return make_status(StatusCode::Corrupt, "path kind out of range");
  }
  if (hop_count > 4096) {
    return make_status(StatusCode::Corrupt, "path record declares an implausible hop count");
  }
  Path path{};
  path.id = PathId::from_value(id);
  path.kind = static_cast<PathKind>(kind);
  path.revision = RevisionId::from_value(revision);
  path.hops.reserve(hop_count);
  for (std::uint32_t i = 0; i < hop_count; ++i) {
    Hop hop{};
    std::uint64_t hop_id = 0;
    std::uint64_t node = 0;
    std::uint64_t ingress = 0;
    std::uint64_t egress = 0;
    std::uint64_t ingress_link = 0;
    std::uint64_t egress_link = 0;
    if (!reader.u64(hop_id) || !reader.u32(hop.index) || !reader.u64(node) || !reader.u64(ingress) ||
        !reader.u64(egress) || !reader.u64(ingress_link) || !reader.u64(egress_link)) {
      return make_status(StatusCode::Corrupt, "truncated hop record");
    }
    hop.id = HopId::from_value(hop_id);
    hop.node = NodeId::from_value(node);
    hop.ingress_port = PortId::from_value(ingress);
    hop.egress_port = PortId::from_value(egress);
    hop.ingress_link = LinkId::from_value(ingress_link);
    hop.egress_link = LinkId::from_value(egress_link);
    path.hops.push_back(hop);
  }
  return path;
}

Result<QueueEntity> decode_queue(ByteReader& reader) {
  std::uint64_t id = 0;
  std::uint64_t node = 0;
  std::uint64_t port = 0;
  std::uint64_t priority = 0;
  std::uint8_t kind = 0;
  if (!reader.u64(id) || !reader.u64(node) || !reader.u64(port) || !reader.u64(priority) ||
      !reader.u8(kind)) {
    return make_status(StatusCode::Corrupt, "truncated queue record");
  }
  if (kind > static_cast<std::uint8_t>(QueueKind::PriorityGroup)) {
    return make_status(StatusCode::Corrupt, "queue kind out of range");
  }
  QueueEntity queue{};
  queue.id = QueueId::from_value(id);
  queue.node = NodeId::from_value(node);
  queue.port = PortId::from_value(port);
  queue.priority = PriorityId::from_value(priority);
  queue.kind = static_cast<QueueKind>(kind);
  return queue;
}

Result<FlowBinding> decode_flow(ByteReader& reader) {
  std::uint64_t values[9] = {};
  for (std::uint64_t& value : values) {
    if (!reader.u64(value)) {
      return make_status(StatusCode::Corrupt, "truncated flow record");
    }
  }
  FlowBinding flow{};
  flow.id = FlowId::from_value(values[0]);
  flow.source = Endpoint{NodeId::from_value(values[1]), PortId::from_value(values[2])};
  flow.destination = Endpoint{NodeId::from_value(values[3]), PortId::from_value(values[4])};
  flow.protocol = ProtocolId::from_value(values[5]);
  flow.path = PathId::from_value(values[6]);
  flow.generation = GenerationId::from_value(values[7]);
  flow.binding_revision = RevisionId::from_value(values[8]);
  return flow;
}

Result<SourceDescriptor> decode_source(ByteReader& reader) {
  std::uint64_t id = 0;
  std::string name;
  std::uint8_t kind = 0;
  std::uint8_t authority = 0;
  if (!reader.u64(id) || !reader.text(name, kMaxSourceNameBytes) || !reader.u8(kind) ||
      !reader.u8(authority)) {
    return make_status(StatusCode::Corrupt, "truncated source record");
  }
  if (kind > static_cast<std::uint8_t>(SourceKind::Synthetic) ||
      authority > static_cast<std::uint8_t>(SourceAuthority::Primary)) {
    return make_status(StatusCode::Corrupt, "source classification out of range");
  }
  SourceDescriptor descriptor{};
  descriptor.id = SourceId::from_value(id);
  descriptor.name = std::move(name);
  descriptor.kind = static_cast<SourceKind>(kind);
  descriptor.authority = static_cast<SourceAuthority>(authority);
  return descriptor;
}

Result<SourceIncarnation> decode_incarnation(ByteReader& reader) {
  std::uint64_t source = 0;
  std::uint64_t epoch = 0;
  std::string name;
  std::int64_t activated = 0;
  std::uint8_t retired = 0;
  if (!reader.u64(source) || !reader.u64(epoch) || !reader.text(name, kMaxIncarnationNameBytes) ||
      !reader.i64(activated) || !reader.u8(retired)) {
    return make_status(StatusCode::Corrupt, "truncated incarnation record");
  }
  SourceIncarnation incarnation{};
  incarnation.source = SourceId::from_value(source);
  incarnation.epoch = EpochId::from_value(epoch);
  incarnation.incarnation = std::move(name);
  incarnation.activated_at = Timestamp::from_unix_nanos(activated);
  incarnation.retired = retired != 0;
  return incarnation;
}

Result<LossEpisode> decode_episode(ByteReader& reader) {
  LossEpisode episode{};
  std::uint64_t id = 0;
  std::uint64_t flow = 0;
  std::uint64_t path = 0;
  std::uint64_t generation = 0;
  std::uint8_t state = 0;
  std::uint8_t peak = 0;
  std::uint8_t granularity = 0;
  std::int64_t started = 0;
  std::int64_t last = 0;
  std::int64_t closed = 0;
  std::uint8_t ratio_defined = 0;
  std::uint8_t saturated = 0;
  std::uint64_t observations = 0;
  if (!reader.u64(id) || !reader.u64(flow) || !reader.u64(path) || !reader.u64(generation) ||
      !reader.u8(state) || !reader.u8(peak) || !reader.u8(granularity) || !reader.i64(started) ||
      !reader.i64(last) || !reader.i64(closed) || !reader.u64(episode.lost_total) ||
      !reader.u64(episode.offered_total) || !reader.u8(ratio_defined) ||
      !reader.u32(episode.peak_ratio_bp) || !reader.u32(episode.last_ratio_bp) ||
      !reader.u8(saturated) || !reader.u64(observations)) {
    return make_status(StatusCode::Corrupt, "truncated episode record");
  }
  if (state > static_cast<std::uint8_t>(EpisodeState::ClosedExplicitly) ||
      peak > static_cast<std::uint8_t>(LossClass::ImplausibleEvidence) ||
      granularity > static_cast<std::uint8_t>(Granularity::Queue)) {
    return make_status(StatusCode::Corrupt, "episode classification out of range");
  }
  episode.id = EpisodeId::from_value(id);
  episode.flow = FlowId::from_value(flow);
  episode.path = PathId::from_value(path);
  episode.generation = GenerationId::from_value(generation);
  episode.state = static_cast<EpisodeState>(state);
  episode.peak_class = static_cast<LossClass>(peak);
  episode.granularity = static_cast<Granularity>(granularity);
  episode.started_at = Timestamp::from_unix_nanos(started);
  episode.last_observed_at = Timestamp::from_unix_nanos(last);
  episode.closed_at = Timestamp::from_unix_nanos(closed);
  episode.ratio_defined = ratio_defined != 0;
  episode.totals_saturated = saturated != 0;
  episode.observation_count = static_cast<std::size_t>(observations);

  std::uint32_t source_count = 0;
  if (!reader.u32(source_count) || source_count > 4096) {
    return make_status(StatusCode::Corrupt, "truncated episode source list");
  }
  for (std::uint32_t i = 0; i < source_count; ++i) {
    std::uint64_t source = 0;
    if (!reader.u64(source)) {
      return make_status(StatusCode::Corrupt, "truncated episode source list");
    }
    episode.sources.push_back(SourceId::from_value(source));
  }
  std::uint32_t evidence_count = 0;
  if (!reader.u32(evidence_count) || evidence_count > 4096) {
    return make_status(StatusCode::Corrupt, "truncated episode evidence list");
  }
  for (std::uint32_t i = 0; i < evidence_count; ++i) {
    std::uint64_t measurement = 0;
    if (!reader.u64(measurement)) {
      return make_status(StatusCode::Corrupt, "truncated episode evidence list");
    }
    episode.evidence_ids.push_back(MeasurementId::from_value(measurement));
  }
  std::uint8_t truncated = 0;
  if (!reader.u8(truncated) || !reader.text(episode.state_detail, 256)) {
    return make_status(StatusCode::Corrupt, "truncated episode trailer");
  }
  episode.evidence_ids_truncated = truncated != 0;
  return episode;
}

}  // namespace

std::vector<std::uint8_t> encode_topology_entity(const Link& link) {
  std::vector<std::uint8_t> buffer(512, 0);
  ByteWriter writer{MutableByteSpan{buffer.data(), buffer.size()}};
  writer.u8(kTopologyEncodingVersion);
  writer.u8(static_cast<std::uint8_t>(TopologyEntityKind::Link));
  writer.u64(link.id.value());
  writer.u64(link.a.node.value());
  writer.u64(link.a.port.value());
  writer.u64(link.b.node.value());
  writer.u64(link.b.port.value());
  writer.u8(static_cast<std::uint8_t>(link.kind));
  if (!writer.ok()) {
    return {};
  }
  buffer.resize(writer.size());
  return buffer;
}

std::vector<std::uint8_t> encode_topology_entity(const Path& path) {
  std::vector<std::uint8_t> buffer(64 + path.hops.size() * 64, 0);
  ByteWriter writer{MutableByteSpan{buffer.data(), buffer.size()}};
  writer.u8(kTopologyEncodingVersion);
  writer.u8(static_cast<std::uint8_t>(TopologyEntityKind::Path));
  writer.u64(path.id.value());
  writer.u8(static_cast<std::uint8_t>(path.kind));
  writer.u64(path.revision.value());
  writer.u32(static_cast<std::uint32_t>(path.hops.size()));
  for (const Hop& hop : path.hops) {
    writer.u64(hop.id.value());
    writer.u32(hop.index);
    writer.u64(hop.node.value());
    writer.u64(hop.ingress_port.value());
    writer.u64(hop.egress_port.value());
    writer.u64(hop.ingress_link.value());
    writer.u64(hop.egress_link.value());
  }
  if (!writer.ok()) {
    return {};
  }
  buffer.resize(writer.size());
  return buffer;
}

std::vector<std::uint8_t> encode_topology_entity(const QueueEntity& queue) {
  std::vector<std::uint8_t> buffer(128, 0);
  ByteWriter writer{MutableByteSpan{buffer.data(), buffer.size()}};
  writer.u8(kTopologyEncodingVersion);
  writer.u8(static_cast<std::uint8_t>(TopologyEntityKind::Queue));
  writer.u64(queue.id.value());
  writer.u64(queue.node.value());
  writer.u64(queue.port.value());
  writer.u64(queue.priority.value());
  writer.u8(static_cast<std::uint8_t>(queue.kind));
  if (!writer.ok()) {
    return {};
  }
  buffer.resize(writer.size());
  return buffer;
}

std::vector<std::uint8_t> encode_topology_entity(const FlowBinding& flow) {
  std::vector<std::uint8_t> buffer(256, 0);
  ByteWriter writer{MutableByteSpan{buffer.data(), buffer.size()}};
  writer.u8(kTopologyEncodingVersion);
  writer.u8(static_cast<std::uint8_t>(TopologyEntityKind::Flow));
  writer.u64(flow.id.value());
  writer.u64(flow.source.node.value());
  writer.u64(flow.source.port.value());
  writer.u64(flow.destination.node.value());
  writer.u64(flow.destination.port.value());
  writer.u64(flow.protocol.value());
  writer.u64(flow.path.value());
  writer.u64(flow.generation.value());
  writer.u64(flow.binding_revision.value());
  if (!writer.ok()) {
    return {};
  }
  buffer.resize(writer.size());
  return buffer;
}

std::vector<std::uint8_t> encode_source(const SourceDescriptor& descriptor) {
  std::vector<std::uint8_t> buffer(512, 0);
  ByteWriter writer{MutableByteSpan{buffer.data(), buffer.size()}};
  writer.u8(kTopologyEncodingVersion);
  writer.u64(descriptor.id.value());
  writer.text(descriptor.name);
  writer.u8(static_cast<std::uint8_t>(descriptor.kind));
  writer.u8(static_cast<std::uint8_t>(descriptor.authority));
  if (!writer.ok()) {
    return {};
  }
  buffer.resize(writer.size());
  return buffer;
}

std::vector<std::uint8_t> encode_incarnation(const SourceIncarnation& incarnation) {
  std::vector<std::uint8_t> buffer(512, 0);
  ByteWriter writer{MutableByteSpan{buffer.data(), buffer.size()}};
  writer.u8(kTopologyEncodingVersion);
  writer.u64(incarnation.source.value());
  writer.u64(incarnation.epoch.value());
  writer.text(incarnation.incarnation);
  writer.i64(incarnation.activated_at.unix_nanos());
  writer.u8(incarnation.retired ? 1 : 0);
  if (!writer.ok()) {
    return {};
  }
  buffer.resize(writer.size());
  return buffer;
}

std::vector<std::uint8_t> encode_episode(const LossEpisode& episode) {
  std::vector<std::uint8_t> buffer(2048, 0);
  ByteWriter writer{MutableByteSpan{buffer.data(), buffer.size()}};
  writer.u8(kTopologyEncodingVersion);
  writer.u64(episode.id.value());
  writer.u64(episode.flow.value());
  writer.u64(episode.path.value());
  writer.u64(episode.generation.value());
  writer.u8(static_cast<std::uint8_t>(episode.state));
  writer.u8(static_cast<std::uint8_t>(episode.peak_class));
  writer.u8(static_cast<std::uint8_t>(episode.granularity));
  writer.i64(episode.started_at.unix_nanos());
  writer.i64(episode.last_observed_at.unix_nanos());
  writer.i64(episode.closed_at.unix_nanos());
  writer.u64(episode.lost_total);
  writer.u64(episode.offered_total);
  writer.u8(episode.ratio_defined ? 1 : 0);
  writer.u32(episode.peak_ratio_bp);
  writer.u32(episode.last_ratio_bp);
  writer.u8(episode.totals_saturated ? 1 : 0);
  writer.u64(episode.observation_count);
  writer.u32(static_cast<std::uint32_t>(episode.sources.size()));
  for (const SourceId source : episode.sources) {
    writer.u64(source.value());
  }
  writer.u32(static_cast<std::uint32_t>(episode.evidence_ids.size()));
  for (const MeasurementId id : episode.evidence_ids) {
    writer.u64(id.value());
  }
  writer.u8(episode.evidence_ids_truncated ? 1 : 0);
  writer.text(episode.state_detail);
  if (!writer.ok()) {
    return {};
  }
  buffer.resize(writer.size());
  return buffer;
}

TopologyEntityKind topology_entity_kind(ByteSpan bytes) {
  if (bytes.size() < 2) {
    return TopologyEntityKind::Unknown;
  }
  const auto value = static_cast<std::uint8_t>(bytes[1]);
  if (value > static_cast<std::uint8_t>(TopologyEntityKind::Flow)) {
    return TopologyEntityKind::Unknown;
  }
  return static_cast<TopologyEntityKind>(value);
}

// ---------------------------------------------------------------------------
// Journal
// ---------------------------------------------------------------------------

PersistenceStore::PersistenceStore(PersistConfig config) : config_(std::move(config)) {}

PersistenceStore::~PersistenceStore() {
  if (stream_) {
    stream_->flush();
    stream_.reset();
  }
}

std::string PersistenceStore::journal_path() const {
  const std::filesystem::path directory(config_.directory);
  return (directory / (config_.instance + ".loj")).string();
}

std::string PersistenceStore::temp_path() const {
  const std::filesystem::path directory(config_.directory);
  return (directory / (config_.instance + ".loj.tmp")).string();
}

Result<void> PersistenceStore::write_bytes_locked(ByteSpan bytes) {
  if (!stream_) {
    return make_status(StatusCode::Internal, "journal is not open");
  }
  if (!bytes.empty()) {
    stream_->write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream_->good()) {
      return make_status(StatusCode::Internal, "short write to the journal");
    }
  }
  stats_.bytes_written += bytes.size();
  stats_.dirty = true;
  return {};
}

Result<void> PersistenceStore::write_record_locked(RecordType type, ByteSpan payload) {
  if (payload.empty()) {
    return make_status(StatusCode::Internal, "refusing to write an empty record payload");
  }
  if (payload.size() > config_.limits.max_payload_bytes) {
    ++stats_.refused_payloads;
    return make_status(StatusCode::CapacityExceeded, "record payload exceeds the configured bound");
  }
  std::uint8_t header[kRecordHeaderBytes] = {};
  ByteWriter writer{MutableByteSpan{header, kRecordHeaderBytes}};
  writer.u32(kRecordMagic);
  writer.u16(static_cast<std::uint16_t>(type));
  writer.u16(0);
  writer.u32(static_cast<std::uint32_t>(payload.size()));
  writer.u64(stats_.records_written + 1);
  if (!writer.ok()) {
    return make_status(StatusCode::Internal, "failed to frame a journal record");
  }
  std::uint32_t crc = crc32c_extend(0, ByteSpan{header, kRecordPrefixBytes});
  crc = crc32c_extend(crc, payload);
  ByteWriter crc_writer{MutableByteSpan{header + kRecordPrefixBytes, 4}};
  crc_writer.u32(crc);
  if (!crc_writer.ok()) {
    return make_status(StatusCode::Internal, "failed to frame a journal record");
  }
  const Result<void> header_result = write_bytes_locked(ByteSpan{header, kRecordHeaderBytes});
  if (!header_result.ok()) {
    return header_result;
  }
  const Result<void> payload_result = write_bytes_locked(payload);
  if (!payload_result.ok()) {
    return payload_result;
  }
  ++stats_.records_written;
  return {};
}

Result<EngineEpoch> PersistenceStore::open_session(Timestamp now, std::string incarnation) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!config_.enabled) {
    return make_status(StatusCode::Unsupported, "persistence is not enabled in this configuration");
  }
  std::error_code error;
  std::filesystem::create_directories(config_.directory, error);
  if (error) {
    return make_status(StatusCode::Internal, "cannot create the persistence directory");
  }
  const std::string path = journal_path();
  const bool exists = std::filesystem::exists(path, error);
  if (!exists) {
    std::ofstream create(path, std::ios::binary | std::ios::trunc);
    if (!create.good()) {
      return make_status(StatusCode::Internal, "cannot create the journal");
    }
  }
  stream_ = std::make_unique<std::fstream>(path, std::ios::in | std::ios::out | std::ios::binary |
                                                     std::ios::app);
  if (!stream_->good()) {
    stream_.reset();
    return make_status(StatusCode::Internal, "cannot open the journal for writing");
  }
  stream_->clear();
  stream_->seekg(0, std::ios::end);
  const std::streamoff size = stream_->tellg();
  if (size < 0) {
    stream_.reset();
    return make_status(StatusCode::Internal, "cannot determine the journal size");
  }
  stats_.open = true;
  stats_.bytes_written = static_cast<std::uint64_t>(size);

  if (size == 0) {
    std::vector<std::uint8_t> manifest(512, 0);
    ByteWriter writer{MutableByteSpan{manifest.data(), manifest.size()}};
    writer.u32(kPersistFormatVersion);
    writer.u32(kSemanticsRevision);
    writer.text(full_version_string());
    writer.i64(now.unix_nanos());
    writer.text(config_.instance);
    manifest.resize(writer.size());
    if (!writer.ok()) {
      return make_status(StatusCode::Internal, "failed to encode the journal manifest");
    }
    const Result<void> written = write_record_locked(RecordType::Manifest, ByteSpan{manifest});
    if (!written.ok()) {
      return written.status();
    }
    manifest_written_ = true;
  }

  std::uint64_t mixed = combine_hash(fnv1a64(config_.instance), static_cast<std::uint64_t>(now.unix_nanos()));
  mixed = combine_hash(mixed, fnv1a64(incarnation));
  mixed = combine_hash(mixed, stats_.records_written + 1);
  epoch_ = EngineEpoch{};
  epoch_.id = EpochId::from_value(mixed == 0 ? 1 : mixed);
  epoch_.booted_at = now;
  epoch_.incarnation = std::move(incarnation);

  std::vector<std::uint8_t> session(512, 0);
  ByteWriter session_writer{MutableByteSpan{session.data(), session.size()}};
  session_writer.u64(epoch_.id.value());
  session_writer.i64(now.unix_nanos());
  session_writer.text(epoch_.incarnation);
  session.resize(session_writer.size());
  if (!session_writer.ok()) {
    return make_status(StatusCode::Internal, "failed to encode the session record");
  }
  const Result<void> session_written = write_record_locked(RecordType::SessionRecord, ByteSpan{session});
  if (!session_written.ok()) {
    return session_written.status();
  }
  const Result<void> flushed = flush_locked();
  if (!flushed.ok()) {
    return flushed.status();
  }
  return epoch_;
}

Result<void> PersistenceStore::flush_locked() {
  if (!stream_) {
    return {};
  }
  stream_->flush();
  if (!stream_->good()) {
    return make_status(StatusCode::Internal, "failed to flush the journal");
  }
  stats_.dirty = false;
  return {};
}

Result<void> PersistenceStore::flush() {
  std::lock_guard<std::mutex> guard(mutex_);
  return flush_locked();
}

Result<void> PersistenceStore::close() {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!stream_) {
    stats_.open = false;
    return {};
  }
  stream_->flush();
  const bool good = stream_->good();
  stream_->close();
  const bool closed = !stream_->fail();
  stream_.reset();
  stats_.open = false;
  if (!good || !closed) {
    return make_status(StatusCode::Internal, "failed to close the journal cleanly");
  }
  return {};
}

Result<void> PersistenceStore::append_topology_entity(const Link& link) {
  std::lock_guard<std::mutex> guard(mutex_);
  const std::vector<std::uint8_t> payload = encode_topology_entity(link);
  return write_record_locked(RecordType::TopologyEntity, ByteSpan{payload});
}

Result<void> PersistenceStore::append_topology_entity(const Path& path) {
  std::lock_guard<std::mutex> guard(mutex_);
  const std::vector<std::uint8_t> payload = encode_topology_entity(path);
  return write_record_locked(RecordType::TopologyEntity, ByteSpan{payload});
}

Result<void> PersistenceStore::append_topology_entity(const QueueEntity& queue) {
  std::lock_guard<std::mutex> guard(mutex_);
  const std::vector<std::uint8_t> payload = encode_topology_entity(queue);
  return write_record_locked(RecordType::TopologyEntity, ByteSpan{payload});
}

Result<void> PersistenceStore::append_topology_entity(const FlowBinding& flow) {
  std::lock_guard<std::mutex> guard(mutex_);
  const std::vector<std::uint8_t> payload = encode_topology_entity(flow);
  return write_record_locked(RecordType::TopologyEntity, ByteSpan{payload});
}

Result<void> PersistenceStore::append_topology(const TopologyRegistry& topology) {
  std::lock_guard<std::mutex> guard(mutex_);
  for (const Link& link : topology.links()) {
    const std::vector<std::uint8_t> payload = encode_topology_entity(link);
    const Result<void> result = write_record_locked(RecordType::TopologyEntity, ByteSpan{payload});
    if (!result.ok()) {
      return result;
    }
  }
  for (const Path& path : topology.paths()) {
    const std::vector<std::uint8_t> payload = encode_topology_entity(path);
    const Result<void> result = write_record_locked(RecordType::TopologyEntity, ByteSpan{payload});
    if (!result.ok()) {
      return result;
    }
  }
  for (const QueueEntity& queue : topology.queues()) {
    const std::vector<std::uint8_t> payload = encode_topology_entity(queue);
    const Result<void> result = write_record_locked(RecordType::TopologyEntity, ByteSpan{payload});
    if (!result.ok()) {
      return result;
    }
  }
  for (const FlowBinding& flow : topology.flows()) {
    const std::vector<std::uint8_t> payload = encode_topology_entity(flow);
    const Result<void> result = write_record_locked(RecordType::TopologyEntity, ByteSpan{payload});
    if (!result.ok()) {
      return result;
    }
  }
  return {};
}

Result<void> PersistenceStore::append_source(const SourceDescriptor& descriptor) {
  std::lock_guard<std::mutex> guard(mutex_);
  const std::vector<std::uint8_t> payload = encode_source(descriptor);
  return write_record_locked(RecordType::SourceDescriptorRecord, ByteSpan{payload});
}

Result<void> PersistenceStore::append_incarnation(const SourceIncarnation& incarnation) {
  std::lock_guard<std::mutex> guard(mutex_);
  const std::vector<std::uint8_t> payload = encode_incarnation(incarnation);
  return write_record_locked(RecordType::SourceIncarnationRecord, ByteSpan{payload});
}

Result<void> PersistenceStore::append_evidence(const EvidenceItem& item) {
  std::lock_guard<std::mutex> guard(mutex_);
  const std::vector<std::uint8_t> payload = encode_evidence(item);
  const Result<void> result = write_record_locked(RecordType::EvidenceRecord, ByteSpan{payload});
  if (result.ok()) {
    ++stats_.evidence_persisted;
  }
  return result;
}

Result<void> PersistenceStore::append_episode(const LossEpisode& episode) {
  std::lock_guard<std::mutex> guard(mutex_);
  const std::vector<std::uint8_t> payload = encode_episode(episode);
  return write_record_locked(RecordType::EpisodeRecord, ByteSpan{payload});
}

PersistStats PersistenceStore::stats() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return stats_;
}

RecoveryReport PersistenceStore::recovery() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return recovery_;
}

std::uint64_t PersistenceStore::journal_bytes() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return stats_.bytes_written;
}

bool PersistenceStore::should_compact() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return stats_.bytes_written >= config_.limits.max_journal_bytes;
}

Result<RecoveryReport> PersistenceStore::load(TopologyRegistry& topology, SourceRegistry& sources,
                                              EvidenceStore& evidence, EpisodeTracker& episodes,
                                              Timestamp now) {
  std::lock_guard<std::mutex> guard(mutex_);
  RecoveryReport report{};
  report.opened = config_.enabled;
  if (!config_.enabled) {
    report.detail = "persistence is disabled; nothing was recovered";
    recovery_ = report;
    return report;
  }
  const std::string path = journal_path();
  std::error_code error;
  if (!std::filesystem::exists(path, error)) {
    report.detail = "no journal exists yet; starting from declared state";
    recovery_ = report;
    return report;
  }
  std::ifstream file(path, std::ios::binary);
  if (!file.good()) {
    return make_status(StatusCode::Internal, "cannot open the journal for reading");
  }

  std::vector<std::uint8_t> header(kRecordHeaderBytes);
  std::vector<std::uint8_t> payload;
  bool manifest_seen = false;
  bool stop = false;
  const PersistLimits& limits = config_.limits;

  bool at_end = false;
  while (!stop && !at_end) {
    file.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(kRecordHeaderBytes));
    const std::streamsize got = file.gcount();
    if (got == 0) {
      at_end = true;
      break;
    }
    if (got != static_cast<std::streamsize>(kRecordHeaderBytes)) {
      report.truncated = true;
      report.detail = "journal ends inside a record header";
      break;
    }
    ByteReader reader{ByteSpan{header.data(), kRecordHeaderBytes}};
    std::uint32_t magic = 0;
    std::uint16_t raw_type = 0;
    std::uint16_t flags = 0;
    std::uint32_t length = 0;
    std::uint64_t sequence = 0;
    std::uint32_t stored_crc = 0;
    if (!reader.u32(magic) || !reader.u16(raw_type) || !reader.u16(flags) || !reader.u32(length) ||
        !reader.u64(sequence) || !reader.u32(stored_crc)) {
      report.truncated = true;
      report.detail = "journal record header is not decodable";
      break;
    }
    if (magic != kRecordMagic) {
      report.truncated = true;
      report.detail = "journal record magic does not match; refusing to resynchronise";
      break;
    }
    if (length > limits.max_payload_bytes) {
      report.truncated = true;
      report.detail = "journal record declares an oversized payload";
      break;
    }
    payload.resize(length);
    if (length > 0) {
      file.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(length));
      if (file.gcount() != static_cast<std::streamsize>(length)) {
        report.truncated = true;
        report.detail = "journal ends inside a record payload";
        break;
      }
    }
    report.bytes_read += kRecordHeaderBytes + length;
    ++report.records_read;
    std::uint32_t computed = crc32c_extend(0, ByteSpan{header.data(), kRecordPrefixBytes});
    computed = crc32c_extend(computed, ByteSpan{payload.data(), payload.size()});
    if (computed != stored_crc) {
      report.checksum_failure = true;
      report.truncated = true;
      report.detail = "journal record failed its integrity check; recovery stopped here";
      ++stats_.checksum_failures;
      ++report.records_discarded;
      ++stats_.discarded_records;
      break;
    }
    if (report.records_read > limits.max_records_loaded) {
      report.truncated = true;
      report.detail = "journal record bound reached during recovery";
      break;
    }
    report.valid_bytes = report.bytes_read;

    const auto type = static_cast<RecordType>(raw_type);
    if (!manifest_seen && type != RecordType::Manifest) {
      report.truncated = true;
      report.detail = "journal does not begin with a manifest";
      break;
    }

    bool accepted = true;
    std::string reject_detail;
    ByteReader body{ByteSpan{payload.data(), payload.size()}};
    switch (type) {
      case RecordType::Manifest: {
        manifest_seen = true;
        std::uint32_t format = 0;
        std::uint32_t semantics = 0;
        std::string product;
        std::string instance;
        std::int64_t created = 0;
        if (!body.u32(format) || !body.u32(semantics) || !body.text(product, 128) ||
            !body.i64(created) || !body.text(instance, 128)) {
          report.truncated = true;
          report.detail = "journal manifest is not decodable";
          stop = true;
          break;
        }
        if (format != kPersistFormatVersion) {
          report.version_mismatch = true;
          report.detail = "journal format version does not match this build";
          accepted = false;
          stop = true;
          break;
        }
        if (semantics != kSemanticsRevision) {
          report.semantics_mismatch = true;
          report.detail = "journal was written with different classification semantics";
          accepted = false;
          stop = true;
          break;
        }
        break;
      }
      case RecordType::SessionRecord: {
        std::uint64_t epoch = 0;
        std::int64_t booted = 0;
        std::string incarnation;
        if (!body.u64(epoch) || !body.i64(booted) || !body.text(incarnation, 128)) {
          accepted = false;
          reject_detail = "session record is not decodable";
          break;
        }
        // Only the identity matters here; the previous session is history.
        break;
      }
      case RecordType::TopologyEntity: {
        if (report.topology_restored >= limits.max_topology_loaded) {
          accepted = false;
          reject_detail = "topology bound reached during recovery";
          break;
        }
        const TopologyEntityKind kind = topology_entity_kind(ByteSpan{payload});
        std::uint8_t version = 0;
        std::uint8_t encoded_kind = 0;
        if (!body.u8(version) || !body.u8(encoded_kind)) {
          accepted = false;
          reject_detail = "topology record header is truncated";
          break;
        }
        if (version != kTopologyEncodingVersion) {
          accepted = false;
          reject_detail = "topology record version mismatch";
          break;
        }
        if (encoded_kind != static_cast<std::uint8_t>(kind)) {
          accepted = false;
          reject_detail = "topology record entity kind does not match its discriminator";
          break;
        }
        Result<UpsertResult> outcome = make_status(StatusCode::Internal, "unreachable");
        switch (kind) {
          case TopologyEntityKind::Link: {
            Result<Link> decoded = decode_link(body);
            if (!decoded.ok()) {
              accepted = false;
              reject_detail = decoded.status().message();
              break;
            }
            outcome = topology.upsert_link(decoded.value(), true);
            break;
          }
          case TopologyEntityKind::Path: {
            Result<Path> decoded = decode_path(body);
            if (!decoded.ok()) {
              accepted = false;
              reject_detail = decoded.status().message();
              break;
            }
            outcome = topology.upsert_path(decoded.value(), true);
            break;
          }
          case TopologyEntityKind::Queue: {
            Result<QueueEntity> decoded = decode_queue(body);
            if (!decoded.ok()) {
              accepted = false;
              reject_detail = decoded.status().message();
              break;
            }
            outcome = topology.upsert_queue(decoded.value(), true);
            break;
          }
          case TopologyEntityKind::Flow: {
            Result<FlowBinding> decoded = decode_flow(body);
            if (!decoded.ok()) {
              accepted = false;
              reject_detail = decoded.status().message();
              break;
            }
            outcome = topology.upsert_flow(decoded.value(), true);
            break;
          }
          case TopologyEntityKind::Unknown:
            accepted = false;
            reject_detail = "unknown topology entity kind";
            break;
        }
        if (accepted && !outcome.ok()) {
          accepted = false;
          reject_detail = outcome.status().message();
        }
        if (accepted) {
          ++report.topology_restored;
        }
        break;
      }
      case RecordType::SourceDescriptorRecord: {
        if (report.sources_restored >= limits.max_sources_loaded) {
          accepted = false;
          reject_detail = "source bound reached during recovery";
          break;
        }
        std::uint8_t version = 0;
        if (!body.u8(version) || version != kTopologyEncodingVersion) {
          accepted = false;
          reject_detail = "source record version mismatch";
          break;
        }
        Result<SourceDescriptor> decoded = decode_source(body);
        if (!decoded.ok()) {
          accepted = false;
          reject_detail = decoded.status().message();
          break;
        }
        const Result<UpsertOutcome> outcome = sources.register_source(decoded.value(), true);
        if (!outcome.ok()) {
          accepted = false;
          reject_detail = outcome.status().message();
          break;
        }
        ++report.sources_restored;
        break;
      }
      case RecordType::SourceIncarnationRecord: {
        std::uint8_t version = 0;
        if (!body.u8(version) || version != kTopologyEncodingVersion) {
          accepted = false;
          reject_detail = "incarnation record version mismatch";
          break;
        }
        Result<SourceIncarnation> decoded = decode_incarnation(body);
        if (!decoded.ok()) {
          accepted = false;
          reject_detail = decoded.status().message();
          break;
        }
        const Result<void> outcome = sources.restore_incarnation(decoded.value());
        if (!outcome.ok()) {
          accepted = false;
          reject_detail = outcome.status().message();
          break;
        }
        break;
      }
      case RecordType::EvidenceRecord: {
        if (report.evidence_restored >= limits.max_evidence_loaded) {
          accepted = false;
          reject_detail = "evidence bound reached during recovery";
          break;
        }
        Result<EvidenceItem> decoded = decode_evidence(ByteSpan{payload});
        if (!decoded.ok()) {
          accepted = false;
          reject_detail = decoded.status().message();
          break;
        }
        const IngestOutcome outcome = evidence.restore(decoded.value());
        if (!outcome.accepted) {
          accepted = false;
          reject_detail = outcome.detail;
          break;
        }
        ++report.evidence_restored;
        ++report.evidence_marked_not_current;
        break;
      }
      case RecordType::EpisodeRecord: {
        if (report.episodes_restored >= limits.max_episodes_loaded) {
          accepted = false;
          reject_detail = "episode bound reached during recovery";
          break;
        }
        std::uint8_t version = 0;
        if (!body.u8(version) || version != kTopologyEncodingVersion) {
          accepted = false;
          reject_detail = "episode record version mismatch";
          break;
        }
        Result<LossEpisode> decoded = decode_episode(body);
        if (!decoded.ok()) {
          accepted = false;
          reject_detail = decoded.status().message();
          break;
        }
        const bool was_open = decoded.value().open();
        episodes.restore(decoded.value());
        ++report.episodes_restored;
        if (was_open) {
          ++report.episodes_sealed;
        }
        break;
      }
      case RecordType::CompactionMarker:
        break;
    }

    if (accepted) {
      ++report.records_accepted;
    } else {
      ++report.records_discarded;
      ++stats_.discarded_records;
      if (report.detail.empty()) {
        report.detail = "at least one record was refused: " + reject_detail;
      }
    }
  }

  file.close();
  report.records_discarded += report.records_read - report.records_accepted - report.records_discarded;
  if (report.truncated && report.bytes_read > report.valid_bytes) {
    stats_.refused_payloads += 0;  // byte accounting is reported through RecoveryReport
  }
  if (report.detail.empty()) {
    report.detail = "journal read to completion";
  }
  (void)now;
  recovery_ = report;
  return report;
}

Result<void> PersistenceStore::compact(const TopologyRegistry& topology, const SourceRegistry& sources,
                                       const EvidenceStore& evidence, const EpisodeTracker& episodes) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!config_.enabled) {
    return make_status(StatusCode::Unsupported, "persistence is not enabled in this configuration");
  }
  const std::string temporary = temp_path();
  auto target = std::make_unique<std::fstream>(temporary, std::ios::in | std::ios::out | std::ios::binary |
                                                               std::ios::trunc);
  if (!target->good()) {
    return make_status(StatusCode::Internal, "cannot create the compaction target");
  }
  std::unique_ptr<std::fstream> previous = std::move(stream_);
  stream_ = std::move(target);
  const std::uint64_t records_before = stats_.records_written;
  const std::uint64_t bytes_before = stats_.bytes_written;
  stats_.records_written = 0;
  stats_.bytes_written = 0;

  auto fail = [&](const Status& status) -> Result<void> {
    if (stream_) {
      stream_->close();
      stream_.reset();
    }
    if (previous) {
      previous->close();
      previous.reset();
    }
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    stats_.records_written = records_before;
    stats_.bytes_written = bytes_before;
    stream_ = std::make_unique<std::fstream>(journal_path(), std::ios::in | std::ios::out |
                                                                 std::ios::binary | std::ios::app);
    if (!stream_ || !stream_->good()) {
      stream_.reset();
      return make_status(StatusCode::Internal,
                         "compaction failed and the journal could not be reopened");
    }
    return status;
  };

  {
    std::vector<std::uint8_t> manifest(512, 0);
    ByteWriter writer{MutableByteSpan{manifest.data(), manifest.size()}};
    writer.u32(kPersistFormatVersion);
    writer.u32(kSemanticsRevision);
    writer.text(full_version_string());
    writer.i64(epoch_.booted_at.unix_nanos());
    writer.text(config_.instance);
    manifest.resize(writer.size());
    const Result<void> written = write_record_locked(RecordType::Manifest, ByteSpan{manifest});
    if (!written.ok()) {
      return fail(written.status());
    }
  }
  {
    std::vector<std::uint8_t> session(512, 0);
    ByteWriter writer{MutableByteSpan{session.data(), session.size()}};
    writer.u64(epoch_.id.value());
    writer.i64(epoch_.booted_at.unix_nanos());
    writer.text(epoch_.incarnation);
    session.resize(writer.size());
    const Result<void> written = write_record_locked(RecordType::SessionRecord, ByteSpan{session});
    if (!written.ok()) {
      return fail(written.status());
    }
  }
  for (const Link& link : topology.links()) {
    const std::vector<std::uint8_t> payload = encode_topology_entity(link);
    const Result<void> written = write_record_locked(RecordType::TopologyEntity, ByteSpan{payload});
    if (!written.ok()) {
      return fail(written.status());
    }
  }
  for (const Path& path : topology.paths()) {
    const std::vector<std::uint8_t> payload = encode_topology_entity(path);
    const Result<void> written = write_record_locked(RecordType::TopologyEntity, ByteSpan{payload});
    if (!written.ok()) {
      return fail(written.status());
    }
  }
  for (const QueueEntity& queue : topology.queues()) {
    const std::vector<std::uint8_t> payload = encode_topology_entity(queue);
    const Result<void> written = write_record_locked(RecordType::TopologyEntity, ByteSpan{payload});
    if (!written.ok()) {
      return fail(written.status());
    }
  }
  for (const FlowBinding& flow : topology.flows()) {
    const std::vector<std::uint8_t> payload = encode_topology_entity(flow);
    const Result<void> written = write_record_locked(RecordType::TopologyEntity, ByteSpan{payload});
    if (!written.ok()) {
      return fail(written.status());
    }
  }
  for (const SourceDescriptor& descriptor : sources.sources()) {
    const std::vector<std::uint8_t> payload = encode_source(descriptor);
    const Result<void> written = write_record_locked(RecordType::SourceDescriptorRecord, ByteSpan{payload});
    if (!written.ok()) {
      return fail(written.status());
    }
  }
  for (const SourceIncarnation& incarnation : sources.incarnations()) {
    const std::vector<std::uint8_t> payload = encode_incarnation(incarnation);
    const Result<void> written =
        write_record_locked(RecordType::SourceIncarnationRecord, ByteSpan{payload});
    if (!written.ok()) {
      return fail(written.status());
    }
  }
  for (const EvidenceItem& item : evidence.snapshot()) {
    const std::vector<std::uint8_t> payload = encode_evidence(item);
    const Result<void> written = write_record_locked(RecordType::EvidenceRecord, ByteSpan{payload});
    if (!written.ok()) {
      return fail(written.status());
    }
  }
  for (const LossEpisode& episode : episodes.episodes()) {
    const std::vector<std::uint8_t> payload = encode_episode(episode);
    const Result<void> written = write_record_locked(RecordType::EpisodeRecord, ByteSpan{payload});
    if (!written.ok()) {
      return fail(written.status());
    }
  }
  {
    const std::uint8_t marker[4] = {1, 0, 0, 0};
    const Result<void> written = write_record_locked(RecordType::CompactionMarker, ByteSpan{marker, 4});
    if (!written.ok()) {
      return fail(written.status());
    }
  }
  stream_->flush();
  if (!stream_->good()) {
    return fail(make_status(StatusCode::Internal, "failed to flush the compacted journal"));
  }
  stream_->close();
  stream_.reset();
  if (previous) {
    previous->close();
    previous.reset();
  }

  std::error_code error;
  std::filesystem::rename(temporary, journal_path(), error);
  if (error) {
    // The original journal is untouched; reopen it so the store stays usable.
    stream_ = std::make_unique<std::fstream>(journal_path(), std::ios::in | std::ios::out |
                                                                 std::ios::binary | std::ios::app);
    return make_status(StatusCode::Internal, "failed to swap in the compacted journal");
  }
  const std::uint64_t compacted_bytes = stats_.bytes_written;
  const std::uint64_t compacted_records = stats_.records_written;
  stream_ = std::make_unique<std::fstream>(journal_path(), std::ios::in | std::ios::out |
                                                               std::ios::binary | std::ios::app);
  if (!stream_->good()) {
    stream_.reset();
    return make_status(StatusCode::Internal, "failed to reopen the compacted journal");
  }
  stats_.bytes_written = compacted_bytes;
  stats_.records_written = compacted_records;
  ++stats_.compactions;
  return {};
}

}  // namespace loss_observatory
