// Fabric Capability Registry example: stale generation rejection.
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
  const AuthorityGrant grant = ExampleGrant({"fabric.queue"});
  if (!registry.DeclareAuthority(grant).HasValue()) return 1;
  const WorkerBootId boot = *WorkerBootId::Parse("55555555555555555555555555555555");
  if (!registry.RegisterPublisher(*PublisherId::Parse(kPublisher), grant.scope, boot).HasValue()) {
    return 1;
  }
  const EntityId nic = *EntityId::Parse("nic:example-6");
  const auto publish = [&](const char* stem, std::uint64_t value, CapabilitySetGeneration expected,
                           EntityGeneration generation) {
    PublicationRequest request = Request(grant, boot, nic, stem, generation);
    request.epoch = registry.CurrentEpoch();
    request.expected_set_generation = expected;
    request.claims = {QuantityClaim("fabric.queue.max_rx_queues", Unit::Count, value)};
    return registry.Publish(request);
  };
  const auto first = publish("example-6-a", 32, CapabilitySetGeneration{}, EntityGeneration::FromValue(1));
  if (!first.HasValue() || !first.Value().Committed()) return 1;
  const auto second =
      publish("example-6-b", 64, first.Value().new_set_generation, EntityGeneration::FromValue(1));
  if (!second.HasValue() || !second.Value().Committed()) return 1;
  const auto stale = publish("example-6-c", 128, CapabilitySetGeneration::FromValue(1),
                             EntityGeneration::FromValue(1));
  if (!stale.HasValue()) return 1;
  std::printf("stale_set_generation status=%s code=%s\n",
              std::string(PublicationStatusName(stale.Value().status)).c_str(),
              std::string(ErrorCodeName(stale.Value().code)).c_str());
  const auto stale_entity = publish("example-6-d", 256, CapabilitySetGeneration::FromValue(2),
                                    EntityGeneration::FromValue(0));
  if (!stale_entity.HasValue()) return 1;
  std::printf("stale_entity_generation status=%s code=%s\n",
              std::string(PublicationStatusName(stale_entity.Value().status)).c_str(),
              std::string(ErrorCodeName(stale_entity.Value().code)).c_str());
  std::printf("note=a rejected publication changes nothing and advances nothing\n");
  std::printf("example=06 result=ok\n");
  return 0;
}
