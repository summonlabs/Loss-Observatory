#include <limits>
#include <string>
#include <vector>

#include "fixtures.hpp"
#include "loss_observatory/core/bounded.hpp"
#include "loss_observatory/core/bytes.hpp"
#include "loss_observatory/core/checked.hpp"
#include "loss_observatory/core/hash.hpp"
#include "loss_observatory/model/ids.hpp"

using namespace loss_observatory;

LO_TEST(checked, add_and_sub_detect_overflow) {
  std::uint64_t out = 0;
  LO_CHECK(checked::add(1, 2, out));
  LO_CHECK_EQ(out, 3ULL);
  LO_CHECK(!checked::add(std::numeric_limits<std::uint64_t>::max(), 1, out));
  LO_CHECK(checked::sub(10, 4, out));
  LO_CHECK_EQ(out, 6ULL);
  LO_CHECK(!checked::sub(4, 10, out));
}

LO_TEST(checked, mul_div_and_narrow) {
  std::uint64_t out = 0;
  LO_CHECK(checked::mul(1000, 1000, out));
  LO_CHECK_EQ(out, 1000000ULL);
  LO_CHECK(!checked::mul(std::numeric_limits<std::uint64_t>::max(), 2, out));
  LO_CHECK(!checked::div(1, 0, out));
  LO_CHECK(checked::div(9, 3, out));
  LO_CHECK_EQ(out, 3ULL);

  std::uint32_t small = 0;
  LO_CHECK(checked::narrow<std::uint32_t>(std::uint64_t{7}, small));
  LO_CHECK_EQ(small, 7U);
  LO_CHECK(!checked::narrow<std::uint32_t>(std::uint64_t{1} << 40, small));
  std::uint8_t tiny = 0;
  LO_CHECK(!checked::narrow<std::uint8_t>(-1, tiny));
  LO_CHECK(checked::narrow<std::uint8_t>(200, tiny));
}

LO_TEST(checked, ratio_basis_points_is_exact_and_bounded) {
  std::uint32_t ratio = 0;
  LO_CHECK(checked::ratio_basis_points(0, 100, ratio));
  LO_CHECK_EQ(ratio, 0U);
  LO_CHECK(checked::ratio_basis_points(1, 100, ratio));
  LO_CHECK_EQ(ratio, 100U);
  LO_CHECK(checked::ratio_basis_points(1, 3, ratio));
  LO_CHECK_EQ(ratio, 3333U);
  LO_CHECK(checked::ratio_basis_points(5, 5, ratio));
  LO_CHECK_EQ(ratio, 10000U);
  LO_CHECK(!checked::ratio_basis_points(1, 0, ratio));
  // A numerator larger than the denominator can never exceed 100%.
  LO_CHECK(checked::ratio_basis_points(1000, 5, ratio));
  LO_CHECK_EQ(ratio, 10000U);
}

LO_TEST(checked, saturating_sum_reports_saturation) {
  checked::SaturatingSum sum{};
  sum.add(std::numeric_limits<std::uint64_t>::max());
  LO_CHECK(!sum.saturated());
  sum.add(1);
  LO_CHECK(sum.saturated());
  LO_CHECK_EQ(sum.value(), std::numeric_limits<std::uint64_t>::max());
}

LO_TEST(bytes, writer_and_reader_round_trip) {
  std::vector<std::uint8_t> buffer(64, 0);
  ByteWriter writer{MutableByteSpan{buffer.data(), buffer.size()}};
  writer.u8(0x12);
  writer.u16(0x3456);
  writer.u32(0x789ABCDEU);
  writer.u64(0x0123456789ABCDEFULL);
  writer.i64(-42);
  writer.text("hello");
  LO_CHECK(writer.ok());
  LO_REQUIRE(writer.size() < buffer.size());
  buffer.resize(writer.size());

  ByteReader reader{ByteSpan{buffer.data(), buffer.size()}};
  std::uint8_t u8_value = 0;
  std::uint16_t u16_value = 0;
  std::uint32_t u32_value = 0;
  std::uint64_t u64_value = 0;
  std::int64_t i64_value = 0;
  std::string text;
  LO_CHECK(reader.u8(u8_value));
  LO_CHECK_EQ(u8_value, 0x12U);
  LO_CHECK(reader.u16(u16_value));
  LO_CHECK_EQ(u16_value, 0x3456U);
  LO_CHECK(reader.u32(u32_value));
  LO_CHECK_EQ(u32_value, 0x789ABCDEU);
  LO_CHECK(reader.u64(u64_value));
  LO_CHECK_EQ(u64_value, 0x0123456789ABCDEFULL);
  LO_CHECK(reader.i64(i64_value));
  LO_CHECK_EQ(i64_value, -42);
  LO_CHECK(reader.text(text, 64));
  LO_CHECK_EQ(text, std::string("hello"));
  LO_CHECK(reader.at_end());
}

LO_TEST(bytes, writer_latches_overflow_and_reader_refuses_truncation) {
  std::vector<std::uint8_t> small(4, 0);
  ByteWriter writer{MutableByteSpan{small.data(), small.size()}};
  writer.u64(1);
  LO_CHECK(!writer.ok());
  LO_CHECK(writer.overflowed());
  // Once the flag is latched nothing further is written and the offset never
  // moves past the end of the buffer.
  const std::size_t after_overflow = writer.size();
  LO_CHECK(after_overflow <= small.size());
  writer.u8(1);
  writer.text("still refused");
  LO_CHECK_EQ(writer.size(), after_overflow);

  const std::uint8_t two[2] = {1, 2};
  ByteReader reader{ByteSpan{two, 2}};
  std::uint32_t value = 0;
  LO_CHECK(!reader.u32(value));
  // A refused read never advances past the end of the buffer.
  LO_CHECK(reader.offset() <= 2ULL);
  std::uint8_t byte = 0;
  LO_CHECK(reader.skip(8) == false);
}

LO_TEST(bytes, crc32c_matches_the_known_check_value) {
  const std::string text = "123456789";
  const auto* data = reinterpret_cast<const std::uint8_t*>(text.data());
  LO_CHECK_EQ(crc32c(ByteSpan{data, text.size()}), 0xE3069283U);

  // Incremental extension must equal a single pass over the concatenation.
  const std::string first = "1234";
  const std::string second = "56789";
  const auto* first_data = reinterpret_cast<const std::uint8_t*>(first.data());
  const auto* second_data = reinterpret_cast<const std::uint8_t*>(second.data());
  const std::uint32_t incremental =
      crc32c_extend(crc32c(ByteSpan{first_data, first.size()}), ByteSpan{second_data, second.size()});
  LO_CHECK_EQ(incremental, 0xE3069283U);
}

LO_TEST(bytes, hex_round_trip_and_rejects_malformed_text) {
  const std::uint8_t raw[4] = {0x00, 0x0F, 0xA5, 0xFF};
  const std::string hex = to_hex(ByteSpan{raw, 4});
  LO_CHECK_EQ(hex, std::string("000fa5ff"));
  auto parsed = from_hex(hex);
  LO_REQUIRE(parsed.ok());
  LO_CHECK_EQ(parsed.value().size(), 4ULL);
  LO_CHECK_EQ(parsed.value()[2], 0xA5U);
  LO_CHECK(!from_hex("abc").ok());
  LO_CHECK(!from_hex("zz").ok());
}

LO_TEST(identity, hex_format_and_strict_parsing) {
  LO_CHECK_EQ(hex_u64(0), std::string("0000000000000000"));
  LO_CHECK_EQ(hex_u64(0xDEADBEEFULL), std::string("00000000deadbeef"));
  auto parsed = parse_hex_u64("00000000deadbeef");
  LO_REQUIRE(parsed.ok());
  LO_CHECK_EQ(parsed.value(), 0xDEADBEEFULL);
  LO_CHECK(!parse_hex_u64("deadbeef").ok());
  LO_CHECK(!parse_hex_u64("00000000deadbeeg").ok());
  LO_CHECK(!parse_hex_u64("").ok());
}

LO_TEST(identity, typed_identities_are_distinct_and_comparable) {
  const FlowId flow = FlowId::from_canonical_text("flow/a");
  const FlowId same = FlowId::from_canonical_text("flow/a");
  const FlowId other = FlowId::from_canonical_text("flow/b");
  LO_CHECK(flow == same);
  LO_CHECK(flow != other);
  LO_CHECK(flow < other || other < flow);
  LO_CHECK(!flow.is_nil());
  LO_CHECK(FlowId{}.is_nil());
  LO_CHECK_EQ(flow.to_string().size(), 16ULL);
  auto reparsed = FlowId::parse(flow.to_string());
  LO_REQUIRE(reparsed.ok());
  LO_CHECK(reparsed.value() == flow);
}

LO_TEST(identity, canonical_identity_prefers_literal_hex) {
  auto literal = canonical_identity("00000000deadbeef");
  LO_REQUIRE(literal.ok());
  LO_CHECK_EQ(literal.value(), 0xDEADBEEFULL);
  auto symbolic = canonical_identity("leaf-a");
  LO_REQUIRE(symbolic.ok());
  LO_CHECK_EQ(symbolic.value(), fnv1a64("leaf-a"));
  LO_CHECK(!canonical_identity("").ok());
}

LO_TEST(identity, subject_ref_keeps_its_class) {
  const FlowId flow = FlowId::from_canonical_text("flow/x");
  const SubjectRef ref = SubjectRef::flow(flow);
  LO_CHECK(ref.kind() == SubjectKind::Flow);
  LO_REQUIRE(ref.as_flow().ok());
  LO_CHECK(ref.as_flow().value() == flow);
  LO_CHECK(!ref.as_path().ok());
  LO_CHECK(!ref.as_hop().ok());
  LO_CHECK(ref.to_string().rfind("flow:", 0) == 0);
  LO_CHECK(SubjectRef{}.is_unknown());
}

LO_TEST(granularity, ordering_matches_fineness) {
  LO_CHECK(is_finer_than(Granularity::Queue, Granularity::Link));
  LO_CHECK(!is_finer_than(Granularity::Flow, Granularity::Path));
  LO_CHECK(coarser_of(Granularity::Queue, Granularity::Path) == Granularity::Path);
  LO_CHECK(finer_of(Granularity::Unknown, Granularity::Hop) == Granularity::Hop);
  LO_CHECK(coarser_of(Granularity::Unknown, Granularity::Hop) == Granularity::Unknown);
}

LO_TEST(time, iso8601_round_trip_and_parsing_errors) {
  const Timestamp instant = Timestamp::from_unix_nanos(1767225600123456789LL);
  const std::string text = instant.to_string();
  LO_CHECK_EQ(text, std::string("2026-01-01T00:00:00.123456789Z"));
  auto parsed = Timestamp::parse_iso8601(text);
  LO_REQUIRE(parsed.ok());
  LO_CHECK(parsed.value() == instant);

  auto epoch = Timestamp::parse_iso8601("1970-01-01T00:00:00Z");
  LO_REQUIRE(epoch.ok());
  LO_CHECK(epoch.value().is_zero());

  LO_CHECK(!Timestamp::parse_iso8601("not-a-time").ok());
  LO_CHECK(!Timestamp::parse_iso8601("2026-13-01T00:00:00Z").ok());
  LO_CHECK(!Timestamp::parse_iso8601("2026-01-01T00:00:00Ztrailing").ok());
  LO_CHECK(!Timestamp::parse_iso8601("2026-01-01T00:00:00.").ok());
}

LO_TEST(time, pre_epoch_instants_are_representable) {
  const Timestamp before = Timestamp::from_unix_seconds(-86400);
  LO_CHECK_EQ(before.to_string(), std::string("1969-12-31T00:00:00.000000000Z"));
  auto parsed = Timestamp::parse_iso8601("1969-12-31T00:00:00.000000000Z");
  LO_REQUIRE(parsed.ok());
  LO_CHECK(parsed.value() == before);
}

LO_TEST(time, manual_clock_is_monotonic_under_advance) {
  ManualClock clock{Timestamp::from_unix_seconds(100)};
  LO_CHECK_EQ(clock.now().unix_nanos(), 100000000000LL);
  clock.advance(Duration::from_millis(1500));
  LO_CHECK_EQ(clock.now().unix_nanos(), 101500000000LL);
  LO_CHECK_EQ((Timestamp::from_unix_seconds(10) - Timestamp::from_unix_seconds(4)).nanos(), 6000000000LL);
}

LO_TEST(bounded, queue_bounds_and_reports_rejections) {
  BoundedQueue<int> queue{2};
  LO_CHECK(queue.try_push(1) == PushOutcome::Accepted);
  LO_CHECK(queue.try_push(2) == PushOutcome::Accepted);
  LO_CHECK(queue.try_push(3) == PushOutcome::RejectedFull);
  int value = 0;
  LO_CHECK(queue.try_pop(value));
  LO_CHECK_EQ(value, 1);
  LO_CHECK(queue.try_push(3) == PushOutcome::Accepted);
  queue.close();
  LO_CHECK(queue.size() == 0ULL);
  LO_CHECK(queue.try_push(4) == PushOutcome::RejectedClosed);
  const QueueStats stats = queue.stats();
  LO_CHECK_EQ(stats.rejected_full, 1ULL);
  LO_CHECK_EQ(stats.rejected_closed, 1ULL);
  LO_CHECK_EQ(stats.pushed, 3ULL);
  LO_CHECK_EQ(stats.high_water, 2ULL);
}

LO_TEST(bounded, queue_pop_honours_a_stop_token) {
  BoundedQueue<int> queue{4};
  std::stop_source source{};
  source.request_stop();
  int value = 0;
  LO_CHECK(!queue.pop(value, source.get_token()));
  LO_CHECK(queue.push(7, source.get_token()) == PushOutcome::Accepted);
  LO_CHECK(queue.pop(value, source.get_token()));
  LO_CHECK_EQ(value, 7);
}

LO_TEST(bounded, ring_history_counts_evictions) {
  RingHistory<int> history{3};
  for (int i = 0; i < 5; ++i) {
    LO_CHECK(history.push(i));
  }
  LO_CHECK_EQ(history.size(), 3ULL);
  LO_CHECK_EQ(history.pushed(), 5ULL);
  LO_CHECK_EQ(history.evicted(), 2ULL);
  const std::vector<int> snapshot = history.snapshot();
  LO_REQUIRE(snapshot.size() == 3ULL);
  LO_CHECK_EQ(snapshot.front(), 2);
  LO_CHECK_EQ(snapshot.back(), 4);

  RingHistory<int> none{0};
  LO_CHECK(!none.push(1));
  LO_CHECK_EQ(none.dropped(), 1ULL);
}

LO_TEST(bounded, bound_notes_deduplicate_by_kind_and_subject) {
  BoundNotes notes{};
  notes.add(BoundKind::EvidenceItems, 10, 11, "flow:a");
  notes.add(BoundKind::EvidenceItems, 10, 12, "flow:a");
  notes.add(BoundKind::EvidenceItems, 10, 12, "flow:b");
  LO_CHECK_EQ(notes.size(), 2ULL);
  LO_CHECK(!notes.truncated());
  for (std::size_t i = 0; i < 200; ++i) {
    notes.add(BoundKind::ResultSet, 1, 1, "s" + std::to_string(i));
  }
  LO_CHECK(notes.truncated());
  LO_CHECK(notes.size() <= 64ULL);
}
