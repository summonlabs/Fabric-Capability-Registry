// Fabric Capability Registry example: queue and offload capability.
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
  const AuthorityGrant grant = ExampleGrant({"fabric.queue", "fabric.offload"});
  if (!registry.DeclareAuthority(grant).HasValue()) return 1;
  const WorkerBootId boot = *WorkerBootId::Parse("22222222222222222222222222222222");
  if (!registry.RegisterPublisher(*PublisherId::Parse(kPublisher), grant.scope, boot).HasValue()) {
    return 1;
  }
  const EntityId nic = *EntityId::Parse("nic:example-3");
  PublicationRequest request =
      Request(grant, boot, nic, "example-publication-3", EntityGeneration::FromValue(1));
  request.epoch = registry.CurrentEpoch();
  request.coverage = Coverage::FullEnumeration;
  request.claims = {QuantityClaim("fabric.queue.max_rx_queues", Unit::Count, 128),
                    QuantityClaim("fabric.queue.max_tx_queues", Unit::Count, 128),
                    BoolClaim("fabric.offload.checksum_rx", true),
                    BoolClaim("fabric.offload.checksum_tx", true),
                    BoolClaim("fabric.offload.segmentation_offload", true),
                    BoolClaim("fabric.offload.rdma", false)};
  const auto published = registry.Publish(request);
  if (!published.HasValue() || !published.Value().Committed()) return 1;
  const auto set = registry.QueryEntity(nic);
  if (!set.HasValue()) return 1;
  std::printf("%s\n", set.Value().ToText().c_str());
  std::printf("note=declared support only; no queue is allocated and no offload is enabled\n");
  std::printf("example=03 result=ok\n");
  return 0;
}
