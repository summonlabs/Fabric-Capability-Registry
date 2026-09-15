// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "fabric/capability/digest.hpp"
#include "fabric/capability/ids.hpp"
#include "fabric/capability/state.hpp"
#include "fabric/capability/value.hpp"

namespace fabric::capability {

// ---------------------------------------------------------------------------
// Provenance. Evidence strength is an explicit ordered classification; it is
// never an opaque confidence score.
// ---------------------------------------------------------------------------

/// Ordered evidence strength. Lower numeric rank is stronger.
enum class ProvenanceClass : std::uint8_t {
  /// Direct current hardware enumeration of the bound entity generation.
  DirectHardwareEnumeration = 0,
  /// Direct driver or operating system device API observation.
  DirectDeviceOrOsApi = 1,
  /// Authoritative administrative declaration. Durable by nature.
  AuthoritativeAdministrativeDeclaration = 2,
  /// Vendor or firmware manifest describing the entity class.
  VendorFirmwareOrSdkManifest = 3,
  /// Imported static capability profile.
  ImportedStaticProfile = 4,
  /// Evidence inferred from other capability facts.
  Inferred = 5,
  /// Synthetic capability model. Never physical proof.
  SyntheticTestBackend = 6,
};

inline constexpr std::uint8_t kProvenanceClassCount = 7;

std::string_view ProvenanceClassName(ProvenanceClass provenance) noexcept;
Outcome<ProvenanceClass> ParseProvenanceClass(std::string_view text);
/// Deterministic rank; lower is stronger.
std::uint8_t ProvenanceRank(ProvenanceClass provenance) noexcept;
bool IsStrongerThan(ProvenanceClass lhs, ProvenanceClass rhs) noexcept;
bool IsDurableByNature(ProvenanceClass provenance) noexcept;

/// True for provenance classes that can only ever be a live observation of a
/// running process: direct hardware enumeration and direct driver or operating
/// system API evidence. Such evidence may never be declared durable, because a
/// durable declaration would assert that a live observation made by a process
/// is still current after that process is gone.
bool IsLiveProcessObservation(ProvenanceClass provenance) noexcept;

/// Where an observation physically came from.
enum class EvidenceSourceClass : std::uint8_t {
  HardwareEnumeration = 0,
  OperatingSystemApi,
  DriverApi,
  FirmwareManifest,
  VendorSdk,
  DeviceAgent,
  SwitchAgent,
  AdministrativeDeclaration,
  ImportedManifest,
  SyntheticBackend,
};

inline constexpr std::uint8_t kEvidenceSourceClassCount = 10;

std::string_view EvidenceSourceClassName(EvidenceSourceClass source) noexcept;
Outcome<EvidenceSourceClass> ParseEvidenceSourceClass(std::string_view text);

/// Strongest provenance class a given source class may claim. A publisher
/// that claims stronger provenance than its source class allows is rejected.
ProvenanceClass StrongestProvenanceForSource(EvidenceSourceClass source) noexcept;

/// Lifetime semantics of an evidence record.
enum class DurabilityClass : std::uint8_t {
  /// Bound to the publishing process and coordinator epoch. Becomes
  /// REVALIDATION_REQUIRED after a coordinator restart or a fence.
  ProcessBound = 0,
  /// Administrative declaration that remains durable across restarts.
  Durable,
};

std::string_view DurabilityClassName(DurabilityClass durability) noexcept;

/// How complete the observation was.
enum class Coverage : std::uint8_t {
  /// Only the mentioned capabilities were observed.
  Partial = 0,
  /// The source enumerated the complete capability surface it can see.
  FullEnumeration,
};

std::string_view CoverageName(Coverage coverage) noexcept;

/// Currentness of an evidence record.
enum class EvidenceCurrentness : std::uint8_t {
  Current = 0,
  /// Retained but not current enough to act on (coordinator restart).
  RevalidationRequired,
  /// Publisher WorkerBootId was fenced.
  Fenced,
  /// Replaced by a newer claim from the same source.
  Superseded,
  /// The bound entity generation was superseded by Fabric Registry.
  EntityGenerationSuperseded,
};

std::string_view EvidenceCurrentnessName(EvidenceCurrentness currentness) noexcept;
bool IsCurrent(EvidenceCurrentness currentness) noexcept;

/// One immutable observation. Evidence is provenance, never authority: the
/// resolved capability state is computed from evidence by the registry.
struct EvidenceRecord {
  EvidenceId id;
  CapabilityId capability;
  EntityId entity;
  EntityGeneration entity_generation;
  SourceId source;
  PublisherId publisher;
  AuthorityScopeId scope;
  WorkerBootId worker_boot;
  CoordinatorEpoch epoch;
  SourceGeneration source_generation;
  EvidenceGeneration evidence_generation;
  MutationAttemptId attempt;
  PublicationId publication;
  ProvenanceClass provenance = ProvenanceClass::SyntheticTestBackend;
  EvidenceSourceClass source_class = EvidenceSourceClass::SyntheticBackend;
  DurabilityClass durability = DurabilityClass::ProcessBound;
  Coverage coverage = Coverage::Partial;
  CapabilityState state = CapabilityState::Unknown;
  CapabilityValue value;
  VersionToken firmware_version;
  VersionToken driver_version;
  ReasonToken reason;
  EvidenceCurrentness currentness = EvidenceCurrentness::Current;
  RegistryGeneration accepted_generation;
  /// Local arrival ordering. Excluded from every semantic digest.
  std::uint64_t arrival_sequence = 0;

  /// Semantic digest of the record (excludes local ordering and derived
  /// currentness detail).
  Digest SemanticDigest() const;

  /// Deterministic multi-line rendering for diagnostics and the CLI.
  std::string ToText() const;
};

using EvidencePtr = std::shared_ptr<const EvidenceRecord>;

/// Deterministic evidence identifier derived from the publishing identity and
/// the semantic position of the observation.
EvidenceId MakeEvidenceId(const PublisherId& publisher, const WorkerBootId& boot,
                          const MutationAttemptId& attempt, const EntityId& entity,
                          EntityGeneration entity_generation, const CapabilityId& capability,
                          std::uint32_t ordinal);

/// Deterministic capability record identifier for an entity generation and
/// capability.
CapabilityRecordId MakeCapabilityRecordId(const EntityId& entity,
                                          EntityGeneration entity_generation,
                                          const CapabilityId& capability);

}  // namespace fabric::capability
