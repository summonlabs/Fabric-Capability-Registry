// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/capability/error.hpp"

namespace fabric::capability {

std::string_view ErrorCodeName(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok:
      return "ok";
    case ErrorCode::InvalidArgument:
      return "invalid-argument";
    case ErrorCode::MalformedIdentifier:
      return "malformed-identifier";
    case ErrorCode::MalformedEncoding:
      return "malformed-encoding";
    case ErrorCode::MalformedValue:
      return "malformed-value";
    case ErrorCode::UnknownNamespace:
      return "unknown-namespace";
    case ErrorCode::UnknownCapability:
      return "unknown-capability";
    case ErrorCode::UnknownEnumDomain:
      return "unknown-enum-domain";
    case ErrorCode::UnsupportedValueKind:
      return "unsupported-value-kind";
    case ErrorCode::SchemaViolation:
      return "schema-violation";
    case ErrorCode::ValueTypeMismatch:
      return "value-type-mismatch";
    case ErrorCode::ValueOutOfRange:
      return "value-out-of-range";
    case ErrorCode::ValueContradiction:
      return "value-contradiction";
    case ErrorCode::ValueNotCanonical:
      return "value-not-canonical";
    case ErrorCode::DuplicateValue:
      return "duplicate-value";
    case ErrorCode::EmptyCollection:
      return "empty-collection";
    case ErrorCode::TooManyItems:
      return "too-many-items";
    case ErrorCode::TextTooLong:
      return "text-too-long";
    case ErrorCode::PayloadTooLarge:
      return "payload-too-large";
    case ErrorCode::ArithmeticOverflow:
      return "arithmetic-overflow";
    case ErrorCode::NestingTooDeep:
      return "nesting-too-deep";
    case ErrorCode::UnknownEntity:
      return "unknown-entity";
    case ErrorCode::UnknownEntityGeneration:
      return "unknown-entity-generation";
    case ErrorCode::EntityGenerationStale:
      return "entity-generation-stale";
    case ErrorCode::EntityGenerationSuperseded:
      return "entity-generation-superseded";
    case ErrorCode::CapabilitySetGenerationStale:
      return "capability-set-generation-stale";
    case ErrorCode::CapabilitySetGenerationMismatch:
      return "capability-set-generation-mismatch";
    case ErrorCode::EvidenceGenerationStale:
      return "evidence-generation-stale";
    case ErrorCode::SourceGenerationStale:
      return "source-generation-stale";
    case ErrorCode::UnknownPublisher:
      return "unknown-publisher";
    case ErrorCode::UnknownAuthorityScope:
      return "unknown-authority-scope";
    case ErrorCode::UnauthorizedPublisher:
      return "unauthorized-publisher";
    case ErrorCode::AuthorityScopeViolation:
      return "authority-scope-violation";
    case ErrorCode::PublicationModeNotAuthorized:
      return "publication-mode-not-authorized";
    case ErrorCode::ProvenanceNotAuthorized:
      return "provenance-not-authorized";
    case ErrorCode::DurablePublicationNotAuthorized:
      return "durable-publication-not-authorized";
    case ErrorCode::ExclusivityRequired:
      return "exclusivity-required";
    case ErrorCode::WorkerBootUnknown:
      return "worker-boot-unknown";
    case ErrorCode::WorkerBootStale:
      return "worker-boot-stale";
    case ErrorCode::WorkerBootFenced:
      return "worker-boot-fenced";
    case ErrorCode::CoordinatorEpochStale:
      return "coordinator-epoch-stale";
    case ErrorCode::CoordinatorEpochUnknown:
      return "coordinator-epoch-unknown";
    case ErrorCode::DuplicateClaim:
      return "duplicate-claim";
    case ErrorCode::ConflictingClaim:
      return "conflicting-claim";
    case ErrorCode::ClaimCountExceeded:
      return "claim-count-exceeded";
    case ErrorCode::EvidenceLimitExceeded:
      return "evidence-limit-exceeded";
    case ErrorCode::StaleReplay:
      return "stale-replay";
    case ErrorCode::DuplicateAttemptConflict:
      return "duplicate-attempt-conflict";
    case ErrorCode::EntityInvalidated:
      return "entity-invalidated";
    case ErrorCode::EntityGenerationAdvanceRequired:
      return "entity-generation-advance-required";
    case ErrorCode::NotFound:
      return "not-found";
    case ErrorCode::PersistencePathInvalid:
      return "persistence-path-invalid";
    case ErrorCode::PersistenceIoFailure:
      return "persistence-io-failure";
    case ErrorCode::PersistenceFormatInvalid:
      return "persistence-format-invalid";
    case ErrorCode::PersistenceVersionUnsupported:
      return "persistence-version-unsupported";
    case ErrorCode::PersistenceIntegrityFailure:
      return "persistence-integrity-failure";
    case ErrorCode::PersistenceCorrupt:
      return "persistence-corrupt";
    case ErrorCode::PersistenceDuplicateRecord:
      return "persistence-duplicate-record";
    case ErrorCode::PersistenceLimitExceeded:
      return "persistence-limit-exceeded";
    case ErrorCode::TransportUnavailable:
      return "transport-unavailable";
    case ErrorCode::TransportFailure:
      return "transport-failure";
    case ErrorCode::TransportClosed:
      return "transport-closed";
    case ErrorCode::FrameMalformed:
      return "frame-malformed";
    case ErrorCode::FrameTooLarge:
      return "frame-too-large";
    case ErrorCode::FrameVersionUnsupported:
      return "frame-version-unsupported";
    case ErrorCode::FrameIntegrityFailure:
      return "frame-integrity-failure";
    case ErrorCode::FrameTrailingBytes:
      return "frame-trailing-bytes";
    case ErrorCode::FrameTypeUnknown:
      return "frame-type-unknown";
    case ErrorCode::FrameStateViolation:
      return "frame-state-violation";
    case ErrorCode::DiscoveryUnavailable:
      return "discovery-unavailable";
    case ErrorCode::DiscoveryFailed:
      return "discovery-failed";
    case ErrorCode::Unsupported:
      return "unsupported";
    case ErrorCode::InternalFailure:
      return "internal-failure";
  }
  return "internal-failure";
}

std::string Error::ToString() const {
  std::string text(ErrorCodeName(code));
  if (!message.empty()) {
    text.append(": ").append(message);
  }
  if (!detail.empty()) {
    text.append(" (").append(detail).append(")");
  }
  return text;
}

bool IsStaleRejection(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::EntityGenerationStale:
    case ErrorCode::EntityGenerationSuperseded:
    case ErrorCode::CapabilitySetGenerationStale:
    case ErrorCode::CapabilitySetGenerationMismatch:
    case ErrorCode::EvidenceGenerationStale:
    case ErrorCode::SourceGenerationStale:
    case ErrorCode::WorkerBootStale:
    case ErrorCode::WorkerBootFenced:
    case ErrorCode::CoordinatorEpochStale:
    case ErrorCode::CoordinatorEpochUnknown:
    case ErrorCode::StaleReplay:
    case ErrorCode::DuplicateAttemptConflict:
      return true;
    default:
      return false;
  }
}

bool IsAuthorityRejection(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::UnknownPublisher:
    case ErrorCode::UnknownAuthorityScope:
    case ErrorCode::UnauthorizedPublisher:
    case ErrorCode::AuthorityScopeViolation:
    case ErrorCode::PublicationModeNotAuthorized:
    case ErrorCode::ProvenanceNotAuthorized:
    case ErrorCode::DurablePublicationNotAuthorized:
    case ErrorCode::ExclusivityRequired:
      return true;
    default:
      return false;
  }
}

bool IsMalformedInputRejection(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::InvalidArgument:
    case ErrorCode::MalformedIdentifier:
    case ErrorCode::MalformedEncoding:
    case ErrorCode::MalformedValue:
    case ErrorCode::UnknownNamespace:
    case ErrorCode::UnknownCapability:
    case ErrorCode::UnknownEnumDomain:
    case ErrorCode::UnsupportedValueKind:
    case ErrorCode::SchemaViolation:
    case ErrorCode::ValueTypeMismatch:
    case ErrorCode::ValueOutOfRange:
    case ErrorCode::ValueContradiction:
    case ErrorCode::ValueNotCanonical:
    case ErrorCode::DuplicateValue:
    case ErrorCode::EmptyCollection:
    case ErrorCode::TooManyItems:
    case ErrorCode::TextTooLong:
    case ErrorCode::PayloadTooLarge:
    case ErrorCode::ArithmeticOverflow:
    case ErrorCode::NestingTooDeep:
      return true;
    default:
      return false;
  }
}

bool IsIntegrityRejection(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::PersistenceFormatInvalid:
    case ErrorCode::PersistenceIntegrityFailure:
    case ErrorCode::PersistenceCorrupt:
    case ErrorCode::PersistenceDuplicateRecord:
    case ErrorCode::PersistenceVersionUnsupported:
    case ErrorCode::FrameMalformed:
    case ErrorCode::FrameIntegrityFailure:
    case ErrorCode::FrameTrailingBytes:
    case ErrorCode::FrameVersionUnsupported:
      return true;
    default:
      return false;
  }
}

}  // namespace fabric::capability
