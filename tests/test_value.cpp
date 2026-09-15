// Fabric Capability Registry test suite: typed capability values.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <array>
#include <vector>

#include "fabric/capability/fabric_capability.hpp"
#include "test_framework.hpp"

using namespace fabric::capability;
using namespace fcr::test;

namespace {

std::vector<std::byte> Encode(const CapabilityValue& value) {
  std::vector<std::byte> bytes;
  ByteWriter writer(bytes);
  value.Encode(writer);
  return bytes;
}

CapabilityValue RoundTrip(const CapabilityValue& value) {
  const std::vector<std::byte> bytes = Encode(value);
  ByteReader reader(bytes);
  auto decoded = CapabilityValue::Decode(reader);
  if (!decoded.HasValue()) FCR_FAIL("decode failed: " + decoded.ErrorString());
  if (!reader.Empty()) FCR_FAIL("decode left trailing bytes");
  return decoded.HasValue() ? decoded.Value() : CapabilityValue{};
}

}  // namespace

FCR_TEST(value, scalar_kinds) {
  const CapabilityValue boolean = CapabilityValue::Boolean(true);
  FCR_CHECK(boolean.Kind() == ValueKind::Boolean);
  FCR_CHECK(boolean.AsBoolean());
  FCR_CHECK_EQ(boolean.ToText(), std::string("true"));
  FCR_CHECK(RoundTrip(boolean) == boolean);

  const CapabilityValue integer = CapabilityValue::Integer(-17);
  FCR_CHECK(integer.Kind() == ValueKind::Integer);
  FCR_CHECK_EQ(integer.ToText(), std::string("-17"));
  FCR_CHECK(RoundTrip(integer) == integer);

  auto quantity = CapabilityValue::QuantityValue(400ull * 1000 * 1000 * 1000, Unit::BitsPerSecond);
  FCR_REQUIRE_OK(quantity);
  FCR_CHECK(quantity.Value().Kind() == ValueKind::Quantity);
  FCR_CHECK_EQ(quantity.Value().ToText(), std::string("400000000000 bit/s"));
  FCR_CHECK(RoundTrip(quantity.Value()) == quantity.Value());

  // A quantity without a unit is rejected: the model never stores an
  // important numeric limit as an untyped number.
  FCR_CHECK_CODE(CapabilityValue::QuantityValue(1, Unit::None), ErrorCode::ValueTypeMismatch);
  FCR_CHECK_CODE(CapabilityValue::QuantityValue(limits::kMaxQuantity + 1, Unit::Count),
                 ErrorCode::ValueOutOfRange);
}

FCR_TEST(value, numeric_sets_are_canonical) {
  const std::array<std::uint64_t, 3> speeds = {400ull * 1000 * 1000 * 1000,
                                               100ull * 1000 * 1000 * 1000,
                                               200ull * 1000 * 1000 * 1000};
  auto set = CapabilityValue::NumericSetValue(Unit::BitsPerSecond, speeds);
  FCR_REQUIRE_OK(set);
  FCR_CHECK_EQ(set.Value().AsNumericSet().values.size(), std::size_t(3));
  FCR_CHECK_EQ(set.Value().AsNumericSet().values[0], 100ull * 1000 * 1000 * 1000);
  FCR_CHECK_EQ(set.Value().AsNumericSet().values[2], 400ull * 1000 * 1000 * 1000);

  // Insertion order does not change the canonical form or its digest.
  const std::array<std::uint64_t, 3> other_order = {200ull * 1000 * 1000 * 1000,
                                                    400ull * 1000 * 1000 * 1000,
                                                    100ull * 1000 * 1000 * 1000};
  auto reordered = CapabilityValue::NumericSetValue(Unit::BitsPerSecond, other_order);
  FCR_REQUIRE_OK(reordered);
  FCR_CHECK(reordered.Value() == set.Value());
  FCR_CHECK(DigestOf(reordered.Value()) == DigestOf(set.Value()));

  const std::array<std::uint64_t, 2> duplicates = {10, 10};
  FCR_CHECK_CODE(CapabilityValue::NumericSetValue(Unit::Count, duplicates),
                 ErrorCode::DuplicateValue);
  FCR_CHECK_CODE(CapabilityValue::NumericSetValue(Unit::Count, {}), ErrorCode::EmptyCollection);
  FCR_CHECK_CODE(CapabilityValue::NumericSetValue(Unit::None, std::array<std::uint64_t, 1>{1}),
                 ErrorCode::ValueTypeMismatch);
  const std::array<std::uint64_t, 1> absurd = {limits::kMaxQuantity + 1};
  FCR_CHECK_CODE(CapabilityValue::NumericSetValue(Unit::Count, absurd), ErrorCode::ValueOutOfRange);
  FCR_CHECK(RoundTrip(set.Value()) == set.Value());
}

FCR_TEST(value, ranges_and_versions) {
  auto range = CapabilityValue::NumericRangeValue(Unit::Bytes, 1500, 9216);
  FCR_REQUIRE_OK(range);
  FCR_CHECK_EQ(range.Value().ToText(), std::string("[1500 B, 9216 B]"));
  FCR_CHECK_CODE(CapabilityValue::NumericRangeValue(Unit::Bytes, 9216, 1500),
                 ErrorCode::ValueContradiction);
  FCR_CHECK(RoundTrip(range.Value()) == range.Value());

  auto minimum = Version::Parse("1.2");
  auto maximum = Version::Parse("2.0.1");
  FCR_REQUIRE_OK(minimum);
  FCR_REQUIRE_OK(maximum);
  auto interval = CapabilityValue::VersionIntervalValue(minimum.Value(), true, maximum.Value(),
                                                        false);
  FCR_REQUIRE_OK(interval);
  FCR_CHECK_EQ(interval.Value().ToText(), std::string("[1.2, 2.0.1)"));
  FCR_CHECK(RoundTrip(interval.Value()) == interval.Value());
  FCR_CHECK_CODE(CapabilityValue::VersionIntervalValue(maximum.Value(), true, minimum.Value(), true),
                 ErrorCode::ValueContradiction);

  FCR_CHECK(Version::Parse("1.2").Value() == Version::Parse("1.2.0").Value());
  FCR_CHECK(Version::Parse("1.2").Value() < Version::Parse("1.10").Value());
  FCR_CHECK_CODE(Version::Parse("1..2"), ErrorCode::MalformedValue);
  FCR_CHECK_CODE(Version::Parse("01.2"), ErrorCode::MalformedValue);
  FCR_CHECK_CODE(Version::Parse("1.2.3.4.5"), ErrorCode::TooManyItems);
  FCR_CHECK_CODE(Version::Parse("1.a"), ErrorCode::MalformedValue);
  FCR_CHECK_CODE(Version::Parse(""), ErrorCode::MalformedValue);
}

FCR_TEST(value, protocol_and_enumeration_sets) {
  const std::array<ProtocolId, 5> protocols = {ProtocolId::Ipv6, ProtocolId::Ethernet,
                                               ProtocolId::Vlan, ProtocolId::Ethernet,
                                               ProtocolId::Ipv4};
  FCR_CHECK_CODE(CapabilityValue::ProtocolSetValue(protocols), ErrorCode::DuplicateValue);
  const std::array<ProtocolId, 3> unique = {ProtocolId::Ipv6, ProtocolId::Ethernet,
                                            ProtocolId::Vlan};
  auto set = CapabilityValue::ProtocolSetValue(unique);
  FCR_REQUIRE_OK(set);
  FCR_CHECK_EQ(set.Value().ToText(), std::string("{ethernet, vlan, ipv6}"));
  FCR_CHECK(RoundTrip(set.Value()) == set.Value());
  const std::array<ProtocolId, 1> unknown = {ProtocolId::Unknown};
  FCR_CHECK_CODE(CapabilityValue::ProtocolSetValue(unknown), ErrorCode::UnknownEnumDomain);
  FCR_CHECK_CODE(CapabilityValue::ProtocolSetValue({}), ErrorCode::EmptyCollection);

  auto domain = EnumDomainId::Parse("fabric.port.fec_mode");
  FCR_REQUIRE_OK(domain);
  const std::array<std::uint32_t, 3> codes = {2, 1, 2};
  FCR_CHECK_CODE(CapabilityValue::EnumerationSetValue(domain.Value(), codes),
                 ErrorCode::DuplicateValue);
  const std::array<std::uint32_t, 2> unique_codes = {2, 1};
  auto enum_set = CapabilityValue::EnumerationSetValue(domain.Value(), unique_codes);
  FCR_REQUIRE_OK(enum_set);
  FCR_CHECK_EQ(enum_set.Value().ToText(), std::string("{rs, base-r}"));
  FCR_CHECK(RoundTrip(enum_set.Value()) == enum_set.Value());
}

FCR_TEST(value, tuples_records_and_opaque_payloads) {
  const std::array<ScalarValue, 2> elements = {ScalarValue::Unsigned(4),
                                               ScalarValue::Unsigned(100ull * 1000 * 1000 * 1000)};
  auto tuple = CapabilityValue::TupleValueOf(elements);
  FCR_REQUIRE_OK(tuple);
  FCR_CHECK_EQ(tuple.Value().ToText(), std::string("(4, 100000000000)"));
  FCR_CHECK(RoundTrip(tuple.Value()) == tuple.Value());
  const std::array<ScalarValue, 1> too_short = {ScalarValue::Unsigned(4)};
  FCR_CHECK_CODE(CapabilityValue::TupleValueOf(too_short), ErrorCode::ValueOutOfRange);

  const std::array<TupleValue, 2> tuples = {TupleValue{{ScalarValue::Unsigned(8),
                                                        ScalarValue::Unsigned(50)}},
                                            TupleValue{{ScalarValue::Unsigned(4),
                                                        ScalarValue::Unsigned(100)}}};
  auto tuple_set = CapabilityValue::TupleSetValue(tuples);
  FCR_REQUIRE_OK(tuple_set);
  FCR_CHECK_EQ(tuple_set.Value().AsTupleSet().tuples.size(), std::size_t(2));
  FCR_CHECK(RoundTrip(tuple_set.Value()) == tuple_set.Value());

  const std::array<RecordField, 2> fields = {
      RecordField{"mode", *ScalarValue::Text("static")},
      RecordField{"depth", ScalarValue::Unsigned(8)}};
  auto record = CapabilityValue::RecordValue(fields);
  FCR_REQUIRE_OK(record);
  FCR_CHECK_EQ(record.Value().ToText(), std::string("{depth=8, mode=static}"));
  FCR_CHECK(RoundTrip(record.Value()) == record.Value());
  const std::array<RecordField, 2> duplicate_fields = {
      RecordField{"mode", ScalarValue::Unsigned(1)}, RecordField{"mode", ScalarValue::Unsigned(2)}};
  FCR_CHECK_CODE(CapabilityValue::RecordValue(duplicate_fields), ErrorCode::DuplicateValue);
  const std::array<RecordField, 1> bad_name = {RecordField{"Mode", ScalarValue::Unsigned(1)}};
  FCR_CHECK_CODE(CapabilityValue::RecordValue(bad_name), ErrorCode::MalformedIdentifier);

  const std::array<std::byte, 3> payload = {std::byte{1}, std::byte{2}, std::byte{3}};
  auto opaque = CapabilityValue::OpaqueValue("application/octet-stream", payload);
  FCR_REQUIRE_OK(opaque);
  FCR_CHECK(RoundTrip(opaque.Value()) == opaque.Value());
  const std::vector<std::byte> oversized(limits::kMaxExtensionPayloadBytes + 1, std::byte{0});
  FCR_CHECK_CODE(CapabilityValue::OpaqueValue("raw", oversized), ErrorCode::PayloadTooLarge);
  FCR_CHECK_CODE(CapabilityValue::OpaqueValue("", payload), ErrorCode::MalformedIdentifier);
}

FCR_TEST(value, decode_rejects_malformed_input) {
  // Unknown kind tag.
  std::vector<std::byte> unknown_tag = {std::byte{99}};
  ByteReader tag_reader(unknown_tag);
  FCR_CHECK_CODE(CapabilityValue::Decode(tag_reader), ErrorCode::MalformedEncoding);

  // Truncated quantity.
  std::vector<std::byte> truncated;
  {
    ByteWriter writer(truncated);
    writer.U8(static_cast<std::uint8_t>(ValueKind::Quantity));
    writer.U32(10);
  }
  ByteReader truncated_reader(truncated);
  FCR_CHECK_CODE(CapabilityValue::Decode(truncated_reader), ErrorCode::MalformedEncoding);

  // Unsorted numeric set is not canonical and must be rejected.
  std::vector<std::byte> unsorted;
  {
    ByteWriter writer(unsorted);
    writer.U8(static_cast<std::uint8_t>(ValueKind::NumericSet));
    writer.U8(static_cast<std::uint8_t>(Unit::Count));
    writer.U32(2);
    writer.U64(9);
    writer.U64(1);
  }
  ByteReader unsorted_reader(unsorted);
  FCR_CHECK_CODE(CapabilityValue::Decode(unsorted_reader), ErrorCode::ValueNotCanonical);

  // Absurd declared cardinality is rejected before allocation.
  std::vector<std::byte> absurd;
  {
    ByteWriter writer(absurd);
    writer.U8(static_cast<std::uint8_t>(ValueKind::NumericSet));
    writer.U8(static_cast<std::uint8_t>(Unit::Count));
    writer.U32(0xFFFFFFFFu);
  }
  ByteReader absurd_reader(absurd);
  FCR_CHECK_CODE(CapabilityValue::Decode(absurd_reader), ErrorCode::TooManyItems);

  CapabilityValue absent;
  FCR_CHECK(absent.IsAbsent());
  FCR_CHECK(absent.Kind() == ValueKind::Absent);
  FCR_CHECK_OK(absent.ValidateCanonical());
  FCR_CHECK(RoundTrip(absent).IsAbsent());
}
