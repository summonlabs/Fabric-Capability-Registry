// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "fabric/capability/authority.hpp"
#include "fabric/capability/evidence.hpp"
#include "fabric/capability/ids.hpp"
#include "fabric/capability/persistence.hpp"
#include "fabric/capability/publication.hpp"
#include "fabric/capability/query.hpp"
#include "fabric/capability/record.hpp"
#include "fabric/capability/requirement.hpp"
#include "fabric/capability/schema.hpp"

namespace fabric::capability {

/// Authoritative capability knowledge runtime.
///
/// Boundary: this registry owns capability truth (what an exact entity
/// generation may support, with which provenance and constraints) and nothing
/// else. It does not own identity (Fabric Registry), topology (Fabric
/// Topology), live link condition (Link State Fabric), port configuration
/// (Port Fabric), queue or buffer runtime state, congestion state or policy.
///
/// Thread safety: every public method is safe to call concurrently from any
/// thread. Reads take a shared lock, mutations take an exclusive lock, and no
/// public method is called while a lock is held. Returned objects are
/// immutable value copies or c std::shared_ptr to immutable sets; no mutable
/// internal state escapes.
class CapabilityRegistry {
 public:
  struct Options {
    std::size_t max_entities = limits::kMaxEntities;
    std::size_t max_capabilities_per_entity = limits::kMaxCapabilitiesPerEntity;
    std::size_t max_evidence_per_capability = limits::kMaxEvidencePerCapability;
    std::size_t max_evidence_per_entity = limits::kMaxEvidencePerEntity;
    std::size_t max_entity_generations_retained = limits::kMaxEntityGenerationsRetained;
    std::size_t max_replay_entries_per_publisher = limits::kMaxReplayEntriesPerPublisher;
    std::size_t max_rejection_journal_entries = limits::kMaxRejectionJournalEntries;
    std::size_t max_fenced_worker_boots = limits::kMaxFencedWorkerBoots;
    std::size_t max_publishers = 4096;
    std::size_t max_authority_scopes = 4096;
    CoordinatorEpoch initial_epoch = CoordinatorEpoch::FromValue(1);
  };

  explicit CapabilityRegistry(Options options = {});
  ~CapabilityRegistry();
  CapabilityRegistry(const CapabilityRegistry&) = delete;
  CapabilityRegistry& operator=(const CapabilityRegistry&) = delete;
  CapabilityRegistry(CapabilityRegistry&&) = delete;
  CapabilityRegistry& operator=(CapabilityRegistry&&) = delete;

  // --- schema -------------------------------------------------------------

  CapabilitySchema& Schema() noexcept { return schema_; }
  const CapabilitySchema& Schema() const noexcept { return schema_; }
  /// Registers a vendor extension descriptor in the registry schema.
  Outcome<void> RegisterVendorDescriptor(CapabilityDescriptor descriptor);

  // --- coordinator epoch --------------------------------------------------

  CoordinatorEpoch CurrentEpoch() const;
  /// Advances the coordinator epoch. Every process bound evidence record
  /// becomes REVALIDATION_REQUIRED; durable administrative declarations are
  /// retained; every live publisher registration is dropped; the registry
  /// generation advances once.
  Outcome<CoordinatorEpoch> AdvanceCoordinatorEpoch(ReasonToken reason = ReasonToken{});

  // --- authority ----------------------------------------------------------

  Outcome<void> DeclareAuthority(const AuthorityGrant& grant);
  Outcome<AuthorityGrant> Authority(const AuthorityScopeId& scope) const;
  std::vector<AuthorityGrant> Authorities() const;

  /// Binds a publisher instance to a boot identity inside a scope. The
  /// registration is bound to the current coordinator epoch.
  Outcome<PublisherRegistration> RegisterPublisher(const PublisherId& publisher,
                                                   const AuthorityScopeId& scope,
                                                   const WorkerBootId& worker_boot);
  /// Drops a live publisher registration without fencing its boot identity.
  Outcome<void> RetirePublisher(const PublisherId& publisher, const WorkerBootId& worker_boot);
  /// Permanently fences a worker boot identity. Every evidence record owned
  /// by that boot becomes non-current, and no later publication from it is
  /// accepted.
  Outcome<void> FenceWorkerBoot(const WorkerBootId& worker_boot, ReasonToken reason);
  bool IsWorkerBootFenced(const WorkerBootId& worker_boot) const;
  std::vector<FenceRecord> Fences() const;
  std::optional<PublisherRegistration> FindPublisher(const PublisherId& publisher,
                                                     const WorkerBootId& worker_boot) const;

  // --- entity lifecycle ---------------------------------------------------

  /// Records an entity generation observed from Fabric Registry. Advancing a
  /// generation retires the previous one: its capability claims are retained
  /// as history only and never become current for the new generation.
  Outcome<EntityGeneration> ObserveEntityGeneration(const EntityId& entity,
                                                    EntityGeneration generation,
                                                    ReasonToken reason = ReasonToken{});
  /// Fences every capability of one entity generation: all claims become
  /// non-current and capability state resolves to REVALIDATION_REQUIRED or
  /// UNKNOWN. Used when Fabric Topology invalidates the structural
  /// relationship a capability depends on.
  Outcome<void> InvalidateEntity(const EntityId& entity, EntityGeneration generation,
                                 ReasonToken reason);
  /// Mass source invalidation: every evidence record of one source becomes
  /// non-current.
  Outcome<std::size_t> InvalidateSource(const SourceId& source, ReasonToken reason);

  // --- publication --------------------------------------------------------

  /// Applies one publication. The request is fully validated before any state
  /// changes; a rejection leaves the registry exactly as it was.
  Outcome<PublicationResult> Publish(const PublicationRequest& request);
  /// Applies publications in order. Each entry is validated and committed
  /// independently; the returned vector has one result per request and the
  /// call fails only when the batch itself is malformed or oversized.
  Outcome<std::vector<PublicationResult>> PublishBatch(
      std::span<const PublicationRequest> requests);

  // --- queries ------------------------------------------------------------

  Outcome<CapabilityQueryResult> Query(const EntityId& entity, const CapabilityId& capability) const;
  Outcome<CapabilityQueryResult> Query(const EntityId& entity, EntityGeneration generation,
                                       const CapabilityId& capability) const;
  Outcome<EntityCapabilitySet> QueryEntity(const EntityId& entity) const;
  Outcome<EntityCapabilitySet> QueryEntity(const EntityId& entity,
                                           EntityGeneration generation) const;
  Outcome<EntityRecordView> EntityRecord(const EntityId& entity) const;
  Outcome<RetiredGenerationView> RetiredGeneration(const EntityId& entity,
                                                   EntityGeneration generation) const;
  std::vector<EntityId> Entities() const;
  std::vector<EntityId> EntitiesSupporting(const CapabilityId& capability) const;
  std::vector<EntityId> EntitiesNotSupporting(const CapabilityId& capability) const;
  std::vector<EntityId> EntitiesWithState(CapabilityState state) const;
  std::vector<CapabilityId> CapabilitiesInNamespace(const CapabilityNamespaceId& ns) const;
  std::vector<std::pair<EntityId, CapabilityId>> CapabilitiesRequiringRevalidation() const;
  std::vector<EvidenceSummary> EvidenceFor(const EntityId& entity,
                                           const CapabilityId& capability) const;

  // --- requirement evaluation ---------------------------------------------

  RequirementEvaluation Evaluate(const EntityId& entity, const Requirement& requirement) const;

  // --- snapshots, diffs, explanations -------------------------------------

  Outcome<CapabilitySnapshot> CreateSnapshot(const SnapshotScope& scope) const;
  SnapshotCurrentness CheckSnapshot(const CapabilitySnapshot& snapshot) const;
  Outcome<CapabilityDiff> DiffAgainstSnapshot(const CapabilitySnapshot& snapshot) const;
  Outcome<CapabilityDiff> DiffEntityAgainstSnapshot(const CapabilitySnapshot& snapshot,
                                                    const EntityId& entity) const;
  Explanation ExplainCapability(const EntityId& entity, const CapabilityId& capability) const;
  Outcome<Explanation> ExplainPublication(const PublicationId& publication) const;
  Outcome<Explanation> ExplainReplay(const MutationAttemptId& attempt) const;
  Outcome<Explanation> ExplainGeneration(const EntityId& entity,
                                         CapabilitySetGeneration generation) const;

  // --- digests and statistics ---------------------------------------------

  /// Canonical digest of the complete semantic capability state. Independent
  /// of insertion order and of local process state.
  Digest ComputeDigest() const;
  RegistryGeneration Generation() const;
  RegistryStatistics Statistics() const;

  // --- persistence --------------------------------------------------------

  /// Persists durable capability knowledge atomically. The registry is not
  /// modified.
  Outcome<SaveReport> Save(const PersistenceConfig& config) const;
  /// Replaces registry content from a persisted store. Process bound evidence
  /// is loaded as REVALIDATION_REQUIRED; durable declarations stay durable.
  Outcome<LoadReport> Load(const PersistenceConfig& config);

 private:
  class Impl;

  /// Governed capability schema owned by this registry.
  CapabilitySchema schema_;
  std::unique_ptr<Impl> impl_;
};

/// Inspects a persisted store without materialising a registry.
Outcome<StoreInspection> InspectStore(const PersistenceConfig& config);

}  // namespace fabric::capability
