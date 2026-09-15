// Fabric Capability Registry example: persistence and conservative recovery.
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

#include <filesystem>

int main() {
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() / "fcr-example-10";
  std::error_code error;
  std::filesystem::remove_all(directory, error);
  PersistenceConfig config;
  config.directory = directory;
  config.file_stem = "example";

  CapabilityRegistry registry;
  const AuthorityGrant grant = ExampleGrant({"fabric.offload", "fabric.queue"});
  if (!registry.DeclareAuthority(grant).HasValue()) return 1;
  const WorkerBootId boot = *WorkerBootId::Parse("99999999999999999999999999999999");
  if (!registry.RegisterPublisher(*PublisherId::Parse(kPublisher), grant.scope, boot).HasValue()) {
    return 1;
  }
  const EntityId nic = *EntityId::Parse("nic:example-10");
  CapabilityClaim declaration = BoolClaim("fabric.offload.rdma", true,
                                     ProvenanceClass::AuthoritativeAdministrativeDeclaration,
                                     EvidenceSourceClass::AdministrativeDeclaration,
                                     DurabilityClass::Durable);
  PublicationRequest request =
      Request(grant, boot, nic, "example-publication-10", EntityGeneration::FromValue(1));
  request.epoch = registry.CurrentEpoch();
  request.claims = {declaration, QuantityClaim("fabric.queue.max_rx_queues", Unit::Count, 32)};
  const auto published = registry.Publish(request);
  if (!published.HasValue() || !published.Value().Committed()) return 1;
  const auto saved = registry.Save(config);
  if (!saved.HasValue()) return 1;
  std::printf("saved records=%zu bytes=%zu digest=%s\n", saved.Value().entity_records,
              saved.Value().bytes, saved.Value().payload_digest.ToString().c_str());

  CapabilityRegistry recovered;
  const auto loaded = recovered.Load(config);
  if (!loaded.HasValue()) return 1;
  recovered.AdvanceCoordinatorEpoch(*ReasonToken::Parse("coordinator restart"));
  std::printf("recovery durable=%zu revalidation_required=%zu\n",
              loaded.Value().durable_evidence_retained,
              loaded.Value().evidence_revalidation_required);
  std::printf("durable_declaration state=%s\n",
              std::string(CapabilityStateName(
                              recovered.Query(nic, declaration.capability).Value().state)).c_str());
  std::printf("process_bound_observation state=%s\n",
              std::string(CapabilityStateName(
                              recovered.Query(nic, *CapabilityId::Parse("fabric.queue.max_rx_queues"))
                                  .Value()
                                  .state)).c_str());
  std::filesystem::remove_all(directory, error);
  std::printf("example=10 result=ok\n");
  return 0;
}
