// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "fabric/capability/digest.hpp"
#include "fabric/capability/encoding.hpp"
#include "fabric/capability/error.hpp"
#include "fabric/capability/ids.hpp"
#include "fabric/capability/limits.hpp"

namespace fabric::capability {

// ---------------------------------------------------------------------------
// Units. Quantities are always expressed in a checked unit; important numeric
// limits are never stored as untyped strings.
// ---------------------------------------------------------------------------

enum class Unit : std::uint8_t {
  None = 0,
  BitsPerSecond,
  Bytes,
  BytesPerSecond,
  Count,
  Packets,
  Nanoseconds,
  Microseconds,
  Milliseconds,
  Percent,
  DecibelMilliwatts,
  MillidegreesCelsius,
  Nanometers,
};

std::string_view UnitName(Unit unit) noexcept;
Outcome<Unit> ParseUnit(std::string_view text);
/// False only for Unit::None; quantity values require a real unit.
bool IsQuantityUnit(Unit unit) noexcept;

// ---------------------------------------------------------------------------
// Protocol families (vendor neutral vocabulary only).
// ---------------------------------------------------------------------------

enum class ProtocolId : std::uint16_t {
  Unknown = 0,
  Ethernet = 1,
  Ipv4,
  Ipv6,
  Vlan,
  Mpls,
  Vxlan,
  Geneve,
  Srv6,
  Tcp,
  Udp,
  RoceV1,
  RoceV2,
  Iwarp,
  InfiniBand,
  Ptp,
  NvmeOf,
  FibreChannel,
  Fcoe,
  Lacp,
  LacpPdu,
};

inline constexpr std::uint16_t kProtocolIdMax = static_cast<std::uint16_t>(ProtocolId::LacpPdu);

std::string_view ProtocolName(ProtocolId protocol) noexcept;
Outcome<ProtocolId> ParseProtocol(std::string_view text);

// ---------------------------------------------------------------------------
// Version value: up to four numeric components, compared with zero padding.
// ---------------------------------------------------------------------------

class Version {
 public:
  Version() = default;

  /// Parses "1", "1.2", "1.2.3" or "1.2.3.4". Rejects empty components,
  /// non-numeric components, more than four components and absurd values.
  static Outcome<Version> Parse(std::string_view text);
  static Outcome<Version> FromComponents(std::span<const std::uint32_t> components);

  std::size_t Count() const noexcept { return count_; }
  std::uint32_t Component(std::size_t index) const noexcept {
    return index < count_ ? components_[index] : 0u;
  }
  std::string ToString() const;

  friend bool operator==(const Version& lhs, const Version& rhs) noexcept {
    for (std::size_t i = 0; i < limits::kMaxVersionComponents; ++i) {
      if (lhs.Component(i) != rhs.Component(i)) return false;
    }
    return true;
  }
  friend std::strong_ordering operator<=>(const Version& lhs, const Version& rhs) noexcept {
    for (std::size_t i = 0; i < limits::kMaxVersionComponents; ++i) {
      if (lhs.Component(i) != rhs.Component(i)) {
        return lhs.Component(i) <=> rhs.Component(i);
      }
    }
    return std::strong_ordering::equal;
  }

  void Encode(ByteWriter& writer) const;
  static Outcome<Version> Decode(ByteReader& reader);

 private:
  std::array<std::uint32_t, limits::kMaxVersionComponents> components_{};
  std::size_t count_ = 0;
};

// ---------------------------------------------------------------------------
// Value payload types.
// ---------------------------------------------------------------------------

struct Quantity {
  std::uint64_t value = 0;
  Unit unit = Unit::None;

  friend bool operator==(const Quantity&, const Quantity&) = default;
};

struct EnumValue {
  EnumDomainId domain;
  std::uint32_t code = 0;

  friend bool operator==(const EnumValue&, const EnumValue&) = default;
};

/// Canonical (sorted, de-duplicated, non-empty) set of enumeration codes
/// drawn from one declared enumeration domain.
struct EnumSet {
  EnumDomainId domain;
  std::vector<std::uint32_t> codes;

  friend bool operator==(const EnumSet&, const EnumSet&) = default;
};

/// Fixed width bit vector. Bits above c bit_count are always zero, so equal
/// capability bitsets with different word padding still compare equal.
struct BitSet {
  std::uint32_t bit_count = 0;
  std::vector<std::uint64_t> words;

  friend bool operator==(const BitSet& lhs, const BitSet& rhs);
};

/// Canonical (sorted, de-duplicated, non-empty) set of quantities.
struct NumericSet {
  Unit unit = Unit::None;
  std::vector<std::uint64_t> values;

  friend bool operator==(const NumericSet&, const NumericSet&) = default;
};

/// Inclusive numeric range.
struct NumericRange {
  Unit unit = Unit::None;
  std::uint64_t minimum = 0;
  std::uint64_t maximum = 0;

  friend bool operator==(const NumericRange&, const NumericRange&) = default;
};

/// Inclusive or exclusive version interval.
struct VersionInterval {
  Version minimum;
  Version maximum;
  bool minimum_inclusive = true;
  bool maximum_inclusive = true;

  friend bool operator==(const VersionInterval&, const VersionInterval&) = default;
};

/// Canonical (sorted, de-duplicated, non-empty) protocol set.
struct ProtocolSet {
  std::vector<ProtocolId> protocols;

  friend bool operator==(const ProtocolSet&, const ProtocolSet&) = default;
};

/// One element of a capability tuple. Tuples are fixed arity sequences of
/// scalars; they are used for combinations such as (lane count, lane rate).
class ScalarValue {
 public:
  enum class Kind : std::uint8_t { Boolean, Signed, Unsigned, Enumeration, Text };

  ScalarValue() = default;

  static ScalarValue Boolean(bool value);
  static ScalarValue Signed(std::int64_t value);
  static ScalarValue Unsigned(std::uint64_t value);
  static Outcome<ScalarValue> Enumeration(const EnumDomainId& domain, std::uint32_t code);
  static Outcome<ScalarValue> Text(std::string_view value);

  Kind GetKind() const noexcept { return static_cast<Kind>(payload_.index()); }
  bool AsBoolean() const { return std::get<bool>(payload_); }
  std::int64_t AsSigned() const { return std::get<std::int64_t>(payload_); }
  std::uint64_t AsUnsigned() const { return std::get<std::uint64_t>(payload_); }
  const EnumValue& AsEnumeration() const { return std::get<EnumValue>(payload_); }
  const std::string& AsText() const { return std::get<std::string>(payload_); }

  std::string ToText() const;
  friend bool operator==(const ScalarValue&, const ScalarValue&) = default;
  friend std::strong_ordering operator<=>(const ScalarValue& lhs, const ScalarValue& rhs);

  void Encode(ByteWriter& writer) const;
  static Outcome<ScalarValue> Decode(ByteReader& reader);

 private:
  using Payload = std::variant<bool, std::int64_t, std::uint64_t, EnumValue, std::string>;
  Payload payload_{false};
};

struct TupleValue {
  std::vector<ScalarValue> elements;

  std::string ToText() const;
  friend bool operator==(const TupleValue&, const TupleValue&) = default;
  friend std::strong_ordering operator<=>(const TupleValue& lhs, const TupleValue& rhs);
};

/// Canonical (sorted, de-duplicated, non-empty) set of tuples.
struct TupleSet {
  std::vector<TupleValue> tuples;

  friend bool operator==(const TupleSet&, const TupleSet&) = default;
};

struct RecordField {
  std::string name;
  ScalarValue value;

  friend bool operator==(const RecordField&, const RecordField&) = default;
};

/// Bounded structured object: sorted, uniquely named scalar fields.
struct StructuredRecord {
  std::vector<RecordField> fields;

  friend bool operator==(const StructuredRecord&, const StructuredRecord&) = default;
};

/// Opaque vendor extension payload. Bounded, never interpreted by the core,
/// and never able to redefine canonical capability semantics.
struct OpaquePayload {
  std::string encoding;
  std::vector<std::byte> bytes;

  friend bool operator==(const OpaquePayload&, const OpaquePayload&) = default;
};

/// Discriminator of the typed capability value model.
enum class ValueKind : std::uint8_t {
  Absent = 0,
  Boolean,
  Integer,
  Quantity,
  Enumeration,
  BitSet,
  NumericSet,
  NumericRange,
  VersionInterval,
  ProtocolSet,
  Tuple,
  TupleSet,
  Record,
  Opaque,
  EnumSet,
};

std::string_view ValueKindName(ValueKind kind) noexcept;

/// A capability value. Exactly one typed alternative is active; the model
/// never degrades a numeric limit into a string.
class CapabilityValue {
 public:
  CapabilityValue() = default;

  static CapabilityValue Boolean(bool value);
  static CapabilityValue Integer(std::int64_t value);
  static Outcome<CapabilityValue> QuantityValue(std::uint64_t value, Unit unit);
  static Outcome<CapabilityValue> Enumeration(const EnumDomainId& domain, std::uint32_t code);
  static Outcome<CapabilityValue> BitSetValue(std::uint32_t bit_count,
                                              std::span<const std::uint64_t> words);
  static Outcome<CapabilityValue> NumericSetValue(Unit unit, std::span<const std::uint64_t> values);
  static Outcome<CapabilityValue> NumericRangeValue(Unit unit, std::uint64_t minimum,
                                                    std::uint64_t maximum);
  static Outcome<CapabilityValue> VersionIntervalValue(const Version& minimum, bool min_inclusive,
                                                       const Version& maximum, bool max_inclusive);
  static Outcome<CapabilityValue> ProtocolSetValue(std::span<const ProtocolId> protocols);
  static Outcome<CapabilityValue> TupleValueOf(std::span<const ScalarValue> elements);
  static Outcome<CapabilityValue> TupleSetValue(std::span<const TupleValue> tuples);
  static Outcome<CapabilityValue> EnumerationSetValue(const EnumDomainId& domain,
                                                      std::span<const std::uint32_t> codes);
  static Outcome<CapabilityValue> RecordValue(std::span<const RecordField> fields);
  static Outcome<CapabilityValue> OpaqueValue(std::string_view encoding,
                                              std::span<const std::byte> bytes);

  ValueKind Kind() const noexcept { return static_cast<ValueKind>(storage_.index()); }
  bool IsAbsent() const noexcept { return storage_.index() == 0; }

  bool AsBoolean() const { return std::get<bool>(storage_); }
  std::int64_t AsInteger() const { return std::get<std::int64_t>(storage_); }
  const Quantity& AsQuantity() const { return std::get<Quantity>(storage_); }
  const EnumValue& AsEnumeration() const { return std::get<EnumValue>(storage_); }
  const BitSet& AsBitSet() const { return std::get<BitSet>(storage_); }
  const NumericSet& AsNumericSet() const { return std::get<NumericSet>(storage_); }
  const NumericRange& AsNumericRange() const { return std::get<NumericRange>(storage_); }
  const VersionInterval& AsVersionInterval() const { return std::get<VersionInterval>(storage_); }
  const ProtocolSet& AsProtocolSet() const { return std::get<ProtocolSet>(storage_); }
  const TupleValue& AsTuple() const { return std::get<TupleValue>(storage_); }
  const TupleSet& AsTupleSet() const { return std::get<TupleSet>(storage_); }
  const StructuredRecord& AsRecord() const { return std::get<StructuredRecord>(storage_); }
  const OpaquePayload& AsOpaque() const { return std::get<OpaquePayload>(storage_); }
  const EnumSet& AsEnumSet() const { return std::get<EnumSet>(storage_); }

  /// Deterministic, stable, human readable rendering used by diagnostics,
  /// explanations and the CLI.
  std::string ToText() const;

  /// Re-validates the canonical invariants of the active alternative:
  /// non-empty sets, sorted and de-duplicated members, ordered ranges,
  /// consistent units, bounded payloads.
  Outcome<void> ValidateCanonical() const;

  /// True when both values denote the same typed claim.
  friend bool operator==(const CapabilityValue& lhs, const CapabilityValue& rhs);

  void Encode(ByteWriter& writer) const;
  static Outcome<CapabilityValue> Decode(ByteReader& reader);

 private:
  using Storage = std::variant<std::monostate, bool, std::int64_t, Quantity, EnumValue, BitSet,
                               NumericSet, NumericRange, VersionInterval, ProtocolSet, TupleValue,
                               TupleSet, StructuredRecord, OpaquePayload, EnumSet>;
  Storage storage_{};
};

/// Stable text rendering of a quantity including its unit.
std::string QuantityToText(std::uint64_t value, Unit unit);

/// Digest of a canonical capability value encoding.
Digest DigestOf(const CapabilityValue& value);

}  // namespace fabric::capability
