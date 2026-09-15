// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "fabric/capability/digest.hpp"
#include "fabric/capability/evidence.hpp"
#include "fabric/capability/ids.hpp"
#include "fabric/capability/value.hpp"

namespace fabric::capability {

/// One capability assertion submitted by a publisher.
struct CapabilityClaim {
  CapabilityId capability;
  CapabilityState state = CapabilityState::Unknown;
  CapabilityValue value;
  ProvenanceClass provenance = ProvenanceClass::SyntheticTestBackend;
  EvidenceSourceClass source_class = EvidenceSourceClass::SyntheticBackend;
  DurabilityClass durability = DurabilityClass::ProcessBound;
  Coverage coverage = Coverage::Partial;
  VersionToken firmware_version;
  VersionToken driver_version;
  ReasonToken reason;
};

/// Resolved capability truth for one capability of one entity generation.
/// This is the object consumers read; it never exposes mutable internals.
struct CapabilityResolution {
  CapabilityId capability;
  CapabilityState state = CapabilityState::Unknown;
  CapabilityValue value;
  bool has_value = false;
  Digest value_digest;
  EvidenceId winning_evidence;
  ProvenanceClass provenance = ProvenanceClass::SyntheticTestBackend;
  EvidenceSourceClass source_class = EvidenceSourceClass::SyntheticBackend;
  Coverage coverage = Coverage::Partial;
  DurabilityClass durability = DurabilityClass::ProcessBound;
  EvidenceGeneration evidence_generation;
  SourceGeneration source_generation;
  CapabilityGeneration capability_generation;
  std::size_t evidence_count = 0;
  std::size_t current_evidence_count = 0;
  std::size_t outranked_evidence_count = 0;
  std::size_t conflicting_evidence_count = 0;
  bool has_non_current_evidence = false;
  std::vector<EvidenceId> conflicting_evidence;
  CapabilityRecordId record_id;

  bool Actionable() const noexcept { return IsActionableSupport(state); }
  bool FailsClosedState() const noexcept { return FailsClosed(state); }
};

/// Immutable resolved capability set of one entity generation.
struct EntityCapabilitySet {
  EntityId entity;
  EntityGeneration entity_generation;
  CapabilitySetGeneration set_generation;
  RegistryGeneration registry_generation;
  bool invalidated = false;
  ReasonToken invalidation_reason;
  /// Sorted by capability identifier.
  std::vector<CapabilityResolution> capabilities;
  Digest digest;

  std::size_t Size() const noexcept { return capabilities.size(); }
  std::size_t CountOf(CapabilityState state) const noexcept;
  const CapabilityResolution* Find(const CapabilityId& capability) const noexcept;
  std::string ToText() const;
};

/// Bounded historical summary of a retired entity generation. The full claim
/// surface of a retired generation is preserved only as a digest and a
/// capability identifier list; capability truth of a retired generation is
/// never served as current.
struct EntityGenerationSummary {
  EntityGeneration generation;
  CapabilitySetGeneration set_generation;
  Digest digest;
  std::size_t capability_count = 0;
  std::size_t supported_count = 0;
  RegistryGeneration retired_at;
  ReasonToken reason;
};

/// Everything the registry knows about one entity identity.
struct EntityRecordView {
  EntityId entity;
  EntityGeneration current_generation;
  bool has_current_set = false;
  EntityCapabilitySet current_set;
  /// Newest first, bounded by limits::kMaxEntityGenerationsRetained.
  std::vector<EntityGenerationSummary> history;
  bool invalidated = false;
  ReasonToken invalidation_reason;
  std::vector<EvidenceId> recent_evidence;
};

/// Resolved capability state of one retired entity generation.
struct RetiredGenerationView {
  EntityId entity;
  EntityGeneration generation;
  CapabilitySetGeneration set_generation;
  Digest digest;
  std::vector<CapabilityId> capabilities;
  bool supported_known = false;
  std::size_t supported_count = 0;
};

}  // namespace fabric::capability
