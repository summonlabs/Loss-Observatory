#include "loss_observatory/script.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "loss_observatory/core/checked.hpp"
#include "loss_observatory/report.hpp"

namespace loss_observatory {
namespace {

[[nodiscard]] std::string trim(std::string_view text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
    ++begin;
  }
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
    --end;
  }
  return std::string(text.substr(begin, end - begin));
}

[[nodiscard]] std::vector<std::string> tokenize(std::string_view line) {
  std::vector<std::string> tokens;
  std::size_t index = 0;
  while (index < line.size()) {
    while (index < line.size() && std::isspace(static_cast<unsigned char>(line[index])) != 0) {
      ++index;
    }
    if (index >= line.size()) {
      break;
    }
    const std::size_t begin = index;
    while (index < line.size() && std::isspace(static_cast<unsigned char>(line[index])) == 0) {
      ++index;
    }
    tokens.emplace_back(line.substr(begin, index - begin));
  }
  return tokens;
}

[[nodiscard]] Result<ScriptCommandKind> verb_kind(std::string_view verb) {
  if (verb == "source") {
    return ScriptCommandKind::Source;
  }
  if (verb == "incarnation") {
    return ScriptCommandKind::Incarnation;
  }
  if (verb == "link") {
    return ScriptCommandKind::Link;
  }
  if (verb == "path") {
    return ScriptCommandKind::Path;
  }
  if (verb == "queue") {
    return ScriptCommandKind::Queue;
  }
  if (verb == "flow") {
    return ScriptCommandKind::Flow;
  }
  if (verb == "counter") {
    return ScriptCommandKind::Counter;
  }
  if (verb == "probe") {
    return ScriptCommandKind::Probe;
  }
  if (verb == "sequence") {
    return ScriptCommandKind::Sequence;
  }
  if (verb == "endpoint") {
    return ScriptCommandKind::Endpoint;
  }
  if (verb == "op") {
    return ScriptCommandKind::Operation;
  }
  return make_status(StatusCode::InvalidArgument, "unknown verb");
}

[[nodiscard]] const std::vector<std::string_view>& allowed_keys(ScriptCommandKind kind) {
  static const std::vector<std::string_view> kSource{"id", "name", "kind", "authority"};
  static const std::vector<std::string_view> kIncarnation{"source", "epoch", "name", "at", "active"};
  static const std::vector<std::string_view> kLink{"id", "a", "b", "kind"};
  static const std::vector<std::string_view> kPath{"id", "kind", "rev", "hops"};
  static const std::vector<std::string_view> kQueue{"id", "node", "port", "priority", "kind"};
  static const std::vector<std::string_view> kFlow{"id", "src", "dst", "proto", "path", "gen", "rev"};
  static const std::vector<std::string_view> kCounter{"id",       "source",   "epoch",    "gen",
                                                      "seq",      "counter",  "scope",    "value",
                                                      "bits",     "class",    "subject",  "node",
                                                      "port",     "queue",    "hop",      "link",
                                                      "observed", "recv",     "note",     "method",
                                                      "rev"};
  static const std::vector<std::string_view> kProbe{"id",       "source",   "epoch",    "gen",
                                                    "seq",      "probe",    "sent",     "received",
                                                    "timeout",  "ttl",      "oneway",   "class",
                                                    "flow",     "path",     "hop",      "link",
                                                    "queue",    "observed", "recv",     "note",
                                                    "method",   "rev",      "subject"};
  static const std::vector<std::string_view> kSequence{"id",       "source",   "epoch",   "gen",
                                                       "seq",      "flow",     "path",    "low",
                                                       "high",     "received", "restart", "class",
                                                       "observed", "recv",     "note",    "method",
                                                       "rev",      "subject"};
  static const std::vector<std::string_view> kEndpoint{"id",       "source",   "epoch",    "gen",
                                                       "seq",      "flow",     "role",     "node",
                                                       "port",     "count",    "reset",    "class",
                                                       "observed", "recv",     "note",     "method",
                                                       "rev",      "subject"};
  static const std::vector<std::string_view> kOperation{"op",    "flow", "path",   "at",    "gran",
                                                        "max",   "scope", "format", "out",  "lines"};
  static const std::vector<std::string_view> kNone{};
  switch (kind) {
    case ScriptCommandKind::Source:
      return kSource;
    case ScriptCommandKind::Incarnation:
      return kIncarnation;
    case ScriptCommandKind::Link:
      return kLink;
    case ScriptCommandKind::Path:
      return kPath;
    case ScriptCommandKind::Queue:
      return kQueue;
    case ScriptCommandKind::Flow:
      return kFlow;
    case ScriptCommandKind::Counter:
      return kCounter;
    case ScriptCommandKind::Probe:
      return kProbe;
    case ScriptCommandKind::Sequence:
      return kSequence;
    case ScriptCommandKind::Endpoint:
      return kEndpoint;
    case ScriptCommandKind::Operation:
      return kOperation;
    case ScriptCommandKind::Setting:
    case ScriptCommandKind::Comment:
      break;
  }
  return kNone;
}

template <class IdType>
[[nodiscard]] Result<IdType> typed_identity(const ScriptCommand& command, std::string_view key) {
  const std::string value = command.get(key);
  if (value.empty()) {
    return make_status(StatusCode::InvalidArgument,
                       std::string("missing required key: ") + std::string(key));
  }
  auto raw = canonical_identity(value);
  if (!raw.ok()) {
    return raw.status();
  }
  return IdType::from_value(raw.value());
}

[[nodiscard]] Result<std::uint64_t> optional_u64(const ScriptCommand& command, std::string_view key,
                                                 std::uint64_t fallback) {
  const std::string value = command.get(key);
  if (value.empty()) {
    return fallback;
  }
  return parse_u64(value);
}

[[nodiscard]] Result<bool> optional_bool(const ScriptCommand& command, std::string_view key, bool fallback) {
  const std::string value = command.get(key);
  if (value.empty()) {
    return fallback;
  }
  return parse_bool(value);
}

struct DispatchContext {
  ObservatoryEngine* engine{nullptr};
  Timestamp now{};
  std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t> next_sequence{};
};

[[nodiscard]] Result<SubjectRef> subject_for(const ScriptCommand& command, Granularity& granularity_out) {
  const std::string explicit_subject = command.get("subject");
  const bool has_class = !command.get("class").empty();
  if (has_class) {
    auto parsed = parse_granularity(command.get("class"));
    if (!parsed.ok()) {
      return parsed.status();
    }
    granularity_out = parsed.value();
  }

  if (!explicit_subject.empty()) {
    const std::size_t colon = explicit_subject.find(':');
    if (colon == std::string::npos) {
      return make_status(StatusCode::InvalidArgument, "subject must be written as <kind>:<identity>");
    }
    auto kind = parse_subject_kind(std::string_view(explicit_subject).substr(0, colon));
    if (!kind.ok()) {
      return kind.status();
    }
    auto raw = canonical_identity(std::string_view(explicit_subject).substr(colon + 1));
    if (!raw.ok()) {
      return raw.status();
    }
    if (!has_class) {
      granularity_out = Granularity::Flow;
    }
    switch (kind.value()) {
      case SubjectKind::Flow:
        return SubjectRef::flow(FlowId::from_value(raw.value()));
      case SubjectKind::Path:
        return SubjectRef::path(PathId::from_value(raw.value()));
      case SubjectKind::Hop:
        return SubjectRef::hop(HopId::from_value(raw.value()));
      case SubjectKind::Link:
        return SubjectRef::link(LinkId::from_value(raw.value()));
      case SubjectKind::Queue:
        return SubjectRef::queue(QueueId::from_value(raw.value()));
      case SubjectKind::Source:
        return SubjectRef::source(SourceId::from_value(raw.value()));
      case SubjectKind::Unknown:
        break;
    }
    return make_status(StatusCode::InvalidArgument, "subject kind is not usable for evidence");
  }

  if (!command.get("queue").empty()) {
    auto id = typed_identity<QueueId>(command, "queue");
    if (!id.ok()) {
      return id.status();
    }
    if (!has_class) {
      granularity_out = Granularity::Queue;
    }
    return SubjectRef::queue(id.value());
  }
  if (!command.get("hop").empty()) {
    auto id = typed_identity<HopId>(command, "hop");
    if (!id.ok()) {
      return id.status();
    }
    if (!has_class) {
      granularity_out = Granularity::Hop;
    }
    return SubjectRef::hop(id.value());
  }
  if (!command.get("link").empty()) {
    auto id = typed_identity<LinkId>(command, "link");
    if (!id.ok()) {
      return id.status();
    }
    if (!has_class) {
      granularity_out = Granularity::Link;
    }
    return SubjectRef::link(id.value());
  }
  if (!command.get("flow").empty()) {
    auto id = typed_identity<FlowId>(command, "flow");
    if (!id.ok()) {
      return id.status();
    }
    return SubjectRef::flow(id.value());
  }
  if (!command.get("path").empty()) {
    auto id = typed_identity<PathId>(command, "path");
    if (!id.ok()) {
      return id.status();
    }
    return SubjectRef::path(id.value());
  }
  return make_status(StatusCode::InvalidArgument, "evidence is not bound to any declared subject");
}

[[nodiscard]] Result<Timestamp> evidence_time(const ScriptCommand& command, std::string_view key,
                                              Timestamp fallback) {
  const std::string value = command.get(key);
  if (value.empty()) {
    return fallback;
  }
  return parse_time_or_offset(value, fallback);
}

[[nodiscard]] Result<SequenceId> next_sequence_id(DispatchContext& context, SourceId source, EpochId epoch,
                                                  const ScriptCommand& command) {
  const std::string explicit_value = command.get("seq");
  if (!explicit_value.empty()) {
    auto parsed = parse_u64(explicit_value);
    if (!parsed.ok()) {
      return parsed.status();
    }
    return SequenceId::from_value(parsed.value());
  }
  std::uint64_t& counter = context.next_sequence[std::make_pair(source.value(), epoch.value())];
  ++counter;
  return SequenceId::from_value(counter);
}

}  // namespace

std::string_view to_string(ScriptCommandKind kind) noexcept {
  switch (kind) {
    case ScriptCommandKind::Source:
      return "source";
    case ScriptCommandKind::Incarnation:
      return "incarnation";
    case ScriptCommandKind::Link:
      return "link";
    case ScriptCommandKind::Path:
      return "path";
    case ScriptCommandKind::Queue:
      return "queue";
    case ScriptCommandKind::Flow:
      return "flow";
    case ScriptCommandKind::Counter:
      return "counter";
    case ScriptCommandKind::Probe:
      return "probe";
    case ScriptCommandKind::Sequence:
      return "sequence";
    case ScriptCommandKind::Endpoint:
      return "endpoint";
    case ScriptCommandKind::Operation:
      return "op";
    case ScriptCommandKind::Setting:
      return "set";
    case ScriptCommandKind::Comment:
      return "comment";
  }
  return "comment";
}

bool ScriptCommand::has(std::string_view key) const {
  return fields.find(std::string(key)) != fields.end();
}

std::string ScriptCommand::get(std::string_view key, std::string_view fallback) const {
  const auto it = fields.find(std::string(key));
  return it == fields.end() ? std::string(fallback) : it->second;
}

std::string ScriptDiagnostic::to_string() const {
  std::string result("line ");
  result.append(std::to_string(line));
  result.append(": ");
  result.append(severity);
  result.append(": ");
  result.append(message);
  return result;
}

bool ScriptProgram::ok() const { return error_count() == 0; }

std::size_t ScriptProgram::error_count() const {
  std::size_t count = 0;
  for (const ScriptDiagnostic& diagnostic : diagnostics) {
    if (diagnostic.severity == "error") {
      ++count;
    }
  }
  return count;
}

Result<ScriptProgram> parse_script(std::string_view text, ScriptLimits limits) {
  if (text.size() > limits.max_input_bytes) {
    return make_status(StatusCode::CapacityExceeded, "script input exceeds the configured bound");
  }
  ScriptProgram program{};
  std::size_t line_number = 0;
  std::size_t begin = 0;
  while (begin < text.size()) {
    if (line_number >= limits.max_lines) {
      program.truncated = true;
      program.diagnostics.push_back(
          ScriptDiagnostic{line_number, "error", "line bound reached; the remainder was not parsed"});
      break;
    }
    std::size_t end = text.find('\n', begin);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    std::string_view raw = text.substr(begin, end - begin);
    if (!raw.empty() && raw.back() == '\r') {
      raw.remove_suffix(1);
    }
    ++line_number;
    begin = end + 1;

    if (raw.size() > limits.max_line_bytes) {
      program.diagnostics.push_back(
          ScriptDiagnostic{line_number, "error", "line exceeds the configured byte bound"});
      continue;
    }
    const std::string trimmed = trim(raw);
    if (trimmed.empty() || trimmed[0] == '#') {
      ++program.lines_parsed;
      continue;
    }
    const std::size_t comment = trimmed.find(" #");
    const std::string content = comment == std::string::npos ? trimmed : trim(trimmed.substr(0, comment));
    const std::vector<std::string> tokens = tokenize(content);
    if (tokens.empty()) {
      ++program.lines_parsed;
      continue;
    }
    if (program.commands.size() >= limits.max_commands) {
      program.truncated = true;
      program.diagnostics.push_back(
          ScriptDiagnostic{line_number, "error", "command bound reached; the remainder was not parsed"});
      break;
    }
    auto kind = verb_kind(tokens.front());
    if (!kind.ok()) {
      program.diagnostics.push_back(
          ScriptDiagnostic{line_number, "error", kind.status().message() + ": " + tokens.front()});
      ++program.lines_parsed;
      continue;
    }
    ScriptCommand command{};
    command.kind = kind.value();
    command.line = line_number;
    command.verb = tokens.front();
    command.raw = content;
    bool failed = false;
    for (std::size_t i = 1; i < tokens.size(); ++i) {
      const std::size_t equals = tokens[i].find('=');
      if (equals == std::string::npos || equals == 0) {
        // "op classify" is the documented short form for "op op=classify".
        const bool is_operation_name =
            command.kind == ScriptCommandKind::Operation && command.fields.empty() && i == 1;
        if (is_operation_name) {
          command.fields.emplace("op", tokens[i]);
          continue;
        }
        program.diagnostics.push_back(
            ScriptDiagnostic{line_number, "error", "expected key=value, found: " + tokens[i]});
        failed = true;
        break;
      }
      const std::string key = tokens[i].substr(0, equals);
      const std::string value = tokens[i].substr(equals + 1);
      if (command.fields.size() >= limits.max_fields_per_command) {
        program.diagnostics.push_back(
            ScriptDiagnostic{line_number, "error", "too many fields on one command"});
        failed = true;
        break;
      }
      if (!command.fields.emplace(key, value).second) {
        program.diagnostics.push_back(ScriptDiagnostic{line_number, "error", "duplicate key: " + key});
        failed = true;
        break;
      }
    }
    if (!failed) {
      const std::vector<std::string_view>& allowed = allowed_keys(command.kind);
      for (const auto& field : command.fields) {
        const bool known =
            std::find(allowed.begin(), allowed.end(), std::string_view(field.first)) != allowed.end();
        if (!known) {
          program.diagnostics.push_back(ScriptDiagnostic{
              line_number, "error", "key is not recognised by verb " + command.verb + ": " + field.first});
          failed = true;
          break;
        }
      }
    }
    if (failed) {
      ++program.lines_parsed;
      continue;
    }
    program.commands.push_back(std::move(command));
    ++program.lines_parsed;
  }
  return program;
}

Result<std::uint64_t> parse_u64(std::string_view text) {
  if (text.empty()) {
    return make_status(StatusCode::InvalidArgument, "empty unsigned integer");
  }
  std::uint64_t value = 0;
  for (const char ch : text) {
    if (ch < '0' || ch > '9') {
      return make_status(StatusCode::InvalidArgument, "not an unsigned integer");
    }
    std::uint64_t next = 0;
    if (!checked::mul(value, 10, next)) {
      return make_status(StatusCode::OutOfRange, "unsigned integer overflows");
    }
    if (!checked::add(next, static_cast<std::uint64_t>(ch - '0'), next)) {
      return make_status(StatusCode::OutOfRange, "unsigned integer overflows");
    }
    value = next;
  }
  return value;
}

Result<std::int64_t> parse_i64(std::string_view text) {
  if (text.empty()) {
    return make_status(StatusCode::InvalidArgument, "empty integer");
  }
  bool negative = false;
  if (text.front() == '-') {
    negative = true;
    text.remove_prefix(1);
  } else if (text.front() == '+') {
    text.remove_prefix(1);
  }
  auto magnitude = parse_u64(text);
  if (!magnitude.ok()) {
    return magnitude.status();
  }
  if (magnitude.value() > 9223372036854775807ULL) {
    return make_status(StatusCode::OutOfRange, "integer overflows");
  }
  const auto value = static_cast<std::int64_t>(magnitude.value());
  return negative ? -value : value;
}

Result<bool> parse_bool(std::string_view text) {
  if (text == "true" || text == "1" || text == "yes") {
    return true;
  }
  if (text == "false" || text == "0" || text == "no") {
    return false;
  }
  return make_status(StatusCode::InvalidArgument, "not a boolean");
}

Result<Timestamp> parse_time_or_offset(std::string_view text, Timestamp relative_to) {
  if (text.empty()) {
    return make_status(StatusCode::InvalidArgument, "empty time value");
  }
  if (text.front() == '+' || text.front() == '-') {
    const bool negative = text.front() == '-';
    const std::string_view remainder = text.substr(1);
    std::size_t digits = 0;
    while (digits < remainder.size() && remainder[digits] >= '0' && remainder[digits] <= '9') {
      ++digits;
    }
    if (digits == 0) {
      return make_status(StatusCode::InvalidArgument, "offset has no digits");
    }
    auto magnitude = parse_u64(remainder.substr(0, digits));
    if (!magnitude.ok()) {
      return magnitude.status();
    }
    const std::string_view unit = remainder.substr(digits);
    std::int64_t nanos = 0;
    if (unit == "ns") {
      nanos = static_cast<std::int64_t>(magnitude.value());
    } else if (unit == "us") {
      nanos = static_cast<std::int64_t>(magnitude.value()) * 1000;
    } else if (unit == "ms") {
      nanos = static_cast<std::int64_t>(magnitude.value()) * 1000000;
    } else if (unit == "s" || unit.empty()) {
      nanos = static_cast<std::int64_t>(magnitude.value()) * 1000000000;
    } else {
      return make_status(StatusCode::InvalidArgument, "unknown time unit");
    }
    if (negative) {
      nanos = -nanos;
    }
    return relative_to + Duration::from_nanos(nanos);
  }
  return Timestamp::parse_iso8601(text);
}

Result<Endpoint> parse_endpoint(std::string_view text) {
  const std::size_t colon = text.find(':');
  if (colon == std::string_view::npos) {
    return make_status(StatusCode::InvalidArgument, "endpoint must be written as <node>:<port>");
  }
  auto node = canonical_identity(text.substr(0, colon));
  if (!node.ok()) {
    return node.status();
  }
  auto port = canonical_identity(text.substr(colon + 1));
  if (!port.ok()) {
    return port.status();
  }
  return Endpoint{NodeId::from_value(node.value()), PortId::from_value(port.value())};
}

Result<std::vector<Hop>> parse_hops(std::string_view text) {
  std::vector<Hop> hops;
  std::size_t index = 0;
  std::uint32_t position = 0;
  while (index < text.size()) {
    std::size_t comma = text.find(',', index);
    if (comma == std::string_view::npos) {
      comma = text.size();
    }
    const std::string_view token = text.substr(index, comma - index);
    index = comma + 1;
    if (token.empty()) {
      continue;
    }
    if (token.find("->") != std::string_view::npos) {
      // Hand-written form: hop identity is derived from the canonical hop text.
      const std::size_t arrow = token.find("->");
      auto ingress = parse_endpoint(token.substr(0, arrow));
      if (!ingress.ok()) {
        return ingress.status();
      }
      auto egress = parse_endpoint(token.substr(arrow + 2));
      if (!egress.ok()) {
        return egress.status();
      }
      Hop hop{};
      hop.id = HopId::from_canonical_text(std::string(token));
      hop.index = position;
      hop.node = ingress.value().node;
      hop.ingress_port = ingress.value().port;
      hop.egress_port = egress.value().port;
      hop.ingress_link = LinkId::from_canonical_text(std::string(token.substr(0, arrow)));
      hop.egress_link = LinkId::from_canonical_text(std::string(token.substr(arrow + 2)));
      hops.push_back(hop);
      ++position;
      continue;
    }

    // Exported form: <hop>:<node>:<ingressPort>:<egressPort>:<ingressLink>:<egressLink>
    std::vector<std::string_view> fields;
    std::size_t cursor = 0;
    while (cursor <= token.size()) {
      const std::size_t colon = token.find(':', cursor);
      if (colon == std::string_view::npos) {
        fields.push_back(token.substr(cursor));
        break;
      }
      fields.push_back(token.substr(cursor, colon - cursor));
      cursor = colon + 1;
    }
    if (fields.size() != 6) {
      return make_status(
          StatusCode::InvalidArgument,
          "hop must be <node>:<port>-><node>:<port> or <hop>:<node>:<in>:<out>:<inlink>:<outlink>");
    }
    Hop hop{};
    auto hop_id = canonical_identity(fields[0]);
    auto node = canonical_identity(fields[1]);
    auto ingress_port = canonical_identity(fields[2]);
    auto egress_port = canonical_identity(fields[3]);
    auto ingress_link = canonical_identity(fields[4]);
    auto egress_link = canonical_identity(fields[5]);
    if (!hop_id.ok() || !node.ok() || !ingress_port.ok() || !egress_port.ok() || !ingress_link.ok() ||
        !egress_link.ok()) {
      return make_status(StatusCode::InvalidArgument, "hop identity field is empty");
    }
    hop.id = HopId::from_value(hop_id.value());
    hop.index = position;
    hop.node = NodeId::from_value(node.value());
    hop.ingress_port = PortId::from_value(ingress_port.value());
    hop.egress_port = PortId::from_value(egress_port.value());
    hop.ingress_link = LinkId::from_value(ingress_link.value());
    hop.egress_link = LinkId::from_value(egress_link.value());
    hops.push_back(hop);
    ++position;
  }
  if (hops.empty()) {
    return make_status(StatusCode::InvalidArgument, "path declares no hops");
  }
  return hops;
}

ScriptRunResult apply_script(ObservatoryEngine& engine, const ScriptProgram& program,
                             std::stop_token token) {
  ScriptRunResult run{};
  run.diagnostics = program.diagnostics;
  DispatchContext context{};
  context.engine = &engine;
  context.now = engine.now();

  /// Emits a step result. When the command carries out=<path> the text is
  /// written there as well, so a caller can consume results in another process.
  auto record_impl = [&](const ScriptCommand& command, StatusCode code, std::string detail,
                         std::string output) {
    ScriptStepResult step{};
    step.line = command.line;
    step.verb = command.verb;
    step.code = code;
    step.detail = std::move(detail);
    step.output = std::move(output);
    if (code == StatusCode::Ok) {
      ++run.applied;
    } else {
      ++run.failed;
    }
    run.steps.push_back(std::move(step));
  };

  auto emit = [&](const ScriptCommand& command, std::string output) {
    const std::string out = command.get("out");
    if (out.empty()) {
      record_impl(command, StatusCode::Ok, std::string{}, std::move(output));
      return;
    }
    std::ofstream file(out, std::ios::binary | std::ios::trunc);
    if (!file.good()) {
      record_impl(command, StatusCode::InvalidArgument, "cannot open the output target: " + out,
                  std::string{});
      return;
    }
    file.write(output.data(), static_cast<std::streamsize>(output.size()));
    file.flush();
    if (!file.good()) {
      record_impl(command, StatusCode::Internal, "failed to write the output target", std::string{});
      return;
    }
    record_impl(command, StatusCode::Ok, out, "bytes=" + std::to_string(output.size()));
  };

  auto record = [&](const ScriptCommand& command, StatusCode code, std::string detail, std::string output) {
    record_impl(command, code, std::move(detail), std::move(output));
  };

  for (const ScriptCommand& command : program.commands) {
    if (token.stop_requested()) {
      run.cancelled = true;
      break;
    }
    switch (command.kind) {
      case ScriptCommandKind::Comment:
        continue;
      case ScriptCommandKind::Source: {
        auto id = typed_identity<SourceId>(command, "id");
        auto kind = parse_source_kind(command.get("kind", "unknown"));
        auto authority = parse_source_authority(command.get("authority", "none"));
        if (!id.ok() || !kind.ok() || !authority.ok()) {
          const Status& status =
              !id.ok() ? id.status() : (!kind.ok() ? kind.status() : authority.status());
          record(command, status.code(), status.message(), {});
          continue;
        }
        SourceDescriptor descriptor{};
        descriptor.id = id.value();
        descriptor.name = command.get("name", command.get("id"));
        descriptor.kind = kind.value();
        descriptor.authority = authority.value();
        const Result<UpsertOutcome> outcome = engine.sources().register_source(descriptor, true);
        if (!outcome.ok()) {
          record(command, outcome.code(), outcome.status().message(), {});
          continue;
        }
        record(command, StatusCode::Ok, std::string(to_string(outcome.value())), descriptor.to_string());
        continue;
      }
      case ScriptCommandKind::Incarnation: {
        auto source = typed_identity<SourceId>(command, "source");
        auto at = evidence_time(command, "at", context.now);
        if (!source.ok() || !at.ok()) {
          const Status& status = !source.ok() ? source.status() : at.status();
          record(command, status.code(), status.message(), {});
          continue;
        }
        const std::string epoch_text = command.get("epoch");
        if (!epoch_text.empty()) {
          auto epoch = canonical_identity(epoch_text);
          if (!epoch.ok()) {
            record(command, epoch.code(), epoch.status().message(), {});
            continue;
          }
          auto active = optional_bool(command, "active", false);
          if (!active.ok()) {
            record(command, active.code(), active.status().message(), {});
            continue;
          }
          SourceIncarnation incarnation{};
          incarnation.source = source.value();
          incarnation.epoch = EpochId::from_value(epoch.value());
          incarnation.incarnation = command.get("name", epoch_text);
          incarnation.activated_at = at.value();
          const Result<void> outcome =
              active.value() ? engine.sources().activate_incarnation_with_epoch(
                                   incarnation.source, incarnation.epoch, incarnation.incarnation,
                                   incarnation.activated_at)
                             : engine.sources().restore_incarnation(incarnation);
          if (!outcome.ok()) {
            record(command, outcome.code(), outcome.status().message(), {});
            continue;
          }
          record(command, StatusCode::Ok, active.value() ? "activated" : "restored",
                 active.value() ? "epoch=" + incarnation.epoch.to_string() : incarnation.to_string());
          continue;
        }
        const Result<EpochId> epoch = engine.sources().activate_incarnation(
            source.value(), command.get("name", "default"), at.value());
        if (!epoch.ok()) {
          record(command, epoch.code(), epoch.status().message(), {});
          continue;
        }
        record(command, StatusCode::Ok, {}, "epoch=" + epoch.value().to_string());
        continue;
      }
      case ScriptCommandKind::Link: {
        auto id = typed_identity<LinkId>(command, "id");
        auto a = parse_endpoint(command.get("a"));
        auto b = parse_endpoint(command.get("b"));
        auto kind = parse_link_kind(command.get("kind", "unknown"));
        if (!id.ok() || !a.ok() || !b.ok() || !kind.ok()) {
          const Status& status = !id.ok()   ? id.status()
                                 : !a.ok()  ? a.status()
                                 : !b.ok()  ? b.status()
                                            : kind.status();
          record(command, status.code(), status.message(), {});
          continue;
        }
        Link link{};
        link.id = id.value();
        link.a = a.value();
        link.b = b.value();
        link.kind = kind.value();
        const Result<UpsertResult> outcome = engine.topology().upsert_link(link, true);
        if (!outcome.ok()) {
          record(command, outcome.code(), outcome.status().message(), {});
          continue;
        }
        record(command, StatusCode::Ok, std::string(to_string(outcome.value().outcome)), link.to_string());
        continue;
      }
      case ScriptCommandKind::Path: {
        auto id = typed_identity<PathId>(command, "id");
        auto kind = parse_path_kind(command.get("kind", "unknown"));
        auto hops = parse_hops(command.get("hops"));
        if (!id.ok() || !kind.ok() || !hops.ok()) {
          const Status& status =
              !id.ok() ? id.status() : (!kind.ok() ? kind.status() : hops.status());
          record(command, status.code(), status.message(), {});
          continue;
        }
        Path path{};
        path.id = id.value();
        path.kind = kind.value();
        path.hops = hops.value();
        const std::string rev = command.get("rev");
        if (!rev.empty()) {
          auto revision = canonical_identity(rev);
          if (!revision.ok()) {
            record(command, revision.code(), revision.status().message(), {});
            continue;
          }
          path.revision = RevisionId::from_value(revision.value());
        }
        const Result<UpsertResult> outcome = engine.topology().upsert_path(path, true);
        if (!outcome.ok()) {
          record(command, outcome.code(), outcome.status().message(), {});
          continue;
        }
        const Result<Path> stored_path = engine.topology().find_path(path.id);
        record(command, StatusCode::Ok, std::string(to_string(outcome.value().outcome)),
               stored_path.ok() ? stored_path.value().to_string() : path.to_string());
        continue;
      }
      case ScriptCommandKind::Queue: {
        auto id = typed_identity<QueueId>(command, "id");
        auto node = typed_identity<NodeId>(command, "node");
        auto port = typed_identity<PortId>(command, "port");
        auto priority = typed_identity<PriorityId>(command, "priority");
        auto kind = parse_queue_kind(command.get("kind", "unknown"));
        if (!id.ok() || !node.ok() || !port.ok() || !priority.ok() || !kind.ok()) {
          const Status& status = !id.ok()        ? id.status()
                                 : !node.ok()    ? node.status()
                                 : !port.ok()    ? port.status()
                                 : !priority.ok() ? priority.status()
                                                  : kind.status();
          record(command, status.code(), status.message(), {});
          continue;
        }
        QueueEntity queue{};
        queue.id = id.value();
        queue.node = node.value();
        queue.port = port.value();
        queue.priority = priority.value();
        queue.kind = kind.value();
        const Result<UpsertResult> outcome = engine.topology().upsert_queue(queue, true);
        if (!outcome.ok()) {
          record(command, outcome.code(), outcome.status().message(), {});
          continue;
        }
        record(command, StatusCode::Ok, std::string(to_string(outcome.value().outcome)), queue.to_string());
        continue;
      }
      case ScriptCommandKind::Flow: {
        auto id = typed_identity<FlowId>(command, "id");
        auto source = parse_endpoint(command.get("src"));
        auto destination = parse_endpoint(command.get("dst"));
        auto protocol = typed_identity<ProtocolId>(command, "proto");
        auto path = typed_identity<PathId>(command, "path");
        auto generation = typed_identity<GenerationId>(command, "gen");
        if (!id.ok() || !source.ok() || !destination.ok() || !protocol.ok() || !path.ok() ||
            !generation.ok()) {
          const Status& status = !id.ok()            ? id.status()
                                 : !source.ok()      ? source.status()
                                 : !destination.ok() ? destination.status()
                                 : !protocol.ok()    ? protocol.status()
                                 : !path.ok()        ? path.status()
                                                     : generation.status();
          record(command, status.code(), status.message(), {});
          continue;
        }
        FlowBinding flow{};
        flow.id = id.value();
        flow.source = source.value();
        flow.destination = destination.value();
        flow.protocol = protocol.value();
        flow.path = path.value();
        flow.generation = generation.value();
        const Result<Path> declared = engine.topology().find_path(flow.path);
        if (!declared.ok()) {
          record(command, declared.code(), "flow references an undeclared path", {});
          continue;
        }
        flow.binding_revision = declared.value().revision;
        if (!command.get("rev").empty()) {
          auto revision = canonical_identity(command.get("rev"));
          if (!revision.ok()) {
            record(command, revision.code(), revision.status().message(), {});
            continue;
          }
          flow.binding_revision = RevisionId::from_value(revision.value());
        }
        const Result<UpsertResult> outcome = engine.topology().upsert_flow(flow, true);
        if (!outcome.ok()) {
          record(command, outcome.code(), outcome.status().message(), {});
          continue;
        }
        record(command, StatusCode::Ok, std::string(to_string(outcome.value().outcome)), flow.to_string());
        continue;
      }
      case ScriptCommandKind::Counter:
      case ScriptCommandKind::Probe:
      case ScriptCommandKind::Sequence:
      case ScriptCommandKind::Endpoint: {
        auto source = typed_identity<SourceId>(command, "source");
        auto epoch = typed_identity<EpochId>(command, "epoch");
        auto generation = typed_identity<GenerationId>(command, "gen");
        if (!source.ok() || !epoch.ok() || !generation.ok()) {
          const Status& status =
              !source.ok() ? source.status() : (!epoch.ok() ? epoch.status() : generation.status());
          record(command, status.code(), status.message(), {});
          continue;
        }
        Granularity granularity = Granularity::Unknown;
        auto subject = subject_for(command, granularity);
        if (!subject.ok()) {
          record(command, subject.code(), subject.status().message(), {});
          continue;
        }
        auto sequence = next_sequence_id(context, source.value(), epoch.value(), command);
        if (!sequence.ok()) {
          record(command, sequence.code(), sequence.status().message(), {});
          continue;
        }
        auto observed = evidence_time(command, "observed", context.now);
        // The receive instant is always "recv"; "received" means the delivered
        // count for probe and sequence reports.
        auto received = evidence_time(command, "recv", observed.ok() ? observed.value() : context.now);
        if (!observed.ok() || !received.ok()) {
          const Status& status = !observed.ok() ? observed.status() : received.status();
          record(command, status.code(), status.message(), {});
          continue;
        }
        EvidenceItem item{};
        if (!command.get("rev").empty()) {
          auto revision = canonical_identity(command.get("rev"));
          if (!revision.ok()) {
            record(command, revision.code(), revision.status().message(), {});
            continue;
          }
          item.header.topology_revision = RevisionId::from_value(revision.value());
        }
        item.header.source = source.value();
        item.header.epoch = epoch.value();
        item.header.generation = generation.value();
        item.header.subject = subject.value();
        item.header.granularity = granularity;
        item.header.source_sequence = sequence.value();
        item.header.observed_at.value = observed.value();
        item.header.received_at.value = received.value();
        item.header.note = command.get("note");

        const std::string id_text = command.get("id");
        if (!id_text.empty()) {
          auto id = canonical_identity(id_text);
          if (!id.ok()) {
            record(command, id.code(), id.status().message(), {});
            continue;
          }
          item.header.id = MeasurementId::from_value(id.value());
        } else {
          std::uint64_t mixed = combine_hash(source.value().value(), epoch.value().value());
          mixed = combine_hash(mixed, sequence.value().value());
          item.header.id = MeasurementId::from_value(mixed == 0 ? 1 : mixed);
        }

        if (command.kind == ScriptCommandKind::Counter) {
          auto scope = parse_counter_scope(command.get("scope"));
          auto value = parse_u64(command.get("value"));
          auto bits = optional_u64(command, "bits", 32);
          auto counter = typed_identity<CounterId>(command, "counter");
          if (!scope.ok() || !value.ok() || !bits.ok() || !counter.ok()) {
            const Status& status = !scope.ok()     ? scope.status()
                                   : !value.ok()   ? value.status()
                                   : !bits.ok()    ? bits.status()
                                                   : counter.status();
            record(command, status.code(), status.message(), {});
            continue;
          }
          CounterSample sample{};
          sample.counter = counter.value();
          sample.scope = scope.value();
          sample.value = value.value();
          sample.width_bits = static_cast<std::uint8_t>(bits.value() > 64 ? 64 : bits.value());
          if (!command.get("node").empty()) {
            auto id = typed_identity<NodeId>(command, "node");
            if (!id.ok()) {
              record(command, id.code(), id.status().message(), {});
              continue;
            }
            sample.node = id.value();
          }
          if (!command.get("port").empty()) {
            auto id = typed_identity<PortId>(command, "port");
            if (!id.ok()) {
              record(command, id.code(), id.status().message(), {});
              continue;
            }
            sample.port = id.value();
          }
          if (!command.get("queue").empty()) {
            auto id = typed_identity<QueueId>(command, "queue");
            if (!id.ok()) {
              record(command, id.code(), id.status().message(), {});
              continue;
            }
            sample.queue = id.value();
          }
          if (!command.get("hop").empty()) {
            auto id = typed_identity<HopId>(command, "hop");
            if (!id.ok()) {
              record(command, id.code(), id.status().message(), {});
              continue;
            }
            sample.hop = id.value();
          }
          if (!command.get("link").empty()) {
            auto id = typed_identity<LinkId>(command, "link");
            if (!id.ok()) {
              record(command, id.code(), id.status().message(), {});
              continue;
            }
            sample.link = id.value();
          }
          item.header.method = MeasurementMethod::CounterDelta;
          if (!command.get("method").empty()) {
            auto method = parse_measurement_method(command.get("method"));
            if (!method.ok()) {
              record(command, method.code(), method.status().message(), {});
              continue;
            }
            item.header.method = method.value();
          }
          item.payload = sample;
        } else if (command.kind == ScriptCommandKind::Probe) {
          auto probe = typed_identity<ProbeId>(command, "probe");
          auto sent = optional_u64(command, "sent", 0);
          auto received_count = optional_u64(command, "received", 0);
          auto timeout = optional_u64(command, "timeout", 0);
          auto oneway = optional_bool(command, "oneway", false);
          if (!probe.ok() || !sent.ok() || !received_count.ok() || !timeout.ok() || !oneway.ok()) {
            const Status& status = !probe.ok()            ? probe.status()
                                   : !sent.ok()           ? sent.status()
                                   : !received_count.ok() ? received_count.status()
                                   : !timeout.ok()        ? timeout.status()
                                                          : oneway.status();
            record(command, status.code(), status.message(), {});
            continue;
          }
          ProbeReport report{};
          report.probe = probe.value();
          report.sent = static_cast<std::uint32_t>(sent.value());
          report.received = static_cast<std::uint32_t>(received_count.value());
          report.timed_out = static_cast<std::uint32_t>(timeout.value());
          report.one_way = oneway.value();
          if (!command.get("ttl").empty()) {
            auto ttl = parse_u64(command.get("ttl"));
            if (!ttl.ok()) {
              record(command, ttl.code(), ttl.status().message(), {});
              continue;
            }
            report.ttl_scoped = true;
            report.ttl = static_cast<std::uint8_t>(ttl.value());
          }
          item.header.method = MeasurementMethod::ProbeRoundTrip;
          if (!command.get("method").empty()) {
            auto method = parse_measurement_method(command.get("method"));
            if (!method.ok()) {
              record(command, method.code(), method.status().message(), {});
              continue;
            }
            item.header.method = method.value();
          }
          if (item.header.granularity == Granularity::Unknown) {
            item.header.granularity = Granularity::Path;
          }
          item.payload = report;
        } else if (command.kind == ScriptCommandKind::Sequence) {
          auto low = parse_u64(command.get("low"));
          auto high = parse_u64(command.get("high"));
          auto received_count = parse_u64(command.get("received"));
          auto restart = optional_bool(command, "restart", false);
          if (!low.ok() || !high.ok() || !received_count.ok() || !restart.ok()) {
            const Status& status = !low.ok()              ? low.status()
                                   : !high.ok()           ? high.status()
                                   : !received_count.ok() ? received_count.status()
                                                          : restart.status();
            record(command, status.code(), status.message(), {});
            continue;
          }
          SequenceReport report{};
          report.lowest_sequence = low.value();
          report.highest_sequence = high.value();
          report.received_count = received_count.value();
          report.sequence_restart = restart.value();
          item.header.method = MeasurementMethod::SequenceGap;
          if (!command.get("method").empty()) {
            auto method = parse_measurement_method(command.get("method"));
            if (!method.ok()) {
              record(command, method.code(), method.status().message(), {});
              continue;
            }
            item.header.method = method.value();
          }
          if (item.header.granularity == Granularity::Unknown) {
            item.header.granularity = Granularity::Flow;
          }
          item.payload = report;
        } else {
          auto role = parse_endpoint_role(command.get("role"));
          auto count = parse_u64(command.get("count"));
          auto reset = optional_bool(command, "reset", false);
          if (!role.ok() || !count.ok() || !reset.ok()) {
            const Status& status =
                !role.ok() ? role.status() : (!count.ok() ? count.status() : reset.status());
            record(command, status.code(), status.message(), {});
            continue;
          }
          EndpointReport report{};
          report.role = role.value();
          report.count = count.value();
          report.counter_reset = reset.value();
          if (!command.get("node").empty()) {
            auto id = typed_identity<NodeId>(command, "node");
            if (!id.ok()) {
              record(command, id.code(), id.status().message(), {});
              continue;
            }
            report.node = id.value();
          }
          if (!command.get("port").empty()) {
            auto id = typed_identity<PortId>(command, "port");
            if (!id.ok()) {
              record(command, id.code(), id.status().message(), {});
              continue;
            }
            report.port = id.value();
          }
          item.header.method = MeasurementMethod::EndpointComparison;
          if (!command.get("method").empty()) {
            auto method = parse_measurement_method(command.get("method"));
            if (!method.ok()) {
              record(command, method.code(), method.status().message(), {});
              continue;
            }
            item.header.method = method.value();
          }
          if (item.header.granularity == Granularity::Unknown) {
            item.header.granularity = Granularity::Flow;
          }
          item.payload = report;
        }

        const Result<IngestOutcome> outcome = engine.ingest(std::move(item));
        if (!outcome.ok()) {
          record(command, outcome.code(), outcome.status().message(), {});
          continue;
        }
        if (!outcome.value().accepted) {
          record(command, StatusCode::InvalidArgument,
                 std::string(to_string(outcome.value().reason)) + ": " + outcome.value().detail, {});
          continue;
        }
        record(command, StatusCode::Ok, std::string(to_string(outcome.value().fence)),
               outcome.value().id.to_string());
        continue;
      }
      case ScriptCommandKind::Operation: {
        const std::string operation = command.get("op");
        if (operation == "classify") {
          auto flow = typed_identity<FlowId>(command, "flow");
          auto at = evidence_time(command, "at", engine.now());
          if (!flow.ok() || !at.ok()) {
            const Status& status = !flow.ok() ? flow.status() : at.status();
            record(command, status.code(), status.message(), {});
            continue;
          }
          const Result<Classification> result = engine.classify_flow(flow.value(), at.value());
          if (!result.ok()) {
            record(command, result.code(), result.status().message(), {});
            continue;
          }
          std::string output = render_classification(result.value());
          output.push_back('\n');
          output.append(render_attribution(result.value().attribution));
          for (const ReasonCode reason : result.value().reasons) {
            output.append("reason=");
            output.append(loss_observatory::to_string(reason));
            output.push_back('\n');
          }
          emit(command, std::move(output));
          continue;
        }
        if (operation == "localize") {
          auto flow = typed_identity<FlowId>(command, "flow");
          auto at = evidence_time(command, "at", engine.now());
          if (!flow.ok() || !at.ok()) {
            const Status& status = !flow.ok() ? flow.status() : at.status();
            record(command, status.code(), status.message(), {});
            continue;
          }
          LocalizationRequest request{};
          request.flow = flow.value();
          request.at = at.value();
          if (!command.get("gran").empty()) {
            auto granularity = parse_granularity(command.get("gran"));
            if (!granularity.ok()) {
              record(command, granularity.code(), granularity.status().message(), {});
              continue;
            }
            request.requested_max_granularity = granularity.value();
          }
          auto max_segments = optional_u64(command, "max", 0);
          if (!max_segments.ok()) {
            record(command, max_segments.code(), max_segments.status().message(), {});
            continue;
          }
          request.max_segments = static_cast<std::size_t>(max_segments.value());
          const Result<LocalizationResult> result = engine.localize(request);
          if (!result.ok()) {
            record(command, result.code(), result.status().message(), {});
            continue;
          }
          emit(command, render_localization(result.value()));
          continue;
        }
        if (operation == "history") {
          EpisodeQuery query{};
          if (!command.get("flow").empty()) {
            auto flow = typed_identity<FlowId>(command, "flow");
            if (!flow.ok()) {
              record(command, flow.code(), flow.status().message(), {});
              continue;
            }
            query.flow = flow.value();
          }
          auto max_results = optional_u64(command, "max", 0);
          if (!max_results.ok()) {
            record(command, max_results.code(), max_results.status().message(), {});
            continue;
          }
          query.max_results = static_cast<std::size_t>(max_results.value());
          const Result<EpisodeQueryResult> result = engine.history(query);
          if (!result.ok()) {
            record(command, result.code(), result.status().message(), {});
            continue;
          }
          std::string output = result.value().to_string();
          output.push_back('\n');
          for (const LossEpisode& episode : result.value().episodes) {
            output.append(render_episode(episode));
            output.push_back('\n');
          }
          emit(command, std::move(output));
          continue;
        }
        if (operation == "explain") {
          ExplainRequest request{};
          if (!command.get("flow").empty()) {
            auto flow = typed_identity<FlowId>(command, "flow");
            if (!flow.ok()) {
              record(command, flow.code(), flow.status().message(), {});
              continue;
            }
            request.subject = SubjectRef::flow(flow.value());
          } else if (!command.get("path").empty()) {
            auto path = typed_identity<PathId>(command, "path");
            if (!path.ok()) {
              record(command, path.code(), path.status().message(), {});
              continue;
            }
            request.subject = SubjectRef::path(path.value());
          } else {
            record(command, StatusCode::InvalidArgument, "explain requires flow= or path=", {});
            continue;
          }
          auto at = evidence_time(command, "at", engine.now());
          if (!at.ok()) {
            record(command, at.code(), at.status().message(), {});
            continue;
          }
          request.at = at.value();
          if (!command.get("gran").empty()) {
            auto granularity = parse_granularity(command.get("gran"));
            if (!granularity.ok()) {
              record(command, granularity.code(), granularity.status().message(), {});
              continue;
            }
            request.requested_max_granularity = granularity.value();
          }
          auto lines = optional_u64(command, "lines", 0);
          if (!lines.ok()) {
            record(command, lines.code(), lines.status().message(), {});
            continue;
          }
          request.max_lines = static_cast<std::size_t>(lines.value());
          const Result<Explanation> result = engine.explain(request);
          if (!result.ok()) {
            record(command, result.code(), result.status().message(), {});
            continue;
          }
          emit(command, result.value().text);
          continue;
        }
        if (operation == "export") {
          ExportRequest request{};
          if (!command.get("scope").empty()) {
            auto scope = parse_export_scope(command.get("scope"));
            if (!scope.ok()) {
              record(command, scope.code(), scope.status().message(), {});
              continue;
            }
            request.scope = scope.value();
          }
          if (!command.get("format").empty()) {
            auto format = parse_export_format(command.get("format"));
            if (!format.ok()) {
              record(command, format.code(), format.status().message(), {});
              continue;
            }
            request.format = format.value();
          }
          if (!command.get("flow").empty()) {
            auto flow = typed_identity<FlowId>(command, "flow");
            if (!flow.ok()) {
              record(command, flow.code(), flow.status().message(), {});
              continue;
            }
            request.flow = flow.value();
          }
          auto max_items = optional_u64(command, "max", 0);
          if (!max_items.ok()) {
            record(command, max_items.code(), max_items.status().message(), {});
            continue;
          }
          if (max_items.value() > 0) {
            request.max_items = static_cast<std::size_t>(max_items.value());
          }
          const Result<ExportBundle> result = engine.export_bundle(request);
          if (!result.ok()) {
            record(command, result.code(), result.status().message(), {});
            continue;
          }
          const std::string out = command.get("out");
          if (!out.empty()) {
            std::ofstream file(out, std::ios::binary | std::ios::trunc);
            if (!file.good()) {
              record(command, StatusCode::InvalidArgument, "cannot open the export target: " + out, {});
              continue;
            }
            if (request.format == ExportFormat::Binary) {
              file.write(reinterpret_cast<const char*>(result.value().binary.data()),
                         static_cast<std::streamsize>(result.value().binary.size()));
            } else {
              file.write(result.value().text.data(),
                         static_cast<std::streamsize>(result.value().text.size()));
            }
            file.flush();
            if (!file.good()) {
              record(command, StatusCode::Internal, "failed to write the export target", {});
              continue;
            }
            record(command, StatusCode::Ok, out, result.value().to_string());
            continue;
          }
          record(command, StatusCode::Ok, {}, result.value().text);
          continue;
        }
        record(command, StatusCode::InvalidArgument, "unknown operation: " + operation, {});
        continue;
      }
      case ScriptCommandKind::Setting:
        record(command, StatusCode::Unsupported, "set is not part of this language revision", {});
        continue;
    }
  }
  return run;
}

std::size_t ScriptRunResult::diagnostic_errors() const noexcept {
  std::size_t count = 0;
  for (const ScriptDiagnostic& diagnostic : diagnostics) {
    if (diagnostic.severity == "error") {
      ++count;
    }
  }
  return count;
}

std::string ScriptRunResult::render() const {
  std::string result;
  for (const ScriptStepResult& step : steps) {
    result.append("line ");
    result.append(std::to_string(step.line));
    result.push_back(' ');
    result.append(step.verb);
    result.append(" -> ");
    result.append(loss_observatory::to_string(step.code));
    if (!step.detail.empty()) {
      result.append(" (");
      result.append(step.detail);
      result.push_back(')');
    }
    if (!step.output.empty()) {
      result.push_back('\n');
      result.append(step.output);
      if (result.back() != '\n') {
        result.push_back('\n');
      }
    } else {
      result.push_back('\n');
    }
  }
  for (const ScriptDiagnostic& diagnostic : diagnostics) {
    result.append(diagnostic.to_string());
    result.push_back('\n');
  }
  return result;
}

}  // namespace loss_observatory
