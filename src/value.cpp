// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/capability/value.hpp"

#include <algorithm>
#include "fabric/capability/schema.hpp"
#include <array>
#include <cstdio>

namespace fabric::capability {
namespace {

struct UnitNameEntry {
  Unit unit;
  std::string_view name;
  std::string_view suffix;
};

constexpr std::array<UnitNameEntry, 13> kUnitNames = {{
    {Unit::None, "none", ""},
    {Unit::BitsPerSecond, "bit/s", "bit/s"},
    {Unit::Bytes, "B", "B"},
    {Unit::BytesPerSecond, "B/s", "B/s"},
    {Unit::Count, "count", "count"},
    {Unit::Packets, "packets", "packets"},
    {Unit::Nanoseconds, "ns", "ns"},
    {Unit::Microseconds, "us", "us"},
    {Unit::Milliseconds, "ms", "ms"},
    {Unit::Percent, "percent", "percent"},
    {Unit::DecibelMilliwatts, "dBm", "dBm"},
    {Unit::MillidegreesCelsius, "mC", "mC"},
    {Unit::Nanometers, "nm", "nm"},
}};

struct ProtocolEntry {
  ProtocolId protocol;
  std::string_view name;
};

constexpr std::array<ProtocolEntry, 21> kProtocolNames = {{
    {ProtocolId::Unknown, "unknown"},
    {ProtocolId::Ethernet, "ethernet"},
    {ProtocolId::Ipv4, "ipv4"},
    {ProtocolId::Ipv6, "ipv6"},
    {ProtocolId::Vlan, "vlan"},
    {ProtocolId::Mpls, "mpls"},
    {ProtocolId::Vxlan, "vxlan"},
    {ProtocolId::Geneve, "geneve"},
    {ProtocolId::Srv6, "srv6"},
    {ProtocolId::Tcp, "tcp"},
    {ProtocolId::Udp, "udp"},
    {ProtocolId::RoceV1, "roce-v1"},
    {ProtocolId::RoceV2, "roce-v2"},
    {ProtocolId::Iwarp, "iwarp"},
    {ProtocolId::InfiniBand, "infiniband"},
    {ProtocolId::Ptp, "ptp"},
    {ProtocolId::NvmeOf, "nvme-of"},
    {ProtocolId::FibreChannel, "fibre-channel"},
    {ProtocolId::Fcoe, "fcoe"},
    {ProtocolId::Lacp, "lacp"},
    {ProtocolId::LacpPdu, "lacp-pdu"},
}};

Status CanonicalFailure(ErrorCode code, std::string_view message, std::string_view detail = {}) {
  return Status::Failure(code, std::string(message), std::string(detail));
}

Outcome<void> RequireQuantityUnit(Unit unit, std::string_view what) {
  if (!IsQuantityUnit(unit)) {
    return CanonicalFailure(ErrorCode::ValueTypeMismatch,
                            std::string(what) + " requires a quantity unit", UnitName(unit));
  }
  return Status::Success();
}

std::size_t WordsForBits(std::uint32_t bits) {
  return (static_cast<std::size_t>(bits) + 63u) / 64u;
}

bool BitsAboveCountAreClear(const BitSet& set) {
  const std::size_t words = WordsForBits(set.bit_count);
  if (set.words.size() != words) return false;
  if (set.bit_count % 64u == 0u) return true;
  const std::uint64_t allowed_mask = (1ull << (set.bit_count % 64u)) - 1ull;
  return (set.words.back() & ~allowed_mask) == 0ull;
}

}  // namespace

std::string_view UnitName(Unit unit) noexcept {
  for (const UnitNameEntry& entry : kUnitNames) {
    if (entry.unit == unit) return entry.name;
  }
  return "none";
}

Outcome<Unit> ParseUnit(std::string_view text) {
  for (const UnitNameEntry& entry : kUnitNames) {
    if (entry.unit != Unit::None && entry.name == text) return entry.unit;
  }
  return Outcome<Unit>::Failure(ErrorCode::UnknownEnumDomain, "unknown unit", std::string(text));
}

bool IsQuantityUnit(Unit unit) noexcept { return unit != Unit::None; }

std::string_view ProtocolName(ProtocolId protocol) noexcept {
  for (const ProtocolEntry& entry : kProtocolNames) {
    if (entry.protocol == protocol) return entry.name;
  }
  return "unknown";
}

Outcome<ProtocolId> ParseProtocol(std::string_view text) {
  for (const ProtocolEntry& entry : kProtocolNames) {
    if (entry.protocol != ProtocolId::Unknown && entry.name == text) return entry.protocol;
  }
  return Outcome<ProtocolId>::Failure(ErrorCode::UnknownEnumDomain, "unknown protocol",
                                      std::string(text));
}

// ---------------------------------------------------------------------------
// Version
// ---------------------------------------------------------------------------

Outcome<Version> Version::Parse(std::string_view text) {
  if (text.empty() || text.size() > 40) {
    return Outcome<Version>::Failure(ErrorCode::MalformedValue, "version text length is invalid",
                                     std::string(text));
  }
  std::array<std::uint32_t, limits::kMaxVersionComponents> components{};
  std::size_t count = 0;
  std::size_t start = 0;
  while (true) {
    const std::size_t dot = text.find('.', start);
    const std::string_view part =
        dot == std::string_view::npos ? text.substr(start) : text.substr(start, dot - start);
    if (part.empty() || part.size() > 10) {
      return Outcome<Version>::Failure(ErrorCode::MalformedValue,
                                       "version component length is invalid", std::string(text));
    }
    if (part.size() > 1 && part.front() == '0') {
      return Outcome<Version>::Failure(ErrorCode::MalformedValue,
                                       "version component has a leading zero",
                                       std::string(text));
    }
    std::uint64_t value = 0;
    for (const char ch : part) {
      if (ch < '0' || ch > '9') {
        return Outcome<Version>::Failure(ErrorCode::MalformedValue,
                                         "version component is not numeric", std::string(text));
      }
      value = value * 10u + static_cast<std::uint64_t>(ch - '0');
    }
    if (value > limits::kMaxVersionComponent) {
      return Outcome<Version>::Failure(ErrorCode::ValueOutOfRange,
                                       "version component exceeds the allowed bound",
                                       std::string(text));
    }
    if (count >= limits::kMaxVersionComponents) {
      return Outcome<Version>::Failure(ErrorCode::TooManyItems,
                                       "version has more than four components", std::string(text));
    }
    components[count] = static_cast<std::uint32_t>(value);
    ++count;
    if (dot == std::string_view::npos) break;
    start = dot + 1;
  }
  if (count == 0) {
    return Outcome<Version>::Failure(ErrorCode::MalformedValue, "version has no components",
                                     std::string(text));
  }
  Version version;
  version.components_ = components;
  version.count_ = count;
  return version;
}

Outcome<Version> Version::FromComponents(std::span<const std::uint32_t> components) {
  if (components.empty() || components.size() > limits::kMaxVersionComponents) {
    return Outcome<Version>::Failure(ErrorCode::TooManyItems,
                                     "version must have between one and four components");
  }
  Version version;
  for (std::size_t index = 0; index < components.size(); ++index) {
    if (components[index] > limits::kMaxVersionComponent) {
      return Outcome<Version>::Failure(ErrorCode::ValueOutOfRange,
                                       "version component exceeds the allowed bound");
    }
    version.components_[index] = components[index];
  }
  version.count_ = components.size();
  return version;
}

std::string Version::ToString() const {
  std::string text;
  for (std::size_t index = 0; index < count_; ++index) {
    if (index != 0) text.push_back('.');
    text.append(std::to_string(components_[index]));
  }
  return text;
}

void Version::Encode(ByteWriter& writer) const {
  writer.U8(static_cast<std::uint8_t>(count_));
  for (std::size_t index = 0; index < count_; ++index) {
    writer.U32(components_[index]);
  }
}

Outcome<Version> Version::Decode(ByteReader& reader) {
  auto count = reader.U8();
  if (!count) return count.GetError();
  if (count.Value() < 1 || count.Value() > limits::kMaxVersionComponents) {
    return Outcome<Version>::Failure(ErrorCode::MalformedEncoding,
                                     "encoded version component count is out of range");
  }
  Version version;
  version.count_ = count.Value();
  for (std::size_t index = 0; index < version.count_; ++index) {
    auto component = reader.U32();
    if (!component) return component.GetError();
    if (component.Value() > limits::kMaxVersionComponent) {
      return Outcome<Version>::Failure(ErrorCode::ValueOutOfRange,
                                       "encoded version component exceeds the allowed bound");
    }
    version.components_[index] = component.Value();
  }
  return version;
}

// ---------------------------------------------------------------------------
// Scalar values
// ---------------------------------------------------------------------------

ScalarValue ScalarValue::Boolean(bool value) {
  ScalarValue scalar;
  scalar.payload_ = value;
  return scalar;
}

ScalarValue ScalarValue::Signed(std::int64_t value) {
  ScalarValue scalar;
  scalar.payload_ = value;
  return scalar;
}

ScalarValue ScalarValue::Unsigned(std::uint64_t value) {
  ScalarValue scalar;
  scalar.payload_ = value;
  return scalar;
}

Outcome<ScalarValue> ScalarValue::Enumeration(const EnumDomainId& domain, std::uint32_t code) {
  if (!domain.IsSet()) {
    return Outcome<ScalarValue>::Failure(ErrorCode::ValueTypeMismatch,
                                         "scalar enumeration requires an enum domain");
  }
  ScalarValue scalar;
  scalar.payload_ = EnumValue{domain, code};
  return scalar;
}

Outcome<ScalarValue> ScalarValue::Text(std::string_view value) {
  if (value.empty() || value.size() > limits::kMaxTextLength) {
    return Outcome<ScalarValue>::Failure(ErrorCode::TextTooLong,
                                         "scalar text length is out of range",
                                         std::to_string(value.size()));
  }
  ScalarValue scalar;
  scalar.payload_ = std::string(value);
  return scalar;
}

std::string ScalarValue::ToText() const {
  switch (GetKind()) {
    case Kind::Boolean:
      return AsBoolean() ? "true" : "false";
    case Kind::Signed:
      return std::to_string(AsSigned());
    case Kind::Unsigned:
      return std::to_string(AsUnsigned());
    case Kind::Enumeration: {
      const EnumValue& value = AsEnumeration();
      return value.domain.Value() + "#" + std::to_string(value.code);
    }
    case Kind::Text:
      return AsText();
  }
  return "?";
}

std::strong_ordering operator<=>(const ScalarValue& lhs, const ScalarValue& rhs) {
  if (lhs.GetKind() != rhs.GetKind()) {
    return static_cast<std::uint8_t>(lhs.GetKind()) <=> static_cast<std::uint8_t>(rhs.GetKind());
  }
  switch (lhs.GetKind()) {
    case ScalarValue::Kind::Boolean:
      return static_cast<int>(lhs.AsBoolean()) <=> static_cast<int>(rhs.AsBoolean());
    case ScalarValue::Kind::Signed:
      return lhs.AsSigned() <=> rhs.AsSigned();
    case ScalarValue::Kind::Unsigned:
      return lhs.AsUnsigned() <=> rhs.AsUnsigned();
    case ScalarValue::Kind::Enumeration: {
      const EnumValue& left = lhs.AsEnumeration();
      const EnumValue& right = rhs.AsEnumeration();
      if (left.domain != right.domain) return left.domain <=> right.domain;
      return left.code <=> right.code;
    }
    case ScalarValue::Kind::Text:
      return lhs.AsText() <=> rhs.AsText();
  }
  return std::strong_ordering::equal;
}

void ScalarValue::Encode(ByteWriter& writer) const {
  writer.U8(static_cast<std::uint8_t>(GetKind()));
  switch (GetKind()) {
    case Kind::Boolean:
      writer.Bool(AsBoolean());
      break;
    case Kind::Signed:
      writer.I64(AsSigned());
      break;
    case Kind::Unsigned:
      writer.U64(AsUnsigned());
      break;
    case Kind::Enumeration:
      writer.Text(AsEnumeration().domain.Value(), limits::kMaxEnumDomainIdLength);
      writer.U32(AsEnumeration().code);
      break;
    case Kind::Text:
      writer.Text(AsText(), limits::kMaxTextLength);
      break;
  }
}

Outcome<ScalarValue> ScalarValue::Decode(ByteReader& reader) {
  auto kind = reader.U8();
  if (!kind) return kind.GetError();
  switch (kind.Value()) {
    case 0: {
      auto value = reader.Bool();
      if (!value) return value.GetError();
      return ScalarValue::Boolean(value.Value());
    }
    case 1: {
      auto value = reader.I64();
      if (!value) return value.GetError();
      if (value.Value() > limits::kMaxInteger || value.Value() < limits::kMinInteger) {
        return Outcome<ScalarValue>::Failure(ErrorCode::ValueOutOfRange,
                                             "scalar integer exceeds the allowed bound");
      }
      return ScalarValue::Signed(value.Value());
    }
    case 2: {
      auto value = reader.U64();
      if (!value) return value.GetError();
      if (value.Value() > limits::kMaxQuantity) {
        return Outcome<ScalarValue>::Failure(ErrorCode::ValueOutOfRange,
                                             "scalar quantity exceeds the allowed bound");
      }
      return ScalarValue::Unsigned(value.Value());
    }
    case 3: {
      auto domain_text = reader.Text(limits::kMaxEnumDomainIdLength);
      if (!domain_text) return domain_text.GetError();
      auto domain = EnumDomainId::Parse(domain_text.Value());
      if (!domain) return domain.GetError();
      auto code = reader.U32();
      if (!code) return code.GetError();
      return ScalarValue::Enumeration(domain.Value(), code.Value());
    }
    case 4: {
      auto text = reader.Text(limits::kMaxTextLength);
      if (!text) return text.GetError();
      return ScalarValue::Text(text.Value());
    }
    default:
      return Outcome<ScalarValue>::Failure(ErrorCode::MalformedEncoding,
                                           "unknown scalar value kind tag",
                                           std::to_string(kind.Value()));
  }
}

std::string TupleValue::ToText() const {
  std::string text = "(";
  for (std::size_t index = 0; index < elements.size(); ++index) {
    if (index != 0) text.append(", ");
    text.append(elements[index].ToText());
  }
  text.push_back(')');
  return text;
}

std::strong_ordering operator<=>(const TupleValue& lhs, const TupleValue& rhs) {
  const std::size_t common = lhs.elements.size() < rhs.elements.size() ? lhs.elements.size()
                                                                      : rhs.elements.size();
  for (std::size_t index = 0; index < common; ++index) {
    const std::strong_ordering order = lhs.elements[index] <=> rhs.elements[index];
    if (order != std::strong_ordering::equal) return order;
  }
  return lhs.elements.size() <=> rhs.elements.size();
}

// ---------------------------------------------------------------------------
// Capability values
// ---------------------------------------------------------------------------

std::string_view ValueKindName(ValueKind kind) noexcept {
  switch (kind) {
    case ValueKind::Absent:
      return "absent";
    case ValueKind::Boolean:
      return "boolean";
    case ValueKind::Integer:
      return "integer";
    case ValueKind::Quantity:
      return "quantity";
    case ValueKind::Enumeration:
      return "enumeration";
    case ValueKind::BitSet:
      return "bitset";
    case ValueKind::NumericSet:
      return "numeric-set";
    case ValueKind::NumericRange:
      return "numeric-range";
    case ValueKind::VersionInterval:
      return "version-interval";
    case ValueKind::ProtocolSet:
      return "protocol-set";
    case ValueKind::Tuple:
      return "tuple";
    case ValueKind::TupleSet:
      return "tuple-set";
    case ValueKind::Record:
      return "record";
    case ValueKind::Opaque:
      return "opaque";
    case ValueKind::EnumSet:
      return "enum-set";
  }
  return "absent";
}

std::string QuantityToText(std::uint64_t value, Unit unit) {
  std::string text = std::to_string(value);
  const std::string_view suffix = [unit]() -> std::string_view {
    for (const UnitNameEntry& entry : kUnitNames) {
      if (entry.unit == unit) return entry.suffix;
    }
    return "";
  }();
  if (!suffix.empty() && unit != Unit::None) {
    text.push_back(' ');
    text.append(suffix);
  }
  return text;
}

CapabilityValue CapabilityValue::Boolean(bool value) {
  CapabilityValue result;
  result.storage_ = value;
  return result;
}

CapabilityValue CapabilityValue::Integer(std::int64_t value) {
  CapabilityValue result;
  result.storage_ = value;
  return result;
}

Outcome<CapabilityValue> CapabilityValue::QuantityValue(std::uint64_t value, Unit unit) {
  auto unit_ok = RequireQuantityUnit(unit, "quantity capability value");
  if (!unit_ok) return unit_ok.GetError();
  if (value > limits::kMaxQuantity) {
    return Outcome<CapabilityValue>::Failure(ErrorCode::ValueOutOfRange,
                                             "quantity exceeds the allowed bound",
                                             std::to_string(value));
  }
  CapabilityValue result;
  result.storage_ = Quantity{value, unit};
  return result;
}

Outcome<CapabilityValue> CapabilityValue::Enumeration(const EnumDomainId& domain,
                                                      std::uint32_t code) {
  if (!domain.IsSet()) {
    return Outcome<CapabilityValue>::Failure(ErrorCode::ValueTypeMismatch,
                                             "enumeration capability value requires a domain");
  }
  CapabilityValue result;
  result.storage_ = EnumValue{domain, code};
  return result;
}

Outcome<CapabilityValue> CapabilityValue::BitSetValue(std::uint32_t bit_count,
                                                      std::span<const std::uint64_t> words) {
  if (bit_count == 0 || bit_count > limits::kMaxBitSetBits) {
    return Outcome<CapabilityValue>::Failure(ErrorCode::ValueOutOfRange,
                                             "bitset bit count is out of range",
                                             std::to_string(bit_count));
  }
  if (words.size() != WordsForBits(bit_count)) {
    return Outcome<CapabilityValue>::Failure(
        ErrorCode::ValueNotCanonical, "bitset word count does not match the bit count",
        "words=" + std::to_string(words.size()) +
            " expected=" + std::to_string(WordsForBits(bit_count)));
  }
  BitSet set;
  set.bit_count = bit_count;
  set.words.assign(words.begin(), words.end());
  if (!BitsAboveCountAreClear(set)) {
    return Outcome<CapabilityValue>::Failure(ErrorCode::ValueNotCanonical,
                                             "bitset has bits set above its declared bit count");
  }
  CapabilityValue result;
  result.storage_ = std::move(set);
  return result;
}

Outcome<CapabilityValue> CapabilityValue::NumericSetValue(Unit unit,
                                                          std::span<const std::uint64_t> values) {
  auto unit_ok = RequireQuantityUnit(unit, "numeric set capability value");
  if (!unit_ok) return unit_ok.GetError();
  if (values.empty()) {
    return Outcome<CapabilityValue>::Failure(ErrorCode::EmptyCollection,
                                             "numeric set must not be empty");
  }
  if (values.size() > limits::kMaxSetCardinality) {
    return Outcome<CapabilityValue>::Failure(ErrorCode::TooManyItems,
                                             "numeric set exceeds the allowed cardinality",
                                             std::to_string(values.size()));
  }
  NumericSet set;
  set.unit = unit;
  set.values.assign(values.begin(), values.end());
  for (const std::uint64_t value : set.values) {
    if (value > limits::kMaxQuantity) {
      return Outcome<CapabilityValue>::Failure(ErrorCode::ValueOutOfRange,
                                               "numeric set element exceeds the allowed bound",
                                               std::to_string(value));
    }
  }
  std::sort(set.values.begin(), set.values.end());
  if (std::adjacent_find(set.values.begin(), set.values.end()) != set.values.end()) {
    return Outcome<CapabilityValue>::Failure(ErrorCode::DuplicateValue,
                                             "numeric set contains duplicate elements");
  }
  CapabilityValue result;
  result.storage_ = std::move(set);
  return result;
}

Outcome<CapabilityValue> CapabilityValue::NumericRangeValue(Unit unit, std::uint64_t minimum,
                                                            std::uint64_t maximum) {
  auto unit_ok = RequireQuantityUnit(unit, "numeric range capability value");
  if (!unit_ok) return unit_ok.GetError();
  if (minimum > maximum) {
    return Outcome<CapabilityValue>::Failure(ErrorCode::ValueContradiction,
                                             "numeric range minimum is above its maximum",
                                             std::to_string(minimum) + " > " +
                                                 std::to_string(maximum));
  }
  if (maximum > limits::kMaxQuantity) {
    return Outcome<CapabilityValue>::Failure(ErrorCode::ValueOutOfRange,
                                             "numeric range bound exceeds the allowed bound");
  }
  CapabilityValue result;
  result.storage_ = NumericRange{unit, minimum, maximum};
  return result;
}

Outcome<CapabilityValue> CapabilityValue::VersionIntervalValue(const Version& minimum,
                                                               bool min_inclusive,
                                                               const Version& maximum,
                                                               bool max_inclusive) {
  if (minimum.Count() == 0 || maximum.Count() == 0) {
    return Outcome<CapabilityValue>::Failure(ErrorCode::ValueTypeMismatch,
                                             "version interval requires both bounds");
  }
  if (maximum < minimum) {
    return Outcome<CapabilityValue>::Failure(ErrorCode::ValueContradiction,
                                             "version interval maximum is below its minimum",
                                             minimum.ToString() + " .. " + maximum.ToString());
  }
  if (minimum == maximum && (!min_inclusive || !max_inclusive)) {
    return Outcome<CapabilityValue>::Failure(ErrorCode::ValueContradiction,
                                             "empty version interval");
  }
  CapabilityValue result;
  result.storage_ = VersionInterval{minimum, maximum, min_inclusive, max_inclusive};
  return result;
}

Outcome<CapabilityValue> CapabilityValue::ProtocolSetValue(std::span<const ProtocolId> protocols) {
  if (protocols.empty()) {
    return Outcome<CapabilityValue>::Failure(ErrorCode::EmptyCollection,
                                             "protocol set must not be empty");
  }
  if (protocols.size() > limits::kMaxProtocolsInSet) {
    return Outcome<CapabilityValue>::Failure(ErrorCode::TooManyItems,
                                             "protocol set exceeds the allowed cardinality");
  }
  ProtocolSet set;
  set.protocols.assign(protocols.begin(), protocols.end());
  for (const ProtocolId protocol : set.protocols) {
    if (protocol == ProtocolId::Unknown ||
        static_cast<std::uint16_t>(protocol) > kProtocolIdMax) {
      return Outcome<CapabilityValue>::Failure(ErrorCode::UnknownEnumDomain,
                                               "protocol set contains an unknown protocol");
    }
  }
  std::sort(set.protocols.begin(), set.protocols.end());
  if (std::adjacent_find(set.protocols.begin(), set.protocols.end()) != set.protocols.end()) {
    return Outcome<CapabilityValue>::Failure(ErrorCode::DuplicateValue,
                                             "protocol set contains duplicate protocols");
  }
  CapabilityValue result;
  result.storage_ = std::move(set);
  return result;
}

Outcome<CapabilityValue> CapabilityValue::TupleValueOf(std::span<const ScalarValue> elements) {
  if (elements.size() < 2 || elements.size() > limits::kMaxTupleArity) {
    return Outcome<CapabilityValue>::Failure(ErrorCode::ValueOutOfRange,
                                             "tuple arity must be between two and eight",
                                             std::to_string(elements.size()));
  }
  TupleValue tuple;
  tuple.elements.assign(elements.begin(), elements.end());
  CapabilityValue result;
  result.storage_ = std::move(tuple);
  return result;
}

Outcome<CapabilityValue> CapabilityValue::TupleSetValue(std::span<const TupleValue> tuples) {
  if (tuples.empty()) {
    return Outcome<CapabilityValue>::Failure(ErrorCode::EmptyCollection,
                                             "tuple set must not be empty");
  }
  if (tuples.size() > limits::kMaxTupleSetCardinality) {
    return Outcome<CapabilityValue>::Failure(ErrorCode::TooManyItems,
                                             "tuple set exceeds the allowed cardinality");
  }
  TupleSet set;
  set.tuples.assign(tuples.begin(), tuples.end());
  const std::size_t arity = set.tuples.front().elements.size();
  for (const TupleValue& tuple : set.tuples) {
    if (tuple.elements.size() != arity) {
      return Outcome<CapabilityValue>::Failure(
          ErrorCode::ValueNotCanonical, "tuple set members do not share one arity");
    }
    if (tuple.elements.size() < 2 || tuple.elements.size() > limits::kMaxTupleArity) {
      return Outcome<CapabilityValue>::Failure(ErrorCode::ValueOutOfRange,
                                               "tuple set member arity is out of range");
    }
  }
  std::sort(set.tuples.begin(), set.tuples.end());
  for (std::size_t index = 1; index < set.tuples.size(); ++index) {
    if (!(set.tuples[index - 1] < set.tuples[index]) && !(set.tuples[index] < set.tuples[index - 1])) {
      return Outcome<CapabilityValue>::Failure(ErrorCode::DuplicateValue,
                                               "tuple set contains duplicate tuples");
    }
  }
  CapabilityValue result;
  result.storage_ = std::move(set);
  return result;
}

Outcome<CapabilityValue> CapabilityValue::EnumerationSetValue(const EnumDomainId& domain,
                                                              std::span<const std::uint32_t> codes) {
  if (!domain.IsSet()) {
    return Outcome<CapabilityValue>::Failure(ErrorCode::ValueTypeMismatch,
                                             "enumeration set requires a domain");
  }
  if (codes.empty()) {
    return Outcome<CapabilityValue>::Failure(ErrorCode::EmptyCollection,
                                             "enumeration set must not be empty");
  }
  if (codes.size() > limits::kMaxSetCardinality) {
    return Outcome<CapabilityValue>::Failure(ErrorCode::TooManyItems,
                                             "enumeration set exceeds the allowed cardinality");
  }
  EnumSet set;
  set.domain = domain;
  set.codes.assign(codes.begin(), codes.end());
  std::sort(set.codes.begin(), set.codes.end());
  if (std::adjacent_find(set.codes.begin(), set.codes.end()) != set.codes.end()) {
    return Outcome<CapabilityValue>::Failure(ErrorCode::DuplicateValue,
                                             "enumeration set contains duplicate codes");
  }
  CapabilityValue result;
  result.storage_ = std::move(set);
  return result;
}

Outcome<CapabilityValue> CapabilityValue::RecordValue(std::span<const RecordField> fields) {
  if (fields.empty()) {
    return Outcome<CapabilityValue>::Failure(ErrorCode::EmptyCollection,
                                             "structured record must not be empty");
  }
  if (fields.size() > limits::kMaxRecordFields) {
    return Outcome<CapabilityValue>::Failure(ErrorCode::TooManyItems,
                                             "structured record exceeds the allowed field count");
  }
  StructuredRecord record;
  record.fields.assign(fields.begin(), fields.end());
  for (const RecordField& field : record.fields) {
    auto valid = detail::ValidateToken(field.name, Charset::LowerToken, 1, 48, "record field name");
    if (!valid) return valid.GetError();
  }
  std::sort(record.fields.begin(), record.fields.end(),
            [](const RecordField& lhs, const RecordField& rhs) { return lhs.name < rhs.name; });
  for (std::size_t index = 1; index < record.fields.size(); ++index) {
    if (record.fields[index - 1].name == record.fields[index].name) {
      return Outcome<CapabilityValue>::Failure(ErrorCode::DuplicateValue,
                                               "structured record has duplicate field names",
                                               record.fields[index].name);
    }
  }
  CapabilityValue result;
  result.storage_ = std::move(record);
  return result;
}

Outcome<CapabilityValue> CapabilityValue::OpaqueValue(std::string_view encoding,
                                                      std::span<const std::byte> bytes) {
  auto valid = detail::ValidateToken(encoding, Charset::PrintableToken, 1, 64,
                                     "opaque payload encoding");
  if (!valid) return valid.GetError();
  if (bytes.size() > limits::kMaxExtensionPayloadBytes) {
    return Outcome<CapabilityValue>::Failure(ErrorCode::PayloadTooLarge,
                                             "opaque payload exceeds the allowed bound",
                                             std::to_string(bytes.size()));
  }
  OpaquePayload payload;
  payload.encoding.assign(encoding);
  payload.bytes.assign(bytes.begin(), bytes.end());
  CapabilityValue result;
  result.storage_ = std::move(payload);
  return result;
}

std::string CapabilityValue::ToText() const {
  switch (Kind()) {
    case ValueKind::Absent:
      return "absent";
    case ValueKind::Boolean:
      return AsBoolean() ? "true" : "false";
    case ValueKind::Integer:
      return std::to_string(AsInteger());
    case ValueKind::Quantity:
      return QuantityToText(AsQuantity().value, AsQuantity().unit);
    case ValueKind::Enumeration:
      return AsEnumeration().domain.Value() + "#" + std::to_string(AsEnumeration().code);
    case ValueKind::BitSet: {
      const BitSet& set = AsBitSet();
      std::string text = "{";
      bool first = true;
      for (std::uint32_t bit = 0; bit < set.bit_count; ++bit) {
        const bool set_bit = (set.words[bit / 64u] >> (bit % 64u)) & 1ull;
        if (!set_bit) continue;
        if (!first) text.push_back(',');
        first = false;
        text.append(std::to_string(bit));
      }
      text.push_back('}');
      return text;
    }
    case ValueKind::NumericSet: {
      const NumericSet& set = AsNumericSet();
      std::string text = "{";
      for (std::size_t index = 0; index < set.values.size(); ++index) {
        if (index != 0) text.append(", ");
        text.append(QuantityToText(set.values[index], set.unit));
      }
      text.push_back('}');
      return text;
    }
    case ValueKind::NumericRange: {
      const NumericRange& range = AsNumericRange();
      return "[" + QuantityToText(range.minimum, range.unit) + ", " +
             QuantityToText(range.maximum, range.unit) + "]";
    }
    case ValueKind::VersionInterval: {
      const VersionInterval& interval = AsVersionInterval();
      std::string text = interval.minimum_inclusive ? "[" : "(";
      text.append(interval.minimum.ToString());
      text.append(", ");
      text.append(interval.maximum.ToString());
      text.push_back(interval.maximum_inclusive ? ']' : ')');
      return text;
    }
    case ValueKind::ProtocolSet: {
      const ProtocolSet& set = AsProtocolSet();
      std::string text = "{";
      for (std::size_t index = 0; index < set.protocols.size(); ++index) {
        if (index != 0) text.append(", ");
        text.append(ProtocolName(set.protocols[index]));
      }
      text.push_back('}');
      return text;
    }
    case ValueKind::Tuple:
      return AsTuple().ToText();
    case ValueKind::TupleSet: {
      const TupleSet& set = AsTupleSet();
      std::string text = "{";
      for (std::size_t index = 0; index < set.tuples.size(); ++index) {
        if (index != 0) text.append(", ");
        text.append(set.tuples[index].ToText());
      }
      text.push_back('}');
      return text;
    }
    case ValueKind::Record: {
      const StructuredRecord& record = AsRecord();
      std::string text = "{";
      for (std::size_t index = 0; index < record.fields.size(); ++index) {
        if (index != 0) text.append(", ");
        text.append(record.fields[index].name);
        text.push_back('=');
        text.append(record.fields[index].value.ToText());
      }
      text.push_back('}');
      return text;
    }
    case ValueKind::Opaque: {
      const OpaquePayload& payload = AsOpaque();
      return "<opaque encoding=" + payload.encoding + " bytes=" +
             std::to_string(payload.bytes.size()) + ">";
    }
    case ValueKind::EnumSet: {
      const EnumSet& set = AsEnumSet();
      std::string text = "{";
      for (std::size_t index = 0; index < set.codes.size(); ++index) {
        if (index != 0) text.append(", ");
        text.append(EnumCodeText(set.domain, set.codes[index]));
      }
      text.push_back('}');
      return text;
    }
  }
  return "absent";
}

Outcome<void> CapabilityValue::ValidateCanonical() const {
  switch (Kind()) {
    case ValueKind::Absent:
      return Status::Success();
    case ValueKind::Boolean:
    case ValueKind::Integer:
      return Status::Success();
    case ValueKind::Quantity:
      return RequireQuantityUnit(AsQuantity().unit, "quantity capability value");
    case ValueKind::Enumeration:
      if (!AsEnumeration().domain.IsSet()) {
        return CanonicalFailure(ErrorCode::ValueTypeMismatch,
                                "enumeration capability value requires a domain");
      }
      return Status::Success();
    case ValueKind::BitSet:
      if (AsBitSet().bit_count == 0 || AsBitSet().bit_count > limits::kMaxBitSetBits ||
          !BitsAboveCountAreClear(AsBitSet())) {
        return CanonicalFailure(ErrorCode::ValueNotCanonical, "bitset is not canonical");
      }
      return Status::Success();
    case ValueKind::NumericSet: {
      const NumericSet& set = AsNumericSet();
      if (set.values.empty() || set.values.size() > limits::kMaxSetCardinality) {
        return CanonicalFailure(ErrorCode::ValueNotCanonical, "numeric set cardinality is invalid");
      }
      if (!std::is_sorted(set.values.begin(), set.values.end()) ||
          std::adjacent_find(set.values.begin(), set.values.end()) != set.values.end()) {
        return CanonicalFailure(ErrorCode::ValueNotCanonical,
                                "numeric set is not sorted and de-duplicated");
      }
      return RequireQuantityUnit(set.unit, "numeric set capability value");
    }
    case ValueKind::NumericRange: {
      const NumericRange& range = AsNumericRange();
      if (range.minimum > range.maximum) {
        return CanonicalFailure(ErrorCode::ValueContradiction,
                                "numeric range minimum is above its maximum");
      }
      return RequireQuantityUnit(range.unit, "numeric range capability value");
    }
    case ValueKind::VersionInterval: {
      const VersionInterval& interval = AsVersionInterval();
      if (interval.maximum < interval.minimum) {
        return CanonicalFailure(ErrorCode::ValueContradiction,
                                "version interval maximum is below its minimum");
      }
      if (interval.minimum == interval.maximum &&
          (!interval.minimum_inclusive || !interval.maximum_inclusive)) {
        return CanonicalFailure(ErrorCode::ValueContradiction, "empty version interval");
      }
      return Status::Success();
    }
    case ValueKind::ProtocolSet: {
      const ProtocolSet& set = AsProtocolSet();
      if (set.protocols.empty() || set.protocols.size() > limits::kMaxProtocolsInSet) {
        return CanonicalFailure(ErrorCode::ValueNotCanonical,
                                "protocol set cardinality is invalid");
      }
      if (!std::is_sorted(set.protocols.begin(), set.protocols.end()) ||
          std::adjacent_find(set.protocols.begin(), set.protocols.end()) != set.protocols.end()) {
        return CanonicalFailure(ErrorCode::ValueNotCanonical,
                                "protocol set is not sorted and de-duplicated");
      }
      for (const ProtocolId protocol : set.protocols) {
        if (protocol == ProtocolId::Unknown || static_cast<std::uint16_t>(protocol) > kProtocolIdMax) {
          return CanonicalFailure(ErrorCode::UnknownEnumDomain,
                                  "protocol set contains an unknown protocol");
        }
      }
      return Status::Success();
    }
    case ValueKind::Tuple: {
      const TupleValue& tuple = AsTuple();
      if (tuple.elements.size() < 2 || tuple.elements.size() > limits::kMaxTupleArity) {
        return CanonicalFailure(ErrorCode::ValueOutOfRange, "tuple arity is out of range");
      }
      return Status::Success();
    }
    case ValueKind::TupleSet: {
      const TupleSet& set = AsTupleSet();
      if (set.tuples.empty() || set.tuples.size() > limits::kMaxTupleSetCardinality) {
        return CanonicalFailure(ErrorCode::ValueNotCanonical, "tuple set cardinality is invalid");
      }
      const std::size_t arity = set.tuples.front().elements.size();
      for (std::size_t index = 0; index < set.tuples.size(); ++index) {
        if (set.tuples[index].elements.size() != arity) {
          return CanonicalFailure(ErrorCode::ValueNotCanonical,
                                  "tuple set members do not share one arity");
        }
        if (index != 0 && !(set.tuples[index - 1] < set.tuples[index])) {
          return CanonicalFailure(ErrorCode::ValueNotCanonical,
                                  "tuple set is not sorted and de-duplicated");
        }
      }
      return Status::Success();
    }
    case ValueKind::Record: {
      const StructuredRecord& record = AsRecord();
      if (record.fields.empty() || record.fields.size() > limits::kMaxRecordFields) {
        return CanonicalFailure(ErrorCode::ValueNotCanonical,
                                "structured record field count is invalid");
      }
      for (std::size_t index = 0; index < record.fields.size(); ++index) {
        if (index != 0 && !(record.fields[index - 1].name < record.fields[index].name)) {
          return CanonicalFailure(ErrorCode::ValueNotCanonical,
                                  "structured record fields are not sorted and unique");
        }
      }
      return Status::Success();
    }
    case ValueKind::Opaque: {
      const OpaquePayload& payload = AsOpaque();
      if (payload.encoding.empty() || payload.bytes.size() > limits::kMaxExtensionPayloadBytes) {
        return CanonicalFailure(ErrorCode::ValueNotCanonical, "opaque payload is not canonical");
      }
      return Status::Success();
    }
    case ValueKind::EnumSet: {
      const EnumSet& set = AsEnumSet();
      if (!set.domain.IsSet()) {
        return CanonicalFailure(ErrorCode::ValueTypeMismatch, "enumeration set requires a domain");
      }
      if (set.codes.empty() || set.codes.size() > limits::kMaxSetCardinality) {
        return CanonicalFailure(ErrorCode::ValueNotCanonical,
                                "enumeration set cardinality is invalid");
      }
      if (!std::is_sorted(set.codes.begin(), set.codes.end()) ||
          std::adjacent_find(set.codes.begin(), set.codes.end()) != set.codes.end()) {
        return CanonicalFailure(ErrorCode::ValueNotCanonical,
                                "enumeration set is not sorted and de-duplicated");
      }
      return Status::Success();
    }
  }
  return CanonicalFailure(ErrorCode::MalformedValue, "unknown capability value kind");
}

bool operator==(const CapabilityValue& lhs, const CapabilityValue& rhs) {
  if (lhs.Kind() != rhs.Kind()) return false;
  switch (lhs.Kind()) {
    case ValueKind::Absent:
      return true;
    case ValueKind::Boolean:
      return lhs.AsBoolean() == rhs.AsBoolean();
    case ValueKind::Integer:
      return lhs.AsInteger() == rhs.AsInteger();
    case ValueKind::Quantity:
      return lhs.AsQuantity() == rhs.AsQuantity();
    case ValueKind::Enumeration:
      return lhs.AsEnumeration() == rhs.AsEnumeration();
    case ValueKind::BitSet:
      return lhs.AsBitSet() == rhs.AsBitSet();
    case ValueKind::NumericSet:
      return lhs.AsNumericSet() == rhs.AsNumericSet();
    case ValueKind::NumericRange:
      return lhs.AsNumericRange() == rhs.AsNumericRange();
    case ValueKind::VersionInterval:
      return lhs.AsVersionInterval() == rhs.AsVersionInterval();
    case ValueKind::ProtocolSet:
      return lhs.AsProtocolSet() == rhs.AsProtocolSet();
    case ValueKind::Tuple:
      return lhs.AsTuple() == rhs.AsTuple();
    case ValueKind::TupleSet:
      return lhs.AsTupleSet() == rhs.AsTupleSet();
    case ValueKind::Record:
      return lhs.AsRecord() == rhs.AsRecord();
    case ValueKind::Opaque:
      return lhs.AsOpaque() == rhs.AsOpaque();
    case ValueKind::EnumSet:
      return lhs.AsEnumSet() == rhs.AsEnumSet();
  }
  return false;
}

bool operator==(const BitSet& lhs, const BitSet& rhs) {
  if (lhs.bit_count != rhs.bit_count) return false;
  if (lhs.words.size() != WordsForBits(lhs.bit_count)) return false;
  if (rhs.words.size() != WordsForBits(rhs.bit_count)) return false;
  return lhs.words == rhs.words;
}

void CapabilityValue::Encode(ByteWriter& writer) const {
  writer.U8(static_cast<std::uint8_t>(Kind()));
  switch (Kind()) {
    case ValueKind::Absent:
      break;
    case ValueKind::Boolean:
      writer.Bool(AsBoolean());
      break;
    case ValueKind::Integer:
      writer.I64(AsInteger());
      break;
    case ValueKind::Quantity:
      writer.U64(AsQuantity().value);
      writer.U8(static_cast<std::uint8_t>(AsQuantity().unit));
      break;
    case ValueKind::Enumeration:
      writer.Text(AsEnumeration().domain.Value(), limits::kMaxEnumDomainIdLength);
      writer.U32(AsEnumeration().code);
      break;
    case ValueKind::BitSet:
      writer.U32(AsBitSet().bit_count);
      writer.U32(static_cast<std::uint32_t>(AsBitSet().words.size()));
      for (const std::uint64_t word : AsBitSet().words) writer.U64(word);
      break;
    case ValueKind::NumericSet:
      writer.U8(static_cast<std::uint8_t>(AsNumericSet().unit));
      writer.U32(static_cast<std::uint32_t>(AsNumericSet().values.size()));
      for (const std::uint64_t value : AsNumericSet().values) writer.U64(value);
      break;
    case ValueKind::NumericRange:
      writer.U8(static_cast<std::uint8_t>(AsNumericRange().unit));
      writer.U64(AsNumericRange().minimum);
      writer.U64(AsNumericRange().maximum);
      break;
    case ValueKind::VersionInterval:
      AsVersionInterval().minimum.Encode(writer);
      writer.Bool(AsVersionInterval().minimum_inclusive);
      AsVersionInterval().maximum.Encode(writer);
      writer.Bool(AsVersionInterval().maximum_inclusive);
      break;
    case ValueKind::ProtocolSet:
      writer.U32(static_cast<std::uint32_t>(AsProtocolSet().protocols.size()));
      for (const ProtocolId protocol : AsProtocolSet().protocols) {
        writer.U16(static_cast<std::uint16_t>(protocol));
      }
      break;
    case ValueKind::Tuple:
      writer.U32(static_cast<std::uint32_t>(AsTuple().elements.size()));
      for (const ScalarValue& element : AsTuple().elements) element.Encode(writer);
      break;
    case ValueKind::TupleSet:
      writer.U32(static_cast<std::uint32_t>(AsTupleSet().tuples.size()));
      for (const TupleValue& tuple : AsTupleSet().tuples) {
        writer.U32(static_cast<std::uint32_t>(tuple.elements.size()));
        for (const ScalarValue& element : tuple.elements) element.Encode(writer);
      }
      break;
    case ValueKind::Record:
      writer.U32(static_cast<std::uint32_t>(AsRecord().fields.size()));
      for (const RecordField& field : AsRecord().fields) {
        writer.Text(field.name, 48);
        field.value.Encode(writer);
      }
      break;
    case ValueKind::Opaque:
      writer.Text(AsOpaque().encoding, 64);
      writer.Bytes(AsOpaque().bytes, limits::kMaxExtensionPayloadBytes);
      break;
    case ValueKind::EnumSet:
      writer.Text(AsEnumSet().domain.Value(), limits::kMaxEnumDomainIdLength);
      writer.U32(static_cast<std::uint32_t>(AsEnumSet().codes.size()));
      for (const std::uint32_t code : AsEnumSet().codes) writer.U32(code);
      break;
  }
}

Outcome<CapabilityValue> CapabilityValue::Decode(ByteReader& reader) {
  auto kind = reader.U8();
  if (!kind) return kind.GetError();
  const std::uint8_t tag = kind.Value();
  if (tag > static_cast<std::uint8_t>(ValueKind::EnumSet)) {
    return Outcome<CapabilityValue>::Failure(ErrorCode::MalformedEncoding,
                                             "unknown capability value kind tag",
                                             std::to_string(tag));
  }
  switch (static_cast<ValueKind>(tag)) {
    case ValueKind::Absent:
      return CapabilityValue();
    case ValueKind::Boolean: {
      auto value = reader.Bool();
      if (!value) return value.GetError();
      return CapabilityValue::Boolean(value.Value());
    }
    case ValueKind::Integer: {
      auto value = reader.I64();
      if (!value) return value.GetError();
      if (value.Value() > limits::kMaxInteger || value.Value() < limits::kMinInteger) {
        return Outcome<CapabilityValue>::Failure(ErrorCode::ValueOutOfRange,
                                                 "encoded integer exceeds the allowed bound");
      }
      return CapabilityValue::Integer(value.Value());
    }
    case ValueKind::Quantity: {
      auto value = reader.U64();
      if (!value) return value.GetError();
      auto unit = reader.U8();
      if (!unit) return unit.GetError();
      if (unit.Value() > static_cast<std::uint8_t>(Unit::Nanometers)) {
        return Outcome<CapabilityValue>::Failure(ErrorCode::MalformedEncoding,
                                                 "unknown unit tag");
      }
      return CapabilityValue::QuantityValue(value.Value(), static_cast<Unit>(unit.Value()));
    }
    case ValueKind::Enumeration: {
      auto domain_text = reader.Text(limits::kMaxEnumDomainIdLength);
      if (!domain_text) return domain_text.GetError();
      auto domain = EnumDomainId::Parse(domain_text.Value());
      if (!domain) return domain.GetError();
      auto code = reader.U32();
      if (!code) return code.GetError();
      return CapabilityValue::Enumeration(domain.Value(), code.Value());
    }
    case ValueKind::BitSet: {
      auto bit_count = reader.U32();
      if (!bit_count) return bit_count.GetError();
      auto word_count = reader.U32();
      if (!word_count) return word_count.GetError();
      const std::size_t expected = WordsForBits(bit_count.Value());
      if (word_count.Value() != expected || expected > WordsForBits(limits::kMaxBitSetBits)) {
        return Outcome<CapabilityValue>::Failure(ErrorCode::ValueNotCanonical,
                                                 "encoded bitset word count is invalid");
      }
      std::vector<std::uint64_t> words;
      words.reserve(expected);
      for (std::size_t index = 0; index < expected; ++index) {
        auto word = reader.U64();
        if (!word) return word.GetError();
        words.push_back(word.Value());
      }
      return CapabilityValue::BitSetValue(bit_count.Value(), words);
    }
    case ValueKind::NumericSet: {
      auto unit = reader.U8();
      if (!unit) return unit.GetError();
      if (unit.Value() > static_cast<std::uint8_t>(Unit::Nanometers)) {
        return Outcome<CapabilityValue>::Failure(ErrorCode::MalformedEncoding, "unknown unit tag");
      }
      auto count = reader.U32();
      if (!count) return count.GetError();
      if (count.Value() > limits::kMaxSetCardinality) {
        return Outcome<CapabilityValue>::Failure(ErrorCode::TooManyItems,
                                                 "encoded numeric set exceeds the allowed cardinality");
      }
      std::vector<std::uint64_t> values;
      values.reserve(count.Value());
      for (std::size_t index = 0; index < count.Value(); ++index) {
        auto value = reader.U64();
        if (!value) return value.GetError();
        values.push_back(value.Value());
      }
      for (std::size_t index = 1; index < values.size(); ++index) {
        if (values[index - 1] >= values[index]) {
          return Outcome<CapabilityValue>::Failure(
              ErrorCode::ValueNotCanonical,
              "encoded numeric set is not sorted and de-duplicated");
        }
      }
      return CapabilityValue::NumericSetValue(static_cast<Unit>(unit.Value()), values);
    }
    case ValueKind::NumericRange: {
      auto unit = reader.U8();
      if (!unit) return unit.GetError();
      if (unit.Value() > static_cast<std::uint8_t>(Unit::Nanometers)) {
        return Outcome<CapabilityValue>::Failure(ErrorCode::MalformedEncoding, "unknown unit tag");
      }
      auto minimum = reader.U64();
      if (!minimum) return minimum.GetError();
      auto maximum = reader.U64();
      if (!maximum) return maximum.GetError();
      return CapabilityValue::NumericRangeValue(static_cast<Unit>(unit.Value()), minimum.Value(),
                                                maximum.Value());
    }
    case ValueKind::VersionInterval: {
      auto minimum = Version::Decode(reader);
      if (!minimum) return minimum.GetError();
      auto minimum_inclusive = reader.Bool();
      if (!minimum_inclusive) return minimum_inclusive.GetError();
      auto maximum = Version::Decode(reader);
      if (!maximum) return maximum.GetError();
      auto maximum_inclusive = reader.Bool();
      if (!maximum_inclusive) return maximum_inclusive.GetError();
      return CapabilityValue::VersionIntervalValue(minimum.Value(), minimum_inclusive.Value(),
                                                   maximum.Value(), maximum_inclusive.Value());
    }
    case ValueKind::ProtocolSet: {
      auto count = reader.U32();
      if (!count) return count.GetError();
      if (count.Value() > limits::kMaxProtocolsInSet) {
        return Outcome<CapabilityValue>::Failure(
            ErrorCode::TooManyItems, "encoded protocol set exceeds the allowed cardinality");
      }
      std::vector<ProtocolId> protocols;
      protocols.reserve(count.Value());
      for (std::size_t index = 0; index < count.Value(); ++index) {
        auto protocol = reader.U16();
        if (!protocol) return protocol.GetError();
        protocols.push_back(static_cast<ProtocolId>(protocol.Value()));
      }
      return CapabilityValue::ProtocolSetValue(protocols);
    }
    case ValueKind::Tuple: {
      auto count = reader.U32();
      if (!count) return count.GetError();
      if (count.Value() < 2 || count.Value() > limits::kMaxTupleArity) {
        return Outcome<CapabilityValue>::Failure(ErrorCode::ValueOutOfRange,
                                                 "encoded tuple arity is out of range");
      }
      std::vector<ScalarValue> elements;
      elements.reserve(count.Value());
      for (std::size_t index = 0; index < count.Value(); ++index) {
        auto element = ScalarValue::Decode(reader);
        if (!element) return element.GetError();
        elements.push_back(element.Value());
      }
      return CapabilityValue::TupleValueOf(elements);
    }
    case ValueKind::TupleSet: {
      auto count = reader.U32();
      if (!count) return count.GetError();
      if (count.Value() > limits::kMaxTupleSetCardinality) {
        return Outcome<CapabilityValue>::Failure(ErrorCode::TooManyItems,
                                                 "encoded tuple set exceeds the allowed cardinality");
      }
      std::vector<TupleValue> tuples;
      tuples.reserve(count.Value());
      for (std::size_t index = 0; index < count.Value(); ++index) {
        auto arity = reader.U32();
        if (!arity) return arity.GetError();
        if (arity.Value() < 2 || arity.Value() > limits::kMaxTupleArity) {
          return Outcome<CapabilityValue>::Failure(ErrorCode::ValueOutOfRange,
                                                   "encoded tuple arity is out of range");
        }
        TupleValue tuple;
        tuple.elements.reserve(arity.Value());
        for (std::size_t element = 0; element < arity.Value(); ++element) {
          auto decoded = ScalarValue::Decode(reader);
          if (!decoded) return decoded.GetError();
          tuple.elements.push_back(decoded.Value());
        }
        tuples.push_back(std::move(tuple));
      }
      return CapabilityValue::TupleSetValue(tuples);
    }
    case ValueKind::Record: {
      auto count = reader.U32();
      if (!count) return count.GetError();
      if (count.Value() > limits::kMaxRecordFields) {
        return Outcome<CapabilityValue>::Failure(ErrorCode::TooManyItems,
                                                 "encoded record exceeds the allowed field count");
      }
      std::vector<RecordField> fields;
      fields.reserve(count.Value());
      for (std::size_t index = 0; index < count.Value(); ++index) {
        auto name = reader.Text(48);
        if (!name) return name.GetError();
        auto value = ScalarValue::Decode(reader);
        if (!value) return value.GetError();
        fields.push_back(RecordField{name.Value(), value.Value()});
      }
      return CapabilityValue::RecordValue(fields);
    }
    case ValueKind::Opaque: {
      auto encoding = reader.Text(64);
      if (!encoding) return encoding.GetError();
      auto bytes = reader.Bytes(limits::kMaxExtensionPayloadBytes);
      if (!bytes) return bytes.GetError();
      return CapabilityValue::OpaqueValue(encoding.Value(), bytes.Value());
    }
    case ValueKind::EnumSet: {
      auto domain_text = reader.Text(limits::kMaxEnumDomainIdLength);
      if (!domain_text) return domain_text.GetError();
      auto domain = EnumDomainId::Parse(domain_text.Value());
      if (!domain) return domain.GetError();
      auto count = reader.U32();
      if (!count) return count.GetError();
      if (count.Value() > limits::kMaxSetCardinality) {
        return Outcome<CapabilityValue>::Failure(
            ErrorCode::TooManyItems, "encoded enumeration set exceeds the allowed cardinality");
      }
      std::vector<std::uint32_t> codes;
      codes.reserve(count.Value());
      for (std::size_t index = 0; index < count.Value(); ++index) {
        auto code = reader.U32();
        if (!code) return code.GetError();
        codes.push_back(code.Value());
      }
      return CapabilityValue::EnumerationSetValue(domain.Value(), codes);
    }
  }
  return Outcome<CapabilityValue>::Failure(ErrorCode::MalformedEncoding,
                                           "unknown capability value kind tag");
}

Digest DigestOf(const CapabilityValue& value) {
  std::vector<std::byte> bytes;
  bytes.reserve(64);
  ByteWriter writer(bytes);
  value.Encode(writer);
  return ComputeDigest(bytes);
}

}  // namespace fabric::capability
