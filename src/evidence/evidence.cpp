#include "loss_observatory/evidence/evidence.hpp"

#include <algorithm>
#include <cstddef>

#include "loss_observatory/evidence/counter.hpp"
#include <string>
#include <type_traits>

namespace loss_observatory {

std::string_view to_string(EvidenceKind kind) noexcept {
  switch (kind) {
    case EvidenceKind::Unknown:
      return "unknown";
    case EvidenceKind::CounterSample:
      return "counter";
    case EvidenceKind::ProbeReport:
      return "probe";
    case EvidenceKind::SequenceReport:
      return "sequence";
    case EvidenceKind::EndpointReport:
      return "endpoint";
  }
  return "unknown";
}

Result<EvidenceKind> parse_evidence_kind(std::string_view text) {
  if (text == "counter" || text == "counter-sample") {
    return EvidenceKind::CounterSample;
  }
  if (text == "probe") {
    return EvidenceKind::ProbeReport;
  }
  if (text == "sequence") {
    return EvidenceKind::SequenceReport;
  }
  if (text == "endpoint") {
    return EvidenceKind::EndpointReport;
  }
  if (text == "unknown") {
    return EvidenceKind::Unknown;
  }
  return make_status(StatusCode::InvalidArgument, "unknown evidence kind");
}

std::string_view to_string(CounterScope scope) noexcept {
  switch (scope) {
    case CounterScope::Unknown:
      return "unknown";
    case CounterScope::ReceivedPackets:
      return "received-packets";
    case CounterScope::TransmittedPackets:
      return "transmitted-packets";
    case CounterScope::DroppedPackets:
      return "dropped-packets";
    case CounterScope::DiscardedPackets:
      return "discarded-packets";
    case CounterScope::ErrorPackets:
      return "error-packets";
    case CounterScope::ReceivedBytes:
      return "received-bytes";
    case CounterScope::TransmittedBytes:
      return "transmitted-bytes";
  }
  return "unknown";
}

Result<CounterScope> parse_counter_scope(std::string_view text) {
  if (text == "unknown") {
    return CounterScope::Unknown;
  }
  if (text == "received-packets" || text == "rx-packets") {
    return CounterScope::ReceivedPackets;
  }
  if (text == "transmitted-packets" || text == "tx-packets") {
    return CounterScope::TransmittedPackets;
  }
  if (text == "dropped-packets" || text == "drops") {
    return CounterScope::DroppedPackets;
  }
  if (text == "discarded-packets" || text == "discards") {
    return CounterScope::DiscardedPackets;
  }
  if (text == "error-packets" || text == "errors") {
    return CounterScope::ErrorPackets;
  }
  if (text == "received-bytes" || text == "rx-bytes") {
    return CounterScope::ReceivedBytes;
  }
  if (text == "transmitted-bytes" || text == "tx-bytes") {
    return CounterScope::TransmittedBytes;
  }
  return make_status(StatusCode::InvalidArgument, "unknown counter scope");
}

std::string_view to_string(EndpointRole role) noexcept {
  switch (role) {
    case EndpointRole::Unknown:
      return "unknown";
    case EndpointRole::Sender:
      return "sender";
    case EndpointRole::Receiver:
      return "receiver";
  }
  return "unknown";
}

Result<EndpointRole> parse_endpoint_role(std::string_view text) {
  if (text == "sender" || text == "tx") {
    return EndpointRole::Sender;
  }
  if (text == "receiver" || text == "rx") {
    return EndpointRole::Receiver;
  }
  if (text == "unknown") {
    return EndpointRole::Unknown;
  }
  return make_status(StatusCode::InvalidArgument, "unknown endpoint role");
}

EvidenceKind kind_of(const EvidencePayload& payload) noexcept {
  return std::visit(
      [](const auto& value) -> EvidenceKind {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, CounterSample>) {
          return EvidenceKind::CounterSample;
        } else if constexpr (std::is_same_v<T, ProbeReport>) {
          return EvidenceKind::ProbeReport;
        } else if constexpr (std::is_same_v<T, SequenceReport>) {
          return EvidenceKind::SequenceReport;
        } else {
          return EvidenceKind::EndpointReport;
        }
      },
      payload);
}

MeasurementMethod default_method_for(EvidenceKind kind) noexcept {
  switch (kind) {
    case EvidenceKind::CounterSample:
      return MeasurementMethod::CounterDelta;
    case EvidenceKind::ProbeReport:
      return MeasurementMethod::ProbeRoundTrip;
    case EvidenceKind::SequenceReport:
      return MeasurementMethod::SequenceGap;
    case EvidenceKind::EndpointReport:
      return MeasurementMethod::EndpointComparison;
    case EvidenceKind::Unknown:
      return MeasurementMethod::Unknown;
  }
  return MeasurementMethod::Unknown;
}

std::string describe(const CounterSample& sample) {
  std::string result = "counter=";
  result.append(sample.counter.to_string());
  result.append(" scope=");
  result.append(loss_observatory::to_string(sample.scope));
  result.append(" value=");
  result.append(std::to_string(sample.value));
  result.append(" bits=");
  result.append(std::to_string(sample.width_bits));
  if (!sample.node.is_nil()) {
    result.append(" node=");
    result.append(sample.node.to_string());
  }
  if (!sample.port.is_nil()) {
    result.append(" port=");
    result.append(sample.port.to_string());
  }
  if (!sample.queue.is_nil()) {
    result.append(" queue=");
    result.append(sample.queue.to_string());
  }
  if (!sample.hop.is_nil()) {
    result.append(" hop=");
    result.append(sample.hop.to_string());
  }
  if (!sample.link.is_nil()) {
    result.append(" link=");
    result.append(sample.link.to_string());
  }
  return result;
}

std::string describe(const ProbeReport& report) {
  std::string result = "probe=";
  result.append(report.probe.to_string());
  result.append(" sent=");
  result.append(std::to_string(report.sent));
  result.append(" received=");
  result.append(std::to_string(report.received));
  result.append(" timeout=");
  result.append(std::to_string(report.timed_out));
  if (report.ttl_scoped) {
    result.append(" ttl=");
    result.append(std::to_string(report.ttl));
  }
  if (report.one_way) {
    result.append(" oneway=true");
  }
  return result;
}

std::string describe(const SequenceReport& report) {
  std::string result = "low=";
  result.append(std::to_string(report.lowest_sequence));
  result.append(" high=");
  result.append(std::to_string(report.highest_sequence));
  result.append(" received=");
  result.append(std::to_string(report.received_count));
  if (report.sequence_restart) {
    result.append(" restart=true");
  }
  return result;
}

std::string describe(const EndpointReport& report) {
  std::string result = "role=";
  result.append(loss_observatory::to_string(report.role));
  result.append(" count=");
  result.append(std::to_string(report.count));
  if (!report.node.is_nil()) {
    result.append(" node=");
    result.append(report.node.to_string());
  }
  if (!report.port.is_nil()) {
    result.append(" port=");
    result.append(report.port.to_string());
  }
  if (report.counter_reset) {
    result.append(" reset=true");
  }
  return result;
}

std::string describe(const EvidenceItem& item) {
  // The verb names the payload shape; the measurement method travels as its own
  // key. Both are needed for the line to re-parse into an identical item.
  std::string result(loss_observatory::to_string(kind_of(item.payload)));
  result.append(" method=");
  result.append(loss_observatory::to_string(item.header.method));
  result.append(" source=");
  result.append(item.header.source.to_string());
  result.append(" epoch=");
  result.append(item.header.epoch.to_string());
  result.append(" gen=");
  result.append(item.header.generation.to_string());
  result.append(" id=");
  result.append(item.header.id.to_string());
  result.append(" seq=");
  result.append(std::to_string(item.header.source_sequence.value()));
  result.append(" subject=");
  result.append(item.header.subject.to_string());
  result.append(" class=");
  result.append(loss_observatory::to_string(item.header.granularity));
  result.append(" rev=");
  result.append(item.header.topology_revision.to_string());
  result.append(" observed=");
  result.append(item.header.observed_at.value.to_string());
  result.append(" recv=");
  result.append(item.header.received_at.value.to_string());
  result.push_back(' ');
  result.append(std::visit([](const auto& payload) { return describe(payload); }, item.payload));
  if (!item.header.note.empty()) {
    result.append(" note=");
    result.append(item.header.note);
  }
  return result;
}

bool canonical_less(const EvidenceItem& lhs, const EvidenceItem& rhs) noexcept {
  if (lhs.header.observed_at != rhs.header.observed_at) {
    return lhs.header.observed_at < rhs.header.observed_at;
  }
  if (lhs.header.source != rhs.header.source) {
    return lhs.header.source < rhs.header.source;
  }
  if (lhs.header.source_sequence != rhs.header.source_sequence) {
    return lhs.header.source_sequence < rhs.header.source_sequence;
  }
  return lhs.header.id < rhs.header.id;
}

std::vector<EvidenceItem> canonical_order(std::vector<EvidenceItem> items) {
  std::sort(items.begin(), items.end(),
            [](const EvidenceItem& lhs, const EvidenceItem& rhs) { return canonical_less(lhs, rhs); });
  return items;
}

Status validate(const EvidenceItem& item) {
  if (item.header.id.is_nil()) {
    return make_status(StatusCode::InvalidArgument, "evidence measurement identity is nil");
  }
  if (item.header.source.is_nil()) {
    return make_status(StatusCode::InvalidArgument, "evidence source identity is nil");
  }
  if (item.header.epoch.is_nil()) {
    return make_status(StatusCode::InvalidArgument, "evidence source epoch is nil");
  }
  if (item.header.subject.is_unknown()) {
    return make_status(StatusCode::InvalidArgument, "evidence subject is not identified");
  }
  if (item.header.method == MeasurementMethod::Unknown) {
    return make_status(StatusCode::Unsupported, "evidence does not declare a measurement method");
  }
  if (item.header.granularity == Granularity::Unknown) {
    return make_status(StatusCode::InvalidArgument, "evidence does not declare its granularity");
  }
  const MethodSemantics& semantics = semantics_of(item.header.method);
  if (is_finer_than(item.header.granularity, semantics.max_granularity)) {
    return make_status(StatusCode::InvalidArgument,
                       "evidence claims a granularity finer than its measurement method can support");
  }
  if (item.header.note.size() > kMaxEvidenceNoteBytes) {
    return make_status(StatusCode::InvalidArgument, "evidence note exceeds the configured bound");
  }
  if (item.header.observed_at.is_zero()) {
    return make_status(StatusCode::InvalidArgument, "evidence has no observation instant");
  }
  if (item.header.received_at.is_zero()) {
    return make_status(StatusCode::InvalidArgument, "evidence has no receive instant");
  }

  const EvidenceKind kind = kind_of(item.payload);
  const MeasurementMethod expected = default_method_for(kind);
  if (item.header.method != expected && item.header.method != MeasurementMethod::SyntheticInjection) {
    return make_status(StatusCode::InvalidArgument,
                       "evidence method does not match the payload it carries");
  }

  if (const auto* sample = std::get_if<CounterSample>(&item.payload)) {
    if (sample->counter.is_nil()) {
      return make_status(StatusCode::InvalidArgument, "counter identity is nil");
    }
    if (sample->scope == CounterScope::Unknown) {
      return make_status(StatusCode::InvalidArgument, "counter scope is not declared");
    }
    if (sample->width_bits != 0 && !counter_width_supported(sample->width_bits)) {
      return make_status(StatusCode::InvalidArgument, "counter width is not a supported value");
    }
  } else if (const auto* probe = std::get_if<ProbeReport>(&item.payload)) {
    if (probe->probe.is_nil()) {
      return make_status(StatusCode::InvalidArgument, "probe identity is nil");
    }
  } else if (std::holds_alternative<SequenceReport>(item.payload)) {
    // Structural only: sequence ordering and span plausibility are judged by
    // derivation so they can be reported as ImplausibleEvidence rather than
    // silently discarded here.
  } else if (const auto* endpoint = std::get_if<EndpointReport>(&item.payload)) {
    if (endpoint->role == EndpointRole::Unknown) {
      return make_status(StatusCode::InvalidArgument, "endpoint report does not declare a role");
    }
    if (endpoint->node.is_nil()) {
      return make_status(StatusCode::InvalidArgument, "endpoint report does not identify its node");
    }
  } else {
    return make_status(StatusCode::InvalidArgument, "evidence payload is not recognised");
  }
  return {};
}

}  // namespace loss_observatory
