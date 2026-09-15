// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "fabric/capability/authority.hpp"
#include "fabric/capability/query.hpp"
#include "fabric/capability/record.hpp"

namespace fabric::capability {

/// Bound operation inside an incremental publication.
enum class IncrementalOperation : std::uint8_t {
  /// Add or replace the source's claim for the capability.
  UpsertClaim = 0,
  /// Withdraw the source's claim. The capability is recomputed from the
  /// remaining evidence and becomes UNKNOWN when nothing remains.
  WithdrawClaim,
  /// Explicitly declare the capability UNSUPPORTED (authoritative absence).
  MarkUnsupported,
  /// Explicitly declare the capability UNKNOWN (insufficient evidence).
  MarkUnknown,
  /// Explicitly declare that prior evidence requires revalidation.
  MarkRevalidationRequired,
  /// Supersede every evidence record this source holds for the entity.
  SupersedeSourceEvidence,
};

std::string_view IncrementalOperationName(IncrementalOperation operation) noexcept;
Outcome<IncrementalOperation> ParseIncrementalOperation(std::string_view text);

struct IncrementalEdit {
  IncrementalOperation operation = IncrementalOperation::UpsertClaim;
  CapabilityId capability;
  /// Used by UpsertClaim, MarkUnsupported, MarkUnknown,
  /// MarkRevalidationRequired.
  CapabilityClaim claim;
};

/// A complete publication request. Every field is validated before any state
/// is touched; a rejected publication leaves no partial capability set.
struct PublicationRequest {
  PublicationMode mode = PublicationMode::PartialObservation;
  MutationAttemptId attempt;
  PublicationId publication;
  CoordinatorEpoch epoch;
  AuthorityContext authority;
  EntityId entity;
  EntityGeneration entity_generation;
  /// Expected capability set generation. Zero means "no set exists yet";
  /// the publication is rejected when a set already exists.
  CapabilitySetGeneration expected_set_generation;
  /// Capabilities published by a FullSnapshot or PartialObservation.
  std::vector<CapabilityClaim> claims;
  /// Edits applied by an Incremental publication.
  std::vector<IncrementalEdit> edits;
  /// Coverage of a FullSnapshot or PartialObservation publication.
  Coverage coverage = Coverage::Partial;
  ReasonToken reason;
};

/// Deterministic outcome classification of a publication.
enum class PublicationStatus : std::uint8_t {
  /// The publication changed registry state; generations advanced.
  Committed = 0,
  /// Exact replay of an already committed attempt. No generation advanced.
  IdempotentReplay,
  /// The publication was rejected; registry state is unchanged.
  Rejected,
};

std::string_view PublicationStatusName(PublicationStatus status) noexcept;

/// Result of one publication attempt.
struct PublicationResult {
  PublicationStatus status = PublicationStatus::Rejected;
  ErrorCode code = ErrorCode::Ok;
  std::string message;
  PublicationId publication;
  MutationAttemptId attempt;
  EntityId entity;
  EntityGeneration entity_generation;
  CapabilitySetGeneration previous_set_generation;
  CapabilitySetGeneration new_set_generation;
  RegistryGeneration registry_generation;
  std::size_t claims_applied = 0;
  std::size_t claims_withdrawn = 0;
  std::size_t capabilities_added = 0;
  std::size_t capabilities_removed = 0;
  std::size_t capabilities_changed = 0;
  CapabilityDiff diff;
  Explanation explanation;
  Digest set_digest;

  bool Committed() const noexcept { return status == PublicationStatus::Committed; }
  bool Idempotent() const noexcept { return status == PublicationStatus::IdempotentReplay; }
  bool Rejected() const noexcept { return status == PublicationStatus::Rejected; }
  std::string ToText() const;
};

/// Report of a persisted store load.
struct LoadReport {
  std::size_t entities_loaded = 0;
  std::size_t evidence_loaded = 0;
  std::size_t evidence_revalidation_required = 0;
  std::size_t durable_evidence_retained = 0;
  std::size_t fenced_worker_boots = 0;
  std::size_t authority_grants = 0;
  std::size_t generations_recovered = 0;
  CoordinatorEpoch epoch;
  RegistryGeneration registry_generation;
  Digest store_digest;
  bool created_empty = false;
};

}  // namespace fabric::capability
