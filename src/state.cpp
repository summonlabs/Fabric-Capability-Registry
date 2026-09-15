// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/capability/state.hpp"

namespace fabric::capability {

std::string_view CapabilityStateName(CapabilityState state) noexcept {
  switch (state) {
    case CapabilityState::Unknown:
      return "UNKNOWN";
    case CapabilityState::Supported:
      return "SUPPORTED";
    case CapabilityState::Unsupported:
      return "UNSUPPORTED";
    case CapabilityState::RevalidationRequired:
      return "REVALIDATION_REQUIRED";
    case CapabilityState::Conflicted:
      return "CONFLICTED";
  }
  return "UNKNOWN";
}

Outcome<CapabilityState> ParseCapabilityState(std::string_view text) {
  if (text == "UNKNOWN" || text == "unknown") return CapabilityState::Unknown;
  if (text == "SUPPORTED" || text == "supported") return CapabilityState::Supported;
  if (text == "UNSUPPORTED" || text == "unsupported") return CapabilityState::Unsupported;
  if (text == "REVALIDATION_REQUIRED" || text == "revalidation-required" ||
      text == "revalidation_required") {
    return CapabilityState::RevalidationRequired;
  }
  if (text == "CONFLICTED" || text == "conflicted") return CapabilityState::Conflicted;
  return Outcome<CapabilityState>::Failure(ErrorCode::MalformedValue, "unknown capability state",
                                           std::string(text));
}

std::string_view CapabilityStateDescription(CapabilityState state) noexcept {
  switch (state) {
    case CapabilityState::Unknown:
      return "insufficient current evidence; never treated as support";
    case CapabilityState::Supported:
      return "authoritative evidence establishes support for this exact entity generation";
    case CapabilityState::Unsupported:
      return "authoritative evidence establishes absence for this exact entity generation";
    case CapabilityState::RevalidationRequired:
      return "prior durable evidence exists but is not current enough to act on";
    case CapabilityState::Conflicted:
      return "current evidence sources disagree and deterministic rules cannot resolve them";
  }
  return "insufficient current evidence";
}

bool IsActionableSupport(CapabilityState state) noexcept {
  return state == CapabilityState::Supported;
}

bool IsAuthoritativeAbsence(CapabilityState state) noexcept {
  return state == CapabilityState::Unsupported;
}

bool FailsClosed(CapabilityState state) noexcept { return !IsActionableSupport(state); }

}  // namespace fabric::capability
