// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Internal durable store layout. This header is private to the library: the
// public API never exposes these structures. The payload is a deterministic
// sequence of length prefixed records; superseded evidence lineage is not
// persisted (replay protection is carried by the persisted source floors
// instead), and process bound evidence is reloaded as REVALIDATION_REQUIRED.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "fabric/capability/authority.hpp"
#include "fabric/capability/evidence.hpp"
#include "fabric/capability/ids.hpp"
#include "fabric/capability/value.hpp"

namespace fabric::capability::internal {

/// One capability of an entity, including capabilities whose claims were all
/// withdrawn. The generation is preserved so that a save/load round trip
/// reproduces the same canonical capability digest.
struct StoredCapability {
  CapabilityId id;
  CapabilityGeneration generation;
};

/// One live evidence record. Records that were superseded or belonged to a
/// retired entity generation are not persisted.
struct StoredClaim {
  CapabilityId capability;
  EvidenceId id;
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
};

/// Durable source floor: the highest source generation seen for one
/// (source, worker boot) pair on an entity. Prevents a stale publisher from
/// resurrecting an older claim after a restart.
struct StoredSourceFloor {
  SourceId source;
  WorkerBootId worker_boot;
  SourceGeneration generation;
};

struct StoredGeneration {
  EntityGeneration generation;
  CapabilitySetGeneration set_generation;
  Digest digest;
  std::uint64_t capability_count = 0;
  std::uint64_t supported_count = 0;
  RegistryGeneration retired_at;
  ReasonToken reason;
};

struct StoredEntity {
  EntityId id;
  EntityGeneration current_generation;
  EntityGeneration bound_generation;
  bool has_set = false;
  CapabilitySetGeneration set_generation;
  bool invalidated = false;
  ReasonToken invalidation_reason;
  std::vector<StoredCapability> capabilities;
  std::vector<StoredClaim> claims;
  std::vector<StoredSourceFloor> source_floors;
  std::vector<StoredGeneration> history;
};

struct StoredFence {
  WorkerBootId worker_boot;
  PublisherId publisher;
  ReasonToken reason;
};

struct StorePayload {
  CoordinatorEpoch epoch;
  RegistryGeneration registry_generation;
  std::vector<AuthorityGrant> authorities;
  std::vector<StoredFence> fences;
  std::vector<StoredEntity> entities;
};

}  // namespace fabric::capability::internal
