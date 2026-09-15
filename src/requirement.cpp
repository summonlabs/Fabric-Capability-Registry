// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/capability/requirement.hpp"

#include <algorithm>
#include <string>

#include "fabric/capability/registry.hpp"
#include "src/internal/requirement_internal.hpp"

namespace fabric::capability {
namespace {

struct RequirementKindEntry {
  RequirementKind kind;
  std::string_view name;
};

constexpr std::array<RequirementKindEntry, 12> kRequirementKindNames = {{
    {RequirementKind::StateIs, "state-is"},
    {RequirementKind::BooleanEquals, "boolean-equals"},
    {RequirementKind::Minimum, "minimum"},
    {RequirementKind::Maximum, "maximum"},
    {RequirementKind::InSet, "in-set"},
    {RequirementKind::RangeContains, "range-contains"},
    {RequirementKind::VersionContains, "version-contains"},
    {RequirementKind::ProtocolSupported, "protocol-supported"},
    {RequirementKind::EnumerationSupported, "enumeration-supported"},
    {RequirementKind::AllOf, "all-of"},
    {RequirementKind::AnyOf, "any-of"},
    {RequirementKind::Not, "not"},
}};

struct ValidationState {
  std::size_t nodes = 0;
};

Outcome<void> ValidateNode(const Requirement& requirement, std::size_t depth,
                           ValidationState& state, bool is_root) {
  if (depth > limits::kMaxRequirementDepth) {
    return Status::Failure(ErrorCode::NestingTooDeep,
                           "requirement nesting exceeds the allowed depth",
                           std::to_string(depth));
  }
  if (++state.nodes > limits::kMaxRequirementNodes) {
    return Status::Failure(ErrorCode::TooManyItems,
                           "requirement exceeds the allowed node count",
                           std::to_string(state.nodes));
  }
  switch (requirement.kind) {
    case RequirementKind::AllOf:
    case RequirementKind::AnyOf: {
      if (requirement.children.empty()) {
        return Status::Failure(ErrorCode::EmptyCollection,
                               "a compound requirement must have at least one child");
      }
      for (const Requirement& child : requirement.children) {
        auto valid = ValidateNode(child, depth + 1, state, false);
        if (!valid) return valid.GetError();
      }
      return Status::Success();
    }
    case RequirementKind::Not: {
      if (is_root) {
        return Status::Failure(ErrorCode::InvalidArgument,
                               "NOT is not permitted as the root requirement");
      }
      if (requirement.children.size() != 1) {
        return Status::Failure(ErrorCode::InvalidArgument,
                               "NOT requires exactly one child requirement");
      }
      return ValidateNode(requirement.children.front(), depth + 1, state, false);
    }
    default:
      break;
  }
  if (!requirement.capability.IsSet()) {
    return Status::Failure(ErrorCode::InvalidArgument,
                           "a leaf requirement must name a capability");
  }
  if (!requirement.children.empty()) {
    return Status::Failure(ErrorCode::InvalidArgument,
                           "a leaf requirement must not have children");
  }
  switch (requirement.kind) {
    case RequirementKind::StateIs:
      return Status::Success();
    case RequirementKind::BooleanEquals:
      if (requirement.operand.Kind() != ValueKind::Boolean) {
        return Status::Failure(ErrorCode::ValueTypeMismatch,
                               "boolean-equals requires a boolean operand");
      }
      return Status::Success();
    case RequirementKind::Minimum:
    case RequirementKind::Maximum:
    case RequirementKind::RangeContains:
    case RequirementKind::InSet:
      if (requirement.operand.Kind() != ValueKind::Quantity) {
        return Status::Failure(ErrorCode::ValueTypeMismatch,
                               "this requirement requires a quantity operand",
                               requirement.capability.ToString());
      }
      return Status::Success();
    case RequirementKind::VersionContains:
      if (requirement.operand_version.Count() == 0) {
        return Status::Failure(ErrorCode::ValueTypeMismatch,
                               "version-contains requires a version operand");
      }
      return Status::Success();
    case RequirementKind::ProtocolSupported:
      if (requirement.operand_protocol == ProtocolId::Unknown) {
        return Status::Failure(ErrorCode::ValueTypeMismatch,
                               "protocol-supported requires a protocol operand");
      }
      return Status::Success();
    case RequirementKind::EnumerationSupported:
      return Status::Success();
    default:
      return Status::Success();
  }
}

/// Extracts the quantity an observed capability value exposes for comparison.
struct ObservedQuantity {
  bool present = false;
  std::uint64_t minimum = 0;
  std::uint64_t maximum = 0;
  Unit unit = Unit::None;
  bool is_set = false;
  std::vector<std::uint64_t> members;
};

ObservedQuantity ExtractQuantity(const CapabilityValue& value) {
  ObservedQuantity quantity;
  switch (value.Kind()) {
    case ValueKind::Quantity:
      quantity.present = true;
      quantity.minimum = value.AsQuantity().value;
      quantity.maximum = value.AsQuantity().value;
      quantity.unit = value.AsQuantity().unit;
      break;
    case ValueKind::NumericRange:
      quantity.present = true;
      quantity.minimum = value.AsNumericRange().minimum;
      quantity.maximum = value.AsNumericRange().maximum;
      quantity.unit = value.AsNumericRange().unit;
      break;
    case ValueKind::NumericSet:
      quantity.present = true;
      quantity.is_set = true;
      quantity.members = value.AsNumericSet().values;
      quantity.unit = value.AsNumericSet().unit;
      if (!quantity.members.empty()) {
        quantity.minimum = quantity.members.front();
        quantity.maximum = quantity.members.back();
      }
      break;
    default:
      break;
  }
  return quantity;
}

class Evaluator {
 public:
  Evaluator(const CapabilityRegistry& registry, const EntityId& entity)
      : registry_(registry), entity_(entity) {}

  RequirementOutcome Evaluate(const Requirement& requirement, const std::string& path) {
    switch (requirement.kind) {
      case RequirementKind::AllOf: {
        bool all_satisfied = true;
        bool any_unsatisfied = false;
        for (std::size_t index = 0; index < requirement.children.size(); ++index) {
          const RequirementOutcome outcome =
              Evaluate(requirement.children[index], path + "." + std::to_string(index));
          if (outcome == RequirementOutcome::NotSatisfied) any_unsatisfied = true;
          if (outcome != RequirementOutcome::Satisfied) all_satisfied = false;
        }
        const RequirementOutcome outcome =
            all_satisfied ? RequirementOutcome::Satisfied
                          : (any_unsatisfied ? RequirementOutcome::NotSatisfied
                                             : RequirementOutcome::Undetermined);
        Record(path, requirement, outcome, CapabilityState::Unknown, false, CapabilityValue{},
               all_satisfied ? "every child requirement holds"
                             : "not every child requirement holds");
        return outcome;
      }
      case RequirementKind::AnyOf: {
        bool any_satisfied = false;
        bool all_unsatisfied = true;
        for (std::size_t index = 0; index < requirement.children.size(); ++index) {
          const RequirementOutcome outcome =
              Evaluate(requirement.children[index], path + "." + std::to_string(index));
          if (outcome == RequirementOutcome::Satisfied) any_satisfied = true;
          if (outcome != RequirementOutcome::NotSatisfied) all_unsatisfied = false;
        }
        const RequirementOutcome outcome =
            any_satisfied ? RequirementOutcome::Satisfied
                          : (all_unsatisfied ? RequirementOutcome::NotSatisfied
                                             : RequirementOutcome::Undetermined);
        Record(path, requirement, outcome, CapabilityState::Unknown, false, CapabilityValue{},
               any_satisfied ? "at least one child requirement holds"
                             : "no child requirement is proven to hold");
        return outcome;
      }
      case RequirementKind::Not: {
        const RequirementOutcome inner = Evaluate(requirement.children.front(), path + ".0");
        RequirementOutcome outcome = RequirementOutcome::Undetermined;
        if (inner == RequirementOutcome::Satisfied) outcome = RequirementOutcome::NotSatisfied;
        if (inner == RequirementOutcome::NotSatisfied) outcome = RequirementOutcome::Satisfied;
        Record(path, requirement, outcome, CapabilityState::Unknown, false, CapabilityValue{},
               inner == RequirementOutcome::Undetermined ? "the negated requirement is unproven"
                                                         : "the negated requirement is decided");
        return outcome;
      }
      default:
        break;
    }

    const auto query = registry_.Query(entity_, requirement.capability);
    const CapabilityState state =
        query.HasValue() ? query.Value().state : CapabilityState::Unknown;
    const bool has_value = query.HasValue() && query.Value().has_value;
    const CapabilityValue observed = query.HasValue() ? query.Value().value : CapabilityValue{};
    const bool proven = state == CapabilityState::Supported;

    if (!proven) {
      const RequirementOutcome outcome = requirement.require_proof
                                             ? RequirementOutcome::Undetermined
                                             : RequirementOutcome::NotSatisfied;
      Record(path, requirement, outcome, state, has_value, observed,
             std::string("capability resolves to ") + std::string(CapabilityStateName(state)) +
                 (requirement.require_proof ? "; the requirement demands proof and fails closed"
                                            : "; an unproven capability is reported as not "
                                              "satisfied"));
      return outcome;
    }

    bool satisfied = false;
    std::string detail;
    switch (requirement.kind) {
      case RequirementKind::StateIs: {
        satisfied = state == requirement.required_state;
        detail = "observed state compared with the required state";
        break;
      }
      case RequirementKind::BooleanEquals: {
        if (!has_value || observed.Kind() != ValueKind::Boolean) {
          return Undetermined(path, requirement, state, has_value, observed,
                              "the capability does not expose a boolean value");
        }
        satisfied = observed.AsBoolean() == requirement.operand.AsBoolean();
        detail = "boolean value compared with the operand";
        break;
      }
      case RequirementKind::Minimum:
      case RequirementKind::Maximum:
      case RequirementKind::RangeContains:
      case RequirementKind::InSet: {
        const ObservedQuantity quantity = ExtractQuantity(observed);
        if (!quantity.present) {
          return Undetermined(path, requirement, state, has_value, observed,
                              "the capability does not expose a comparable quantity");
        }
        const Quantity& operand = requirement.operand.AsQuantity();
        if (quantity.unit != operand.unit) {
          return Undetermined(path, requirement, state, has_value, observed,
                              "the requirement unit does not match the capability unit");
        }
        switch (requirement.kind) {
          case RequirementKind::Minimum:
            satisfied = quantity.maximum >= operand.value;
            detail = "capability upper bound compared with the required minimum";
            break;
          case RequirementKind::Maximum:
            satisfied = quantity.minimum <= operand.value;
            detail = "capability lower bound compared with the required maximum";
            break;
          case RequirementKind::RangeContains:
            satisfied = !quantity.is_set && quantity.minimum <= operand.value &&
                        operand.value <= quantity.maximum;
            detail = "the required quantity is inside the capability interval";
            break;
          default:
            satisfied = std::find(quantity.members.begin(), quantity.members.end(),
                                  operand.value) != quantity.members.end();
            detail = "the required quantity is a member of the capability set";
            break;
        }
        break;
      }
      case RequirementKind::VersionContains: {
        if (!has_value || observed.Kind() != ValueKind::VersionInterval) {
          return Undetermined(path, requirement, state, has_value, observed,
                              "the capability does not expose a version interval");
        }
        const VersionInterval& interval = observed.AsVersionInterval();
        const Version& version = requirement.operand_version;
        const bool above = interval.minimum_inclusive ? !(version < interval.minimum)
                                                      : interval.minimum < version;
        const bool below = interval.maximum_inclusive ? !(interval.maximum < version)
                                                      : version < interval.maximum;
        satisfied = above && below;
        detail = "the required version is inside the capability version interval";
        break;
      }
      case RequirementKind::ProtocolSupported: {
        if (!has_value || observed.Kind() != ValueKind::ProtocolSet) {
          return Undetermined(path, requirement, state, has_value, observed,
                              "the capability does not expose a protocol set");
        }
        satisfied = std::find(observed.AsProtocolSet().protocols.begin(),
                              observed.AsProtocolSet().protocols.end(),
                              requirement.operand_protocol) !=
                    observed.AsProtocolSet().protocols.end();
        detail = "the required protocol is a member of the capability protocol set";
        break;
      }
      case RequirementKind::EnumerationSupported: {
        if (!has_value || observed.Kind() != ValueKind::EnumSet) {
          return Undetermined(path, requirement, state, has_value, observed,
                              "the capability does not expose an enumeration set");
        }
        satisfied = std::find(observed.AsEnumSet().codes.begin(), observed.AsEnumSet().codes.end(),
                              requirement.operand_enum_code) != observed.AsEnumSet().codes.end();
        detail = "the required enumeration code is a member of the capability set";
        break;
      }
      default:
        return Undetermined(path, requirement, state, has_value, observed,
                            "unsupported requirement kind");
    }

    const RequirementOutcome outcome =
        satisfied ? RequirementOutcome::Satisfied : RequirementOutcome::NotSatisfied;
    Record(path, requirement, outcome, state, has_value, observed, detail);
    return outcome;
  }

  std::vector<RequirementNodeResult> TakeNodes() { return std::move(nodes_); }

 private:
  RequirementOutcome Undetermined(const std::string& path, const Requirement& requirement,
                                  CapabilityState state, bool has_value,
                                  const CapabilityValue& observed, std::string detail) {
    Record(path, requirement, RequirementOutcome::Undetermined, state, has_value, observed,
           std::move(detail));
    return RequirementOutcome::Undetermined;
  }

  void Record(const std::string& path, const Requirement& requirement, RequirementOutcome outcome,
              CapabilityState state, bool has_value, const CapabilityValue& observed,
              std::string detail) {
    if (nodes_.size() >= limits::kMaxRequirementNodes) return;
    RequirementNodeResult node;
    node.path = path;
    node.kind = requirement.kind;
    node.name = requirement.name.IsSet() ? requirement.name.Value() : std::string();
    node.capability = requirement.capability;
    node.outcome = outcome;
    node.observed_state = state;
    node.has_observed_value = has_value;
    if (has_value) node.observed_value = observed;
    node.entity = entity_;
    node.detail = std::move(detail);
    nodes_.push_back(std::move(node));
  }

  const CapabilityRegistry& registry_;
  EntityId entity_;
  std::vector<RequirementNodeResult> nodes_;
};

}  // namespace

std::string_view RequirementKindName(RequirementKind kind) noexcept {
  for (const RequirementKindEntry& entry : kRequirementKindNames) {
    if (entry.kind == kind) return entry.name;
  }
  return "state-is";
}

Outcome<RequirementKind> ParseRequirementKind(std::string_view text) {
  for (const RequirementKindEntry& entry : kRequirementKindNames) {
    if (entry.name == text) return entry.kind;
  }
  return Outcome<RequirementKind>::Failure(ErrorCode::MalformedValue,
                                           "unknown requirement kind", std::string(text));
}

Outcome<void> ValidateRequirement(const Requirement& requirement) {
  ValidationState state;
  return ValidateNode(requirement, 0, state, true);
}

RequirementEvaluation EvaluateRequirementAgainst(const CapabilityRegistry& registry,
                                                 const EntityId& entity,
                                                 const Requirement& requirement) {
  RequirementEvaluation evaluation;
  evaluation.entity = entity;
  auto valid = ValidateRequirement(requirement);
  if (!valid) {
    evaluation.outcome = RequirementOutcome::Undetermined;
    evaluation.explanation.code = "requirement.invalid";
    evaluation.explanation.summary = valid.GetError().ToString();
    return evaluation;
  }
  {
    const auto current = registry.QueryEntity(entity);
    if (current.HasValue()) {
      evaluation.entity_generation = current.Value().entity_generation;
    }
  }
  Evaluator evaluator(registry, entity);
  evaluation.outcome = evaluator.Evaluate(requirement, "0");
  evaluation.nodes = evaluator.TakeNodes();
  evaluation.nodes_evaluated = evaluation.nodes.size();

  evaluation.explanation.code =
      std::string("requirement.") + std::string(RequirementOutcomeName(evaluation.outcome));
  evaluation.explanation.summary =
      std::string("requirement evaluation for ") + entity.ToString() + " is " +
      std::string(RequirementOutcomeName(evaluation.outcome));
  {
    ExplanationStep step;
    step.code = "requirement.nodes";
    step.summary = "evaluated nodes";
    step.fields.emplace_back("nodes", std::to_string(evaluation.nodes.size()));
    step.fields.emplace_back("entity_generation", evaluation.entity_generation.ToString());
    evaluation.explanation.steps.push_back(std::move(step));
  }
  for (const RequirementNodeResult& node : evaluation.nodes) {
    if (evaluation.explanation.steps.size() >= limits::kMaxTextFieldsPerExplanation) break;
    ExplanationStep step;
    step.code = std::string("requirement.node.") + std::string(RequirementOutcomeName(node.outcome));
    step.summary = std::string(RequirementKindName(node.kind)) + " -> " +
                   std::string(RequirementOutcomeName(node.outcome));
    step.fields.emplace_back("path", node.path);
    if (!node.name.empty()) step.fields.emplace_back("name", node.name);
    if (node.capability.IsSet()) {
      step.fields.emplace_back("capability", node.capability.ToString());
    }
    step.fields.emplace_back("observed_state", std::string(CapabilityStateName(node.observed_state)));
    if (node.has_observed_value) {
      step.fields.emplace_back("observed_value", node.observed_value.ToText());
    }
    if (!node.detail.empty()) step.fields.emplace_back("detail", node.detail);
    evaluation.explanation.steps.push_back(std::move(step));
  }
  return evaluation;
}

}  // namespace fabric::capability
