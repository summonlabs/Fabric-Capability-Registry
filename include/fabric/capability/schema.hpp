// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "fabric/capability/error.hpp"
#include "fabric/capability/ids.hpp"
#include "fabric/capability/value.hpp"

namespace fabric::capability {

// ---------------------------------------------------------------------------
// Enumeration domains. Enumeration codes are only meaningful inside a
// declared domain; a code that the domain does not declare is rejected.
// ---------------------------------------------------------------------------

struct EnumCodeDescriptor {
  std::uint32_t code = 0;
  std::string_view name;
};

struct EnumDomainDescriptor {
  EnumDomainId id;
  std::string_view description;
  std::vector<EnumCodeDescriptor> codes;
};

/// Immutable table of the enumeration domains the canonical schema uses.
const std::vector<EnumDomainDescriptor>& BuiltinEnumDomains();

/// Looks up an enumeration domain. Fails with UnknownEnumDomain when the
/// domain is not declared.
Outcome<const EnumDomainDescriptor*> FindEnumDomain(const EnumDomainId& domain);

/// Stable name of an enumeration code, or a deterministic rendering of the
/// numeric code when the domain does not declare it.
std::string EnumCodeText(const EnumDomainId& domain, std::uint32_t code);

bool EnumDomainDeclares(const EnumDomainId& domain, std::uint32_t code);

// ---------------------------------------------------------------------------
// Capability descriptors: the governed schema.
// ---------------------------------------------------------------------------

/// Declares the accepted typed form and the numeric/cardinality bounds of one
/// capability. A capability that has no descriptor is rejected: the registry
/// is not a free form key/value store.
struct CapabilityDescriptor {
  CapabilityId id;
  ValueKind kind = ValueKind::Boolean;
  Unit unit = Unit::None;
  EnumDomainId enum_domain;
  std::uint64_t minimum_quantity = 0;
  std::uint64_t maximum_quantity = limits::kMaxQuantity;
  std::uint32_t bit_count = 0;
  std::uint8_t tuple_arity = 0;
  std::array<ValueKind, limits::kMaxTupleArity> tuple_component_kinds{};
  std::array<Unit, limits::kMaxTupleArity> tuple_component_units{};
  std::size_t max_set_cardinality = limits::kMaxSetCardinality;
  std::vector<ProtocolId> allowed_protocols;
  CompatibilityClassId compatibility_class;
  bool vendor_extension = false;
  /// True only when the semantics of the capability make the absence of a
  /// report authoritative. Absence of a report then resolves to UNSUPPORTED,
  /// and only when a current full enumeration from direct device evidence
  /// exists. Every other capability resolves absence to UNKNOWN.
  bool absence_is_negative = false;
  std::string description;

  friend bool operator==(const CapabilityDescriptor&, const CapabilityDescriptor&) = default;
};

/// Evaluates a value (or the absence of a value) against a descriptor.
Outcome<void> ValidateValueAgainstDescriptor(const CapabilityDescriptor& descriptor,
                                             const CapabilityValue& value);

/// Capability schema: the built-in canonical descriptors plus bounded,
/// validated vendor extension descriptors.
class CapabilitySchema {
 public:
  /// Schema pre-loaded with the canonical vendor neutral descriptors.
  CapabilitySchema();
  CapabilitySchema(const CapabilitySchema&) = delete;
  CapabilitySchema& operator=(const CapabilitySchema&) = delete;
  ~CapabilitySchema() = default;

  /// Registers a vendor extension descriptor. The descriptor must live in a
  /// "vendor." namespace and must not shadow a canonical capability.
  Outcome<void> RegisterVendorDescriptor(CapabilityDescriptor descriptor);

  Outcome<const CapabilityDescriptor*> Find(const CapabilityId& id) const;
  bool Contains(const CapabilityId& id) const;

  /// All descriptors, ordered by capability identifier.
  std::vector<CapabilityDescriptor> Descriptors() const;

  /// Namespaces declared by at least one descriptor.
  std::vector<CapabilityNamespaceId> Namespaces() const;

  std::size_t Size() const;

 private:
  mutable std::shared_mutex mutex_;
  std::unordered_map<CapabilityId, CapabilityDescriptor> descriptors_;
  std::size_t vendor_count_ = 0;
};

/// The process wide canonical schema. Vendors register extensions against
/// their own schema instance; this accessor returns the immutable built-in
/// table used for canonical capability lookups.
const CapabilitySchema& CanonicalSchema();

/// Canonical (vendor neutral) capability namespace identifiers, ordered.
const std::vector<CapabilityNamespaceId>& CanonicalNamespaces();

}  // namespace fabric::capability
