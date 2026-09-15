// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "fabric/capability/evidence.hpp"
#include "fabric/capability/ids.hpp"

namespace fabric::capability {

// ---------------------------------------------------------------------------
// Publication modes.
// ---------------------------------------------------------------------------

enum class PublicationMode : std::uint8_t {
  /// Authoritative replacement of the publisher's evidence for the entity
  /// generation. Capabilities not mentioned are withdrawn for that publisher
  /// (and, for an exclusive authority, for every publisher).
  FullSnapshot = 1,
  /// Generation bound add/update/withdraw operations.
  Incremental = 2,
  /// Observation that covers only part of the surface. Capabilities that are
  /// not mentioned are left completely untouched.
  PartialObservation = 4,
};

using PublicationModeMask = std::uint8_t;

constexpr PublicationModeMask ModeBit(PublicationMode mode) noexcept {
  return static_cast<PublicationModeMask>(mode);
}
constexpr bool AllowsMode(PublicationModeMask mask, PublicationMode mode) noexcept {
  return (mask & ModeBit(mode)) != 0;
}
std::string_view PublicationModeName(PublicationMode mode) noexcept;
std::string PublicationModeMaskText(PublicationModeMask mask);

// ---------------------------------------------------------------------------
// Authority.
// ---------------------------------------------------------------------------

/// Declarative authority scope. Authority is explicit and enumerative: there
/// is no wildcard namespace grant, and a publisher may only publish inside
/// the scopes it is bound to.
struct AuthorityGrant {
  AuthorityScopeId scope;
  /// Entity classes this scope may publish for. Empty means no entity class.
  std::vector<FabricEntityKind> entity_kinds;
  /// Capability namespaces this scope may publish into. Empty means none.
  std::vector<CapabilityNamespaceId> namespaces;
  PublicationModeMask modes = 0;
  /// When true the scope may publish an authoritative full snapshot that
  /// supersedes evidence owned by other scopes.
  bool exclusive = false;
  /// When true the scope may publish durable administrative declarations.
  bool may_publish_durable = false;
  /// Strongest provenance class the scope may claim.
  ProvenanceClass strongest_provenance = ProvenanceClass::DirectHardwareEnumeration;
  /// Optional publisher allowlist. Empty means every publisher that presents a
  /// valid boot identity may register inside this scope; a non empty list
  /// restricts registration to exactly those publisher identifiers.
  std::vector<PublisherId> allowed_publishers;
  std::size_t max_claims_per_publication = limits::kMaxClaimsPerPublication;
  ReasonToken description;
};

/// A publisher bound to a boot identity inside one authority scope.
struct PublisherRegistration {
  PublisherId publisher;
  AuthorityScopeId scope;
  WorkerBootId worker_boot;
  CoordinatorEpoch epoch;
  RegistryGeneration accepted_generation;
};

/// Identity of a publisher instance for one mutation.
struct AuthorityContext {
  AuthorityScopeId scope;
  PublisherId publisher;
  WorkerBootId worker_boot;
  SourceId source;
  SourceGeneration source_generation;
};

/// Reason a publisher identity was rejected, rendered deterministically.
struct FenceRecord {
  WorkerBootId worker_boot;
  PublisherId publisher;
  ReasonToken reason;
  RegistryGeneration fenced_at;
};

}  // namespace fabric::capability
