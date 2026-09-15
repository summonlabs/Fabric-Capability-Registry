// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "fabric/capability/error.hpp"
#include "fabric/capability/ids.hpp"
#include "fabric/capability/query.hpp"
#include "fabric/capability/state.hpp"
#include "fabric/capability/value.hpp"

namespace fabric::capability {

/// Bounded, explicit requirement expression. There is deliberately no
/// embedded scripting language: the node set is closed and the evaluation
/// complexity is bounded by depth and node count.
enum class RequirementKind : std::uint8_t {
  /// The capability must resolve to the required state.
  StateIs = 0,
  /// Boolean capability must equal the operand.
  BooleanEquals,
  /// Capability quantity must be at least the operand quantity.
  Minimum,
  /// Capability quantity must be at most the operand quantity.
  Maximum,
  /// Capability value must be a member of the operand set.
  InSet,
  /// Capability range must contain the operand quantity.
  RangeContains,
  /// Capability version interval must accept the operand version.
  VersionContains,
  /// Capability protocol set must contain the operand protocol.
  ProtocolSupported,
  /// Capability enumeration set must contain the operand code.
  EnumerationSupported,
  /// Every child must hold.
  AllOf,
  /// At least one child must hold.
  AnyOf,
  /// The single child must not hold. Only permitted as a child of AllOf or
  /// AnyOf, never as the root.
  Not,
};

std::string_view RequirementKindName(RequirementKind kind) noexcept;
Outcome<RequirementKind> ParseRequirementKind(std::string_view text);

struct Requirement {
  RequirementKind kind = RequirementKind::StateIs;
  RequirementName name;
  CapabilityId capability;
  CapabilityState required_state = CapabilityState::Supported;
  CapabilityValue operand;
  Version operand_version;
  ProtocolId operand_protocol = ProtocolId::Unknown;
  std::uint32_t operand_enum_code = 0;
  std::vector<Requirement> children;

  /// When true the requirement fails closed on UNKNOWN, REVALIDATION_REQUIRED
  /// and CONFLICTED: the outcome is Undetermined and never Satisfied.
  /// When false an unproven capability is reported as NotSatisfied.
  bool require_proof = true;
};

/// Validates bounds: nesting depth, node count, operand presence, capability
/// identifiers, and that Not is not the root.
Outcome<void> ValidateRequirement(const Requirement& requirement);

/// One evaluated node, in deterministic pre-order.
struct RequirementNodeResult {
  std::string path;
  RequirementKind kind = RequirementKind::StateIs;
  std::string name;
  CapabilityId capability;
  RequirementOutcome outcome = RequirementOutcome::Undetermined;
  CapabilityState observed_state = CapabilityState::Unknown;
  bool has_observed_value = false;
  CapabilityValue observed_value;
  EntityId entity;
  EntityGeneration entity_generation;
  std::string detail;
};

struct RequirementEvaluation {
  RequirementOutcome outcome = RequirementOutcome::Undetermined;
  EntityId entity;
  EntityGeneration entity_generation;
  std::size_t nodes_evaluated = 0;
  std::vector<RequirementNodeResult> nodes;
  Explanation explanation;
};

}  // namespace fabric::capability
