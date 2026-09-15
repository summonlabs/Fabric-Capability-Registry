// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace fabric::capability {

/// Machine readable failure classification. Every rejection produced by this
/// library carries exactly one of these codes; no rejection is ever signalled
/// by an unstructured string alone.
enum class ErrorCode : std::uint16_t {
  Ok = 0,

  // --- input shape and encoding -------------------------------------------
  InvalidArgument = 1,
  MalformedIdentifier,
  MalformedEncoding,
  MalformedValue,
  UnknownNamespace,
  UnknownCapability,
  UnknownEnumDomain,
  UnsupportedValueKind,
  SchemaViolation,
  ValueTypeMismatch,
  ValueOutOfRange,
  ValueContradiction,
  ValueNotCanonical,
  DuplicateValue,
  EmptyCollection,
  TooManyItems,
  TextTooLong,
  PayloadTooLarge,
  ArithmeticOverflow,
  NestingTooDeep,

  // --- entity identity and generations ------------------------------------
  UnknownEntity,
  UnknownEntityGeneration,
  EntityGenerationStale,
  EntityGenerationSuperseded,
  CapabilitySetGenerationStale,
  CapabilitySetGenerationMismatch,
  EvidenceGenerationStale,
  SourceGenerationStale,

  // --- authority, fencing, epochs -----------------------------------------
  UnknownPublisher,
  UnknownAuthorityScope,
  UnauthorizedPublisher,
  AuthorityScopeViolation,
  PublicationModeNotAuthorized,
  ProvenanceNotAuthorized,
  DurablePublicationNotAuthorized,
  ExclusivityRequired,
  WorkerBootUnknown,
  WorkerBootStale,
  WorkerBootFenced,
  CoordinatorEpochStale,
  CoordinatorEpochUnknown,

  // --- publication semantics ----------------------------------------------
  DuplicateClaim,
  ConflictingClaim,
  ClaimCountExceeded,
  EvidenceLimitExceeded,
  StaleReplay,
  DuplicateAttemptConflict,
  EntityInvalidated,
  EntityGenerationAdvanceRequired,

  // --- lookup --------------------------------------------------------------
  NotFound,

  // --- persistence ---------------------------------------------------------
  PersistencePathInvalid,
  PersistenceIoFailure,
  PersistenceFormatInvalid,
  PersistenceVersionUnsupported,
  PersistenceIntegrityFailure,
  PersistenceCorrupt,
  PersistenceDuplicateRecord,
  PersistenceLimitExceeded,

  // --- transport -----------------------------------------------------------
  TransportUnavailable,
  TransportFailure,
  TransportClosed,
  FrameMalformed,
  FrameTooLarge,
  FrameVersionUnsupported,
  FrameIntegrityFailure,
  FrameTrailingBytes,
  FrameTypeUnknown,
  FrameStateViolation,

  // --- discovery -----------------------------------------------------------
  DiscoveryUnavailable,
  DiscoveryFailed,

  // --- generic -------------------------------------------------------------
  Unsupported,
  InternalFailure,
};

/// Stable lower-case name, used by diagnostics, the CLI and tests.
std::string_view ErrorCodeName(ErrorCode code) noexcept;

/// True when the failure is a staleness rejection: the request described an
/// older generation, epoch, boot or attempt than the registry already holds.
/// Staleness rejections are never reported as idempotent success.
bool IsStaleRejection(ErrorCode code) noexcept;

/// True when the failure is an authority rejection.
bool IsAuthorityRejection(ErrorCode code) noexcept;

/// True when the failure indicates malformed, oversized or contradictory
/// input rather than a state conflict.
bool IsMalformedInputRejection(ErrorCode code) noexcept;

/// True when the failure indicates an integrity or format problem in durable
/// or transmitted bytes.
bool IsIntegrityRejection(ErrorCode code) noexcept;

/// Structured failure value.
struct Error {
  ErrorCode code = ErrorCode::Ok;
  std::string message;
  std::string detail;

  Error() = default;
  Error(ErrorCode c, std::string msg, std::string det = {})
      : code(c), message(std::move(msg)), detail(std::move(det)) {}

  bool ok() const noexcept { return code == ErrorCode::Ok; }
  std::string ToString() const;
};

/// Result carrier. c Outcome either holds a value or an c Error; it never
/// throws and never silently discards a failure.
template <class T>
class Outcome {
 public:
  Outcome(T value) : storage_(std::move(value)) {}          // NOLINT implicit
  Outcome(Error error) : storage_(std::move(error)) {}      // NOLINT implicit

  static Outcome Failure(ErrorCode code, std::string message, std::string detail = {}) {
    return Outcome(Error(code, std::move(message), std::move(detail)));
  }

  bool HasValue() const noexcept { return storage_.index() == 0; }
  explicit operator bool() const noexcept { return HasValue(); }

  const T& Value() const { return std::get<0>(storage_); }
  T& Value() { return std::get<0>(storage_); }
  T&& TakeValue() && { return std::move(std::get<0>(storage_)); }
  const T* operator->() const { return &std::get<0>(storage_); }
  T* operator->() { return &std::get<0>(storage_); }
  const T& operator*() const { return std::get<0>(storage_); }
  T& operator*() { return std::get<0>(storage_); }

  const Error& GetError() const { return std::get<1>(storage_); }
  ErrorCode Code() const noexcept {
    return HasValue() ? ErrorCode::Ok : std::get<1>(storage_).code;
  }
  std::string ErrorString() const {
    return HasValue() ? std::string{} : std::get<1>(storage_).ToString();
  }

  T ValueOr(T fallback) const { return HasValue() ? Value() : std::move(fallback); }

 private:
  std::variant<T, Error> storage_;
};

template <>
class Outcome<void> {
 public:
  Outcome() : error_() {}
  Outcome(Error error) : error_(std::move(error)) {}  // NOLINT implicit

  static Outcome Failure(ErrorCode code, std::string message, std::string detail = {}) {
    return Outcome(Error(code, std::move(message), std::move(detail)));
  }
  static Outcome Success() { return Outcome(); }

  bool HasValue() const noexcept { return error_.ok(); }
  explicit operator bool() const noexcept { return HasValue(); }
  const Error& GetError() const { return error_; }
  ErrorCode Code() const noexcept { return error_.code; }
  std::string ErrorString() const { return error_.ToString(); }

 private:
  Error error_;
};

using Status = Outcome<void>;

}  // namespace fabric::capability
