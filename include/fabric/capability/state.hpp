// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string_view>

#include "fabric/capability/error.hpp"

namespace fabric::capability {

// Capability is not configuration, availability, health, current use or
// policy permission. This runtime answers exactly one question: what may be
// possible for the exact bound entity generation, under which evidence.
//
// The following concepts are deliberately NOT capability states and are never
// produced by this library; they belong to other runtimes:
//
//   ENABLED / CONFIGURED  -> Port Fabric, configuration runtimes
//   ACTIVE                -> Port Fabric, queue/buffer/offload runtimes
//   HEALTHY / AVAILABLE   -> Link State Fabric, availability runtimes
//   AUTHORIZED            -> policy runtimes
//
// A capability claim that is SUPPORTED says nothing about any of them.

/// First class support semantics of a capability claim.
enum class CapabilityState : std::uint8_t {
  /// Insufficient current evidence. Never treated as support.
  Unknown = 0,
  /// Authoritative evidence establishes that the bound entity generation
  /// supports the capability.
  Supported,
  /// Authoritative evidence establishes that the bound entity generation
  /// does not support the capability.
  Unsupported,
  /// Prior durable evidence exists but is not current enough to act on.
  RevalidationRequired,
  /// Current evidence sources disagree in a way the deterministic resolution
  /// rules cannot resolve.
  Conflicted,
};

std::string_view CapabilityStateName(CapabilityState state) noexcept;
Outcome<CapabilityState> ParseCapabilityState(std::string_view text);
std::string_view CapabilityStateDescription(CapabilityState state) noexcept;

/// True only for SUPPORTED. Consumers that need proof must test this rather
/// than testing "not UNSUPPORTED".
bool IsActionableSupport(CapabilityState state) noexcept;

/// True only for UNSUPPORTED: authoritative evidence of absence.
bool IsAuthoritativeAbsence(CapabilityState state) noexcept;

/// True when the state cannot be answered as support: UNKNOWN,
/// REVALIDATION_REQUIRED and CONFLICTED all fail closed.
bool FailsClosed(CapabilityState state) noexcept;

}  // namespace fabric::capability
