// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fabric/capability/evidence.hpp"
#include "fabric/capability/record.hpp"

namespace fabric::capability {

// ---------------------------------------------------------------------------
// Structured explanations. Operators must be able to ask why.
// ---------------------------------------------------------------------------

/// One ordered, machine coded step of an explanation.
struct ExplanationStep {
  std::string code;
  std::string summary;
  std::vector<std::pair<std::string, std::string>> fields;

  std::string ToText() const;
};

/// Structured explanation with a stable human rendering. Rendering is
/// deterministic: steps and fields keep insertion order and never contain
/// timestamps, pointers or process state.
struct Explanation {
  std::string code;
  std::string summary;
  std::vector<ExplanationStep> steps;

  bool Empty() const noexcept { return code.empty() && summary.empty() && steps.empty(); }
  std::string ToText() const;
};

// ---------------------------------------------------------------------------
// Capability queries.
// ---------------------------------------------------------------------------

/// Bounded summary of one evidence record attached to a query result.
struct EvidenceSummary {
  EvidenceId id;
  SourceId source;
  PublisherId publisher;
  AuthorityScopeId scope;
  WorkerBootId worker_boot;
  ProvenanceClass provenance = ProvenanceClass::SyntheticTestBackend;
  EvidenceSourceClass source_class = EvidenceSourceClass::SyntheticBackend;
  DurabilityClass durability = DurabilityClass::ProcessBound;
  Coverage coverage = Coverage::Partial;
  CapabilityState state = CapabilityState::Unknown;
  CapabilityValue value;
  bool has_value = false;
  EvidenceCurrentness currentness = EvidenceCurrentness::Current;
  EvidenceGeneration evidence_generation;
  SourceGeneration source_generation;
  CoordinatorEpoch epoch;
  VersionToken firmware_version;
  VersionToken driver_version;
  ReasonToken reason;
  bool winning = false;
  bool conflicting = false;
  bool outranked = false;
};

/// Answer to "what does this exact entity generation support now, and why".
/// Consumers never have to infer currentness from missing fields.
struct CapabilityQueryResult {
  EntityId entity;
  EntityGeneration entity_generation;
  CapabilityId capability;
  bool entity_known = false;
  bool record_exists = false;
  CapabilityState state = CapabilityState::Unknown;
  CapabilityValue value;
  bool has_value = false;
  RegistryGeneration registry_generation;
  CapabilitySetGeneration set_generation;
  CapabilityGeneration capability_generation;
  EvidenceGeneration evidence_generation;
  SourceGeneration source_generation;
  EvidenceId winning_evidence;
  ProvenanceClass provenance = ProvenanceClass::SyntheticTestBackend;
  EvidenceSourceClass source_class = EvidenceSourceClass::SyntheticBackend;
  Coverage coverage = Coverage::Partial;
  DurabilityClass durability = DurabilityClass::ProcessBound;
  bool actionable = false;
  bool fails_closed = true;
  std::size_t evidence_count = 0;
  std::size_t current_evidence_count = 0;
  std::size_t outranked_evidence_count = 0;
  std::size_t conflicting_evidence_count = 0;
  std::vector<EvidenceSummary> evidence;
  Explanation explanation;
};

/// Answer to a compatibility requirement evaluation.
enum class RequirementOutcome : std::uint8_t {
  Satisfied = 0,
  NotSatisfied,
  /// The registry cannot prove the requirement and the requirement demanded
  /// proof. Reported separately from NotSatisfied, and always fails closed.
  Undetermined,
};

std::string_view RequirementOutcomeName(RequirementOutcome outcome) noexcept;

// ---------------------------------------------------------------------------
// Deterministic diffs.
// ---------------------------------------------------------------------------

enum class CapabilityDiffKind : std::uint8_t {
  CapabilityAdded = 0,
  CapabilityRemoved,
  StateChanged,
  ValueChanged,
  ProvenanceChanged,
  EvidenceSuperseded,
  CurrentnessChanged,
  GenerationAdvanced,
  EntityGenerationChanged,
  EntityInvalidated,
};

std::string_view CapabilityDiffKindName(CapabilityDiffKind kind) noexcept;

struct CapabilityDiffEntry {
  CapabilityId capability;
  CapabilityDiffKind kind = CapabilityDiffKind::CapabilityAdded;
  CapabilityState before_state = CapabilityState::Unknown;
  CapabilityState after_state = CapabilityState::Unknown;
  CapabilityValue before_value;
  CapabilityValue after_value;
  bool has_before_value = false;
  bool has_after_value = false;
  ProvenanceClass before_provenance = ProvenanceClass::SyntheticTestBackend;
  ProvenanceClass after_provenance = ProvenanceClass::SyntheticTestBackend;
  CapabilityGeneration before_generation;
  CapabilityGeneration after_generation;
  std::string detail;
};

struct CapabilityDiff {
  EntityId entity;
  EntityGeneration before_entity_generation;
  EntityGeneration after_entity_generation;
  CapabilitySetGeneration before_set_generation;
  CapabilitySetGeneration after_set_generation;
  Digest before_digest;
  Digest after_digest;
  /// Ordered by capability identifier, then by diff kind.
  std::vector<CapabilityDiffEntry> entries;

  bool Empty() const noexcept { return entries.empty(); }
  std::string ToText() const;
};

// ---------------------------------------------------------------------------
// Immutable snapshots.
// ---------------------------------------------------------------------------

/// Selects the entities a snapshot covers.
struct SnapshotScope {
  bool all_entities = true;
  std::vector<EntityId> entities;
  std::vector<CapabilityNamespaceId> namespaces;
  std::vector<FabricEntityKind> entity_kinds;

  std::string ToText() const;
};

struct SnapshotEntityEntry {
  EntityId entity;
  EntityGeneration entity_generation;
  CapabilitySetGeneration set_generation;
  Digest set_digest;
  /// Immutable shared view of the resolved set; snapshots never expose
  /// mutable registry internals.
  std::shared_ptr<const EntityCapabilitySet> set;
};

/// Immutable capability snapshot. The digest binds registry generation,
/// coordinator epoch, per entity generations and resolved capability content.
struct CapabilitySnapshot {
  SnapshotId id;
  RegistryGeneration registry_generation;
  CoordinatorEpoch epoch;
  SnapshotScope scope;
  std::vector<SnapshotEntityEntry> entities;
  Digest digest;

  std::size_t EntityCount() const noexcept { return entities.size(); }
  const SnapshotEntityEntry* Find(const EntityId& entity) const noexcept;
  std::string ToText() const;
};

/// Result of checking whether a snapshot still describes current truth.
struct SnapshotCurrentness {
  bool current = false;
  std::vector<std::string> reasons;
  std::string ToText() const;
};

// ---------------------------------------------------------------------------
// Aggregate statistics.
// ---------------------------------------------------------------------------

struct RegistryStatistics {
  std::size_t entities = 0;
  std::size_t capability_records = 0;
  std::size_t evidence_records = 0;
  std::size_t supported = 0;
  std::size_t unsupported = 0;
  std::size_t unknown = 0;
  std::size_t revalidation_required = 0;
  std::size_t conflicted = 0;
  std::size_t authority_scopes = 0;
  std::size_t registered_publishers = 0;
  std::size_t fenced_worker_boots = 0;
  std::size_t vendor_descriptors = 0;
  std::size_t replay_entries = 0;
  std::size_t rejection_journal_entries = 0;
  std::size_t retired_generations = 0;
  RegistryGeneration registry_generation;
  CoordinatorEpoch epoch;
};

}  // namespace fabric::capability
