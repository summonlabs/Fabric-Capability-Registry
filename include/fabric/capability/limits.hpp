// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstddef>
#include <cstdint>

namespace fabric::capability::limits {

// ---------------------------------------------------------------------------
// Identifier and text bounds.
// ---------------------------------------------------------------------------

/// Maximum length of a fully rendered capability identifier
/// ("fabric.<domain>.<name>" or "vendor.<vendor>.<domain>.<name>").
inline constexpr std::size_t kMaxCapabilityIdLength = 192;

/// Maximum length of a single namespace segment (domain, vendor, ...).
inline constexpr std::size_t kMaxNamespaceSegmentLength = 32;

/// Maximum number of dot separated segments in a capability identifier.
inline constexpr std::size_t kMaxCapabilityIdSegments = 6;

/// Maximum length of the local (capability name) portion.
inline constexpr std::size_t kMaxCapabilityLocalLength = 64;

/// Maximum length of a canonical entity name as issued by Fabric Registry.
inline constexpr std::size_t kMaxEntityNameLength = 128;

/// Maximum length of a rendered entity identifier ("switch:<name>").
inline constexpr std::size_t kMaxEntityIdLength = 160;

/// Maximum length of free-form bounded text (reasons, descriptions).
inline constexpr std::size_t kMaxTextLength = 192;

/// Maximum length of a firmware/driver version token.
inline constexpr std::size_t kMaxVersionTokenLength = 64;

/// Maximum length of a source, publisher, scope or profile identifier.
inline constexpr std::size_t kMaxSourceIdLength = 96;
inline constexpr std::size_t kMaxPublisherIdLength = 96;
inline constexpr std::size_t kMaxAuthorityScopeIdLength = 96;
inline constexpr std::size_t kMaxProfileIdLength = 96;
inline constexpr std::size_t kMaxCompatibilityClassIdLength = 96;
inline constexpr std::size_t kMaxAttemptIdLength = 96;
inline constexpr std::size_t kMaxPublicationIdLength = 96;
inline constexpr std::size_t kMaxEnumDomainIdLength = 96;

/// Number of characters in a digest-derived identifier (128 bit hex prefix).
inline constexpr std::size_t kDigestIdLength = 32;

// ---------------------------------------------------------------------------
// Cardinality bounds. All are enforced before allocation.
// ---------------------------------------------------------------------------

inline constexpr std::size_t kMaxEntities = 1'000'000;
inline constexpr std::size_t kMaxCapabilitiesPerEntity = 4096;
inline constexpr std::size_t kMaxClaimsPerPublication = 4096;
inline constexpr std::size_t kMaxPublicationsPerBatch = 1024;
inline constexpr std::size_t kMaxSetCardinality = 4096;
inline constexpr std::size_t kMaxBitSetBits = 4096;
inline constexpr std::size_t kMaxTupleArity = 8;
inline constexpr std::size_t kMaxRecordFields = 32;
inline constexpr std::size_t kMaxTupleSetCardinality = 1024;
inline constexpr std::size_t kMaxExtensionPayloadBytes = 65536;
inline constexpr std::size_t kMaxEvidencePerCapability = 64;
inline constexpr std::size_t kMaxEvidencePerEntity = 65536;
inline constexpr std::size_t kMaxVersionComponents = 4;
inline constexpr std::size_t kMaxProtocolsInSet = 64;
inline constexpr std::size_t kMaxEnumCodes = 4096;
inline constexpr std::size_t kMaxVendorDescriptors = 1024;
inline constexpr std::size_t kMaxProfiles = 1024;
inline constexpr std::size_t kMaxEntityGenerationsRetained = 8;
/// Capability identifiers retained per retired entity generation. Beyond this
/// bound a retired generation keeps its digest and counts only.
inline constexpr std::size_t kMaxRetainedCapabilityIds = 64;
inline constexpr std::size_t kMaxReplayEntriesPerPublisher = 128;
inline constexpr std::size_t kMaxFencedWorkerBoots = 65536;
inline constexpr std::size_t kMaxRejectionJournalEntries = 4096;
inline constexpr std::size_t kMaxRequirementDepth = 8;
inline constexpr std::size_t kMaxRequirementNodes = 256;
inline constexpr std::size_t kMaxAuthorityNamespaces = 64;
inline constexpr std::size_t kMaxAuthorityEntityKinds = 16;
inline constexpr std::size_t kMaxSnapshotEntities = 1'000'000;
inline constexpr std::size_t kMaxDiffEntries = 1'000'000;
inline constexpr std::size_t kMaxTextFieldsPerExplanation = 64;

// ---------------------------------------------------------------------------
// Numeric bounds.
// ---------------------------------------------------------------------------

/// Largest quantity accepted for any capability quantity or numeric set
/// element. Chosen so that unit conversions and comparisons cannot overflow
/// 64 bit arithmetic in the value model.
inline constexpr std::uint64_t kMaxQuantity = (1ull << 62);

/// Largest signed integer accepted for the integer value kind.
inline constexpr std::int64_t kMaxInteger = (1ll << 61);
inline constexpr std::int64_t kMinInteger = -(1ll << 61);

/// Largest version component.
inline constexpr std::uint32_t kMaxVersionComponent = 1'000'000'000u;

// ---------------------------------------------------------------------------
// Persistence bounds.
// ---------------------------------------------------------------------------

inline constexpr std::size_t kMaxPersistenceBytes = 1ull << 31;  // 2 GiB
inline constexpr std::size_t kMaxPersistenceRecords = 4'000'000;
inline constexpr std::size_t kMaxPersistenceEvidenceRecords = 8'000'000;
inline constexpr std::size_t kPersistenceDigestBytes = 32;
inline constexpr std::size_t kPersistencePathLength = 200;

// ---------------------------------------------------------------------------
// Wire protocol bounds.
// ---------------------------------------------------------------------------

inline constexpr std::size_t kMaxFrameBytes = 1u << 20;  // 1 MiB
inline constexpr std::uint32_t kMaxFrameRequestsInFlight = 64;

}  // namespace fabric::capability::limits
