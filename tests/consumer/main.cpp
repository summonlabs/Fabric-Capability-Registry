// Independent downstream consumer of the installed Fabric Capability Registry
// package. This program only uses find_package(FabricCapabilityRegistry) and
// the installed headers; it never sees the source tree.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdio>
#include <string>
#include <vector>

#include "fabric/capability/fabric_capability.hpp"

int main() {
  using namespace fabric::capability;

  CapabilityRegistry registry;
  AuthorityGrant grant;
  grant.scope = *AuthorityScopeId::Parse("consumer-scope");
  grant.entity_kinds = {FabricEntityKind::Nic};
  grant.namespaces = {*CapabilityNamespaceId::Parse("fabric.port"),
                      *CapabilityNamespaceId::Parse("fabric.queue")};
  grant.modes = static_cast<std::uint8_t>(PublicationMode::FullSnapshot) |
                static_cast<std::uint8_t>(PublicationMode::PartialObservation);
  grant.strongest_provenance = ProvenanceClass::DirectHardwareEnumeration;
  grant.allowed_publishers = {*PublisherId::Parse("consumer-publisher")};
  if (!registry.DeclareAuthority(grant).HasValue()) return 1;

  const WorkerBootId boot = *WorkerBootId::Parse("c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0");
  if (!registry.RegisterPublisher(*PublisherId::Parse("consumer-publisher"), grant.scope, boot)
           .HasValue()) {
    return 1;
  }

  CapabilityClaim speeds;
  speeds.capability = *CapabilityId::Parse("fabric.port.supported_speeds");
  speeds.state = CapabilityState::Supported;
  speeds.value = *CapabilityValue::NumericSetValue(
      Unit::BitsPerSecond,
      std::vector<std::uint64_t>{100'000'000'000ull, 400'000'000'000ull});
  speeds.provenance = ProvenanceClass::DirectHardwareEnumeration;
  speeds.source_class = EvidenceSourceClass::HardwareEnumeration;

  const EntityId nic = *EntityId::Parse("nic:consumer-0");
  PublicationRequest request;
  request.mode = PublicationMode::FullSnapshot;
  request.publication = *PublicationId::Parse("consumer-publication");
  request.attempt = *MutationAttemptId::Parse("consumer-attempt");
  request.epoch = registry.CurrentEpoch();
  request.authority = AuthorityContext{grant.scope, *PublisherId::Parse("consumer-publisher"),
                                       boot, *SourceId::Parse("consumer-source"),
                                       SourceGeneration::FromValue(1)};
  request.entity = nic;
  request.entity_generation = EntityGeneration::FromValue(1);
  request.coverage = Coverage::FullEnumeration;
  request.claims = {speeds};

  const auto published = registry.Publish(request);
  if (!published.HasValue() || !published.Value().Committed()) return 1;
  std::printf("published status=%s set_generation=%s\n",
              std::string(PublicationStatusName(published.Value().status)).c_str(),
              published.Value().new_set_generation.ToString().c_str());

  Requirement requirement;
  requirement.kind = RequirementKind::Minimum;
  requirement.capability = speeds.capability;
  requirement.operand = *CapabilityValue::QuantityValue(200'000'000'000ull, Unit::BitsPerSecond);

  const RequirementEvaluation evaluation = registry.Evaluate(nic, requirement);
  std::printf("requirement outcome=%s\n",
              std::string(RequirementOutcomeName(evaluation.outcome)).c_str());
  std::printf("library=%s version=%s\n", kLibraryName, kVersionString);
  if (evaluation.outcome != RequirementOutcome::Satisfied) return 1;
  std::printf("consumer=ok\n");
  return 0;
}
