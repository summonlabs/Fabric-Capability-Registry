// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Deterministic renderers for the query, record and publication value types.
// Rendering never includes timestamps, addresses, thread identifiers or any
// other local process state.

#include <algorithm>

#include "fabric/capability/authority.hpp"
#include "fabric/capability/publication.hpp"
#include "fabric/capability/query.hpp"
#include "fabric/capability/record.hpp"

namespace fabric::capability {

std::string ExplanationStep::ToText() const {
  std::string text = code;
  if (!summary.empty()) {
    text.append(": ").append(summary);
  }
  for (const auto& field : fields) {
    text.append("\n  ").append(field.first).append("=").append(field.second);
  }
  return text;
}

std::string Explanation::ToText() const {
  std::string text;
  text.append(code.empty() ? "explanation" : code);
  if (!summary.empty()) {
    text.append(": ").append(summary);
  }
  for (const ExplanationStep& step : steps) {
    text.append("\n- ").append(step.ToText());
  }
  return text;
}

std::string_view RequirementOutcomeName(RequirementOutcome outcome) noexcept {
  switch (outcome) {
    case RequirementOutcome::Satisfied:
      return "satisfied";
    case RequirementOutcome::NotSatisfied:
      return "not-satisfied";
    case RequirementOutcome::Undetermined:
      return "undetermined";
  }
  return "undetermined";
}

std::string_view CapabilityDiffKindName(CapabilityDiffKind kind) noexcept {
  switch (kind) {
    case CapabilityDiffKind::CapabilityAdded:
      return "capability-added";
    case CapabilityDiffKind::CapabilityRemoved:
      return "capability-removed";
    case CapabilityDiffKind::StateChanged:
      return "state-changed";
    case CapabilityDiffKind::ValueChanged:
      return "value-changed";
    case CapabilityDiffKind::ProvenanceChanged:
      return "provenance-changed";
    case CapabilityDiffKind::EvidenceSuperseded:
      return "evidence-superseded";
    case CapabilityDiffKind::CurrentnessChanged:
      return "currentness-changed";
    case CapabilityDiffKind::GenerationAdvanced:
      return "generation-advanced";
    case CapabilityDiffKind::EntityGenerationChanged:
      return "entity-generation-changed";
    case CapabilityDiffKind::EntityInvalidated:
      return "entity-invalidated";
  }
  return "capability-added";
}

std::string CapabilityDiff::ToText() const {
  std::string text = "entity=" + entity.ToString();
  text.append(" before_entity_generation=").append(before_entity_generation.ToString());
  text.append(" after_entity_generation=").append(after_entity_generation.ToString());
  text.append(" before_set_generation=").append(before_set_generation.ToString());
  text.append(" after_set_generation=").append(after_set_generation.ToString());
  text.append(" entries=").append(std::to_string(entries.size()));
  if (entries.empty()) {
    text.append("\n(no capability difference)");
    return text;
  }
  for (const CapabilityDiffEntry& entry : entries) {
    text.append("\n");
    if (entry.capability.IsSet()) {
      text.append(entry.capability.ToString());
    } else {
      text.append("-");
    }
    text.append(" ").append(CapabilityDiffKindName(entry.kind));
    text.append(" ").append(CapabilityStateName(entry.before_state));
    text.append("->").append(CapabilityStateName(entry.after_state));
    if (entry.has_before_value || entry.has_after_value) {
      text.append(" value=");
      text.append(entry.has_before_value ? entry.before_value.ToText() : std::string("absent"));
      text.append("->");
      text.append(entry.has_after_value ? entry.after_value.ToText() : std::string("absent"));
    }
    if (entry.kind == CapabilityDiffKind::ProvenanceChanged) {
      text.append(" provenance=").append(ProvenanceClassName(entry.before_provenance));
      text.append("->").append(ProvenanceClassName(entry.after_provenance));
    }
    if (!entry.detail.empty()) {
      text.append(" detail=").append(entry.detail);
    }
  }
  return text;
}

std::size_t EntityCapabilitySet::CountOf(CapabilityState state) const noexcept {
  std::size_t count = 0;
  for (const CapabilityResolution& resolution : capabilities) {
    if (resolution.state == state) ++count;
  }
  return count;
}

const CapabilityResolution* EntityCapabilitySet::Find(const CapabilityId& capability) const noexcept {
  const auto position = std::lower_bound(
      capabilities.begin(), capabilities.end(), capability,
      [](const CapabilityResolution& resolution, const CapabilityId& id) {
        return resolution.capability < id;
      });
  if (position == capabilities.end() || !(position->capability == capability)) return nullptr;
  return &*position;
}

std::string EntityCapabilitySet::ToText() const {
  std::string text = "entity=" + entity.ToString();
  text.append(" entity_generation=").append(entity_generation.ToString());
  text.append(" set_generation=").append(set_generation.ToString());
  text.append(" registry_generation=").append(registry_generation.ToString());
  text.append(" capabilities=").append(std::to_string(capabilities.size()));
  text.append(" supported=").append(std::to_string(CountOf(CapabilityState::Supported)));
  text.append(" unsupported=").append(std::to_string(CountOf(CapabilityState::Unsupported)));
  text.append(" unknown=").append(std::to_string(CountOf(CapabilityState::Unknown)));
  text.append(" revalidation_required=")
      .append(std::to_string(CountOf(CapabilityState::RevalidationRequired)));
  text.append(" conflicted=").append(std::to_string(CountOf(CapabilityState::Conflicted)));
  text.append(" digest=").append(digest.ToString());
  if (invalidated) {
    text.append(" invalidated=true");
    if (invalidation_reason.IsSet()) {
      text.append(" reason=").append(invalidation_reason.Value());
    }
  }
  for (const CapabilityResolution& resolution : capabilities) {
    text.append("\n  capability=").append(resolution.capability.ToString());
    text.append(" state=").append(CapabilityStateName(resolution.state));
    text.append(" value=");
    text.append(resolution.has_value ? resolution.value.ToText() : std::string("absent"));
    text.append(" provenance=").append(ProvenanceClassName(resolution.provenance));
    text.append(" source_class=").append(EvidenceSourceClassName(resolution.source_class));
    text.append(" coverage=").append(CoverageName(resolution.coverage));
    text.append(" durability=").append(DurabilityClassName(resolution.durability));
    text.append(" evidence=").append(std::to_string(resolution.evidence_count));
    text.append(" current_evidence=").append(std::to_string(resolution.current_evidence_count));
    text.append(" generation=").append(resolution.capability_generation.ToString());
  }
  return text;
}

std::string_view IncrementalOperationName(IncrementalOperation operation) noexcept {
  switch (operation) {
    case IncrementalOperation::UpsertClaim:
      return "upsert-claim";
    case IncrementalOperation::WithdrawClaim:
      return "withdraw-claim";
    case IncrementalOperation::MarkUnsupported:
      return "mark-unsupported";
    case IncrementalOperation::MarkUnknown:
      return "mark-unknown";
    case IncrementalOperation::MarkRevalidationRequired:
      return "mark-revalidation-required";
    case IncrementalOperation::SupersedeSourceEvidence:
      return "supersede-source-evidence";
  }
  return "upsert-claim";
}

Outcome<IncrementalOperation> ParseIncrementalOperation(std::string_view text) {
  if (text == "upsert-claim" || text == "upsert") return IncrementalOperation::UpsertClaim;
  if (text == "withdraw-claim" || text == "withdraw") return IncrementalOperation::WithdrawClaim;
  if (text == "mark-unsupported") return IncrementalOperation::MarkUnsupported;
  if (text == "mark-unknown") return IncrementalOperation::MarkUnknown;
  if (text == "mark-revalidation-required") {
    return IncrementalOperation::MarkRevalidationRequired;
  }
  if (text == "supersede-source-evidence") {
    return IncrementalOperation::SupersedeSourceEvidence;
  }
  return Outcome<IncrementalOperation>::Failure(ErrorCode::MalformedValue,
                                                "unknown incremental operation",
                                                std::string(text));
}

std::string_view PublicationModeName(PublicationMode mode) noexcept {
  switch (mode) {
    case PublicationMode::FullSnapshot:
      return "full-snapshot";
    case PublicationMode::Incremental:
      return "incremental";
    case PublicationMode::PartialObservation:
      return "partial-observation";
  }
  return "partial-observation";
}

std::string PublicationModeMaskText(PublicationModeMask mask) {
  std::string text;
  const auto append = [&text](std::string_view name) {
    if (!text.empty()) text.push_back(',');
    text.append(name);
  };
  if (AllowsMode(mask, PublicationMode::FullSnapshot)) append("full");
  if (AllowsMode(mask, PublicationMode::Incremental)) append("incremental");
  if (AllowsMode(mask, PublicationMode::PartialObservation)) append("partial");
  return text.empty() ? std::string("none") : text;
}

std::string_view PublicationStatusName(PublicationStatus status) noexcept {
  switch (status) {
    case PublicationStatus::Committed:
      return "committed";
    case PublicationStatus::IdempotentReplay:
      return "idempotent-replay";
    case PublicationStatus::Rejected:
      return "rejected";
  }
  return "rejected";
}

std::string PublicationResult::ToText() const {
  std::string text = "publication=" + publication.Value();
  text.append(" status=").append(PublicationStatusName(status));
  text.append(" code=").append(ErrorCodeName(code));
  text.append(" entity=").append(entity.ToString());
  text.append(" entity_generation=").append(entity_generation.ToString());
  text.append(" previous_set_generation=").append(previous_set_generation.ToString());
  text.append(" set_generation=").append(new_set_generation.ToString());
  text.append(" registry_generation=").append(registry_generation.ToString());
  text.append(" claims_applied=").append(std::to_string(claims_applied));
  text.append(" claims_withdrawn=").append(std::to_string(claims_withdrawn));
  text.append(" added=").append(std::to_string(capabilities_added));
  text.append(" removed=").append(std::to_string(capabilities_removed));
  text.append(" changed=").append(std::to_string(capabilities_changed));
  if (!message.empty()) {
    text.append(" message=").append(message);
  }
  return text;
}

std::string SnapshotScope::ToText() const {
  std::string text = all_entities ? "scope=all" : "scope=explicit";
  if (!entities.empty()) {
    text.append(" entities=").append(std::to_string(entities.size()));
    std::vector<EntityId> sorted = entities;
    std::sort(sorted.begin(), sorted.end());
    for (const EntityId& entity : sorted) {
      text.append(",").append(entity.ToString());
    }
  }
  if (!namespaces.empty()) {
    text.append(" namespaces=");
    std::vector<CapabilityNamespaceId> sorted = namespaces;
    std::sort(sorted.begin(), sorted.end());
    for (std::size_t index = 0; index < sorted.size(); ++index) {
      if (index != 0) text.push_back(',');
      text.append(sorted[index].Value());
    }
  }
  if (!entity_kinds.empty()) {
    text.append(" entity_kinds=");
    std::vector<std::string_view> names;
    for (const FabricEntityKind kind : entity_kinds) names.push_back(FabricEntityKindName(kind));
    std::sort(names.begin(), names.end());
    for (std::size_t index = 0; index < names.size(); ++index) {
      if (index != 0) text.push_back(',');
      text.append(names[index]);
    }
  }
  return text;
}

const SnapshotEntityEntry* CapabilitySnapshot::Find(const EntityId& entity) const noexcept {
  const auto position = std::lower_bound(
      entities.begin(), entities.end(), entity,
      [](const SnapshotEntityEntry& entry, const EntityId& id) { return entry.entity < id; });
  if (position == entities.end() || !(position->entity == entity)) return nullptr;
  return &*position;
}

std::string CapabilitySnapshot::ToText() const {
  std::string text = "snapshot=" + id.Value();
  text.append(" registry_generation=").append(registry_generation.ToString());
  text.append(" epoch=").append(epoch.ToString());
  text.append(" entities=").append(std::to_string(entities.size()));
  text.append(" digest=").append(digest.ToString());
  text.append(" ").append(scope.ToText());
  for (const SnapshotEntityEntry& entry : entities) {
    text.append("\n  entity=").append(entry.entity.ToString());
    text.append(" entity_generation=").append(entry.entity_generation.ToString());
    text.append(" set_generation=").append(entry.set_generation.ToString());
    text.append(" digest=").append(entry.set_digest.ToString());
  }
  return text;
}

std::string SnapshotCurrentness::ToText() const {
  std::string text = current ? "current=true" : "current=false";
  for (const std::string& reason : reasons) {
    text.append("\n  reason=").append(reason);
  }
  return text;
}

}  // namespace fabric::capability
