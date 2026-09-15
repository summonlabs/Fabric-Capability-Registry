// Fabric Capability Registry example: conflicting evidence.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Uses only the public library API.

#include <array>
#include <cstdio>
#include <string>
#include <vector>

#include "fabric/capability/fabric_capability.hpp"

using namespace fabric::capability;

namespace {

constexpr const char* kScope = "example-scope";
constexpr const char* kPublisher = "example-publisher";
constexpr const char* kSource = "example-source";

AuthorityGrant ExampleGrant(const std::vector<const char*>& namespaces,
                            FabricEntityKind kind = FabricEntityKind::Nic) {
  AuthorityGrant grant;
  grant.scope = *AuthorityScopeId::Parse(kScope);
  grant.entity_kinds = {kind};
  for (const char* ns : namespaces) grant.namespaces.push_back(*CapabilityNamespaceId::Parse(ns));
  grant.modes = static_cast<std::uint8_t>(PublicationMode::FullSnapshot) |
                static_cast<std::uint8_t>(PublicationMode::Incremental) |
                static_cast<std::uint8_t>(PublicationMode::PartialObservation);
  grant.may_publish_durable = true;
  grant.strongest_provenance = ProvenanceClass::DirectHardwareEnumeration;
  grant.allowed_publishers = {*PublisherId::Parse(kPublisher)};
  return grant;
}

CapabilityClaim BoolClaim(const char* capability, bool value,
                     ProvenanceClass provenance = ProvenanceClass::DirectHardwareEnumeration,
                     EvidenceSourceClass source_class = EvidenceSourceClass::HardwareEnumeration,
                     DurabilityClass durability = DurabilityClass::ProcessBound) {
  CapabilityClaim claim;
  claim.capability = *CapabilityId::Parse(capability);
  claim.state = CapabilityState::Supported;
  claim.value = CapabilityValue::Boolean(value);
  claim.provenance = provenance;
  claim.source_class = source_class;
  claim.durability = durability;
  return claim;
}

CapabilityClaim QuantityClaim(const char* capability, Unit unit, std::uint64_t value) {
  CapabilityClaim claim;
  claim.capability = *CapabilityId::Parse(capability);
  claim.state = CapabilityState::Supported;
  claim.value = *CapabilityValue::QuantityValue(value, unit);
  claim.provenance = ProvenanceClass::DirectHardwareEnumeration;
  claim.source_class = EvidenceSourceClass::HardwareEnumeration;
  return claim;
}

PublicationRequest Request(const AuthorityGrant& grant, const WorkerBootId& boot,
                           const EntityId& entity, const char* stem, EntityGeneration generation) {
  PublicationRequest request;
  request.mode = PublicationMode::FullSnapshot;
  request.publication = *PublicationId::Parse(stem);
  request.attempt = *MutationAttemptId::Parse(stem);
  request.epoch = CoordinatorEpoch{};
  request.authority = AuthorityContext{grant.scope, *PublisherId::Parse(kPublisher), boot,
                                       *SourceId::Parse(kSource), SourceGeneration::FromValue(1)};
  request.entity = entity;
  request.entity_generation = generation;
  return request;
}

}  // namespace

int main() {
  CapabilityRegistry registry;
  AuthorityGrant first = ExampleGrant({"fabric.queue"});
  first.allowed_publishers = {*PublisherId::Parse("agent-a")};
  AuthorityGrant second = first;
  second.scope = *AuthorityScopeId::Parse("example-scope-b");
  second.allowed_publishers = {*PublisherId::Parse("agent-b")};
  if (!registry.DeclareAuthority(first).HasValue()) return 1;
  if (!registry.DeclareAuthority(second).HasValue()) return 1;
  const WorkerBootId boot_a = *WorkerBootId::Parse("44444444444444444444444444444444");
  const WorkerBootId boot_b = *WorkerBootId::Parse("45454545454545454545454545454545");
  if (!registry.RegisterPublisher(*PublisherId::Parse("agent-a"), first.scope, boot_a).HasValue()) {
    return 1;
  }
  if (!registry.RegisterPublisher(*PublisherId::Parse("agent-b"), second.scope, boot_b).HasValue()) {
    return 1;
  }
  const EntityId nic = *EntityId::Parse("nic:example-5");
  const CapabilityId queues = *CapabilityId::Parse("fabric.queue.max_rx_queues");
  const auto publish = [&registry, &nic](const AuthorityGrant& grant, const char* publisher,
                                         const WorkerBootId& boot, const char* source,
                                         std::uint64_t value, const char* stem,
                                         CapabilitySetGeneration expected, bool snapshot) {
    PublicationRequest request;
    request.mode = snapshot ? PublicationMode::FullSnapshot : PublicationMode::PartialObservation;
    request.publication = *PublicationId::Parse(stem);
    request.attempt = *MutationAttemptId::Parse(stem);
    request.epoch = registry.CurrentEpoch();
    request.authority = AuthorityContext{grant.scope, *PublisherId::Parse(publisher), boot,
                                         *SourceId::Parse(source), SourceGeneration::FromValue(1)};
    request.entity = nic;
    request.entity_generation = EntityGeneration::FromValue(1);
    request.expected_set_generation = expected;
    request.claims = {QuantityClaim("fabric.queue.max_rx_queues", Unit::Count, value)};
    return registry.Publish(request);
  };
  const auto first_publish =
      publish(first, "agent-a", boot_a, "agent-a-source", 64, "example-5-a",
              CapabilitySetGeneration{}, true);
  if (!first_publish.HasValue() || !first_publish.Value().Committed()) return 1;
  const auto second_publish =
      publish(second, "agent-b", boot_b, "agent-b-source", 128, "example-5-b",
              first_publish.Value().new_set_generation, false);
  if (!second_publish.HasValue() || !second_publish.Value().Committed()) return 1;

  const auto conflicted = registry.Query(nic, queues);
  if (!conflicted.HasValue()) return 1;
  std::printf("state=%s conflicting_evidence=%zu\n",
              std::string(CapabilityStateName(conflicted.Value().state)).c_str(),
              conflicted.Value().conflicting_evidence_count);
  for (const EvidenceSummary& summary : conflicted.Value().evidence) {
    std::printf("  evidence=%s source=%s value=%s\n", summary.id.Value().c_str(),
                summary.source.Value().c_str(),
                summary.has_value ? summary.value.ToText().c_str() : "absent");
  }
  std::printf("note=equally strong current sources that disagree stay CONFLICTED\n");
  std::printf("example=05 result=ok\n");
  return 0;
}
