// Fabric Capability Registry test support: registry fixtures.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "fabric/capability/fabric_capability.hpp"
#include "test_framework.hpp"

namespace fcr::test {

using namespace fabric::capability;

/// Deterministic worker boot identifier derived from a small seed.
inline WorkerBootId Boot(std::uint64_t seed) {
  std::array<char, 32> digits{};
  const char* hex = "0123456789abcdef";
  std::uint64_t state = seed * 6364136223846793005ull + 1442695040888963407ull;
  for (std::size_t index = 0; index < digits.size(); ++index) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    digits[index] = hex[state & 0xF];
  }
  auto parsed = WorkerBootId::Parse(std::string(digits.begin(), digits.end()));
  return parsed.HasValue() ? parsed.Value() : WorkerBootId{};
}

inline CapabilityId Cap(const char* text) {
  auto parsed = CapabilityId::Parse(text);
  return parsed.HasValue() ? parsed.Value() : CapabilityId{};
}

inline EntityId Entity(const char* text) {
  auto parsed = EntityId::Parse(text);
  return parsed.HasValue() ? parsed.Value() : EntityId{};
}

inline ReasonToken Reason(const char* text) {
  auto parsed = ReasonToken::Parse(text);
  return parsed.HasValue() ? parsed.Value() : ReasonToken{};
}

/// Standard test fixture: one registry, one authority scope that may publish
/// for NIC entities in the port, forwarding, offload, queue, telemetry,
/// virtualization and device-management namespaces, and one registered
/// publisher.
struct Fixture {
  Fixture() {
    registry = std::make_unique<CapabilityRegistry>();
    grant.scope = *AuthorityScopeId::Parse("test-scope");
    grant.entity_kinds = {FabricEntityKind::Nic, FabricEntityKind::Switch,
                          FabricEntityKind::Port, FabricEntityKind::Device,
                          FabricEntityKind::SmartNic, FabricEntityKind::Dpu,
                          FabricEntityKind::Router, FabricEntityKind::Link};
    for (const char* ns : {"fabric.port", "fabric.forwarding", "fabric.offload", "fabric.queue",
                           "fabric.telemetry", "fabric.virtualization", "fabric.protocol",
                           "fabric.rdma", "fabric.congestion", "fabric.qos", "fabric.tunneling",
                           "fabric.timestamping", "fabric.optics", "fabric.device-management",
                           "fabric.buffer-exposure", "fabric.compatibility"}) {
      auto parsed = CapabilityNamespaceId::Parse(ns);
      if (parsed.HasValue()) grant.namespaces.push_back(parsed.Value());
    }
    grant.modes = static_cast<std::uint8_t>(PublicationMode::FullSnapshot) |
                  static_cast<std::uint8_t>(PublicationMode::Incremental) |
                  static_cast<std::uint8_t>(PublicationMode::PartialObservation);
    grant.exclusive = false;
    grant.may_publish_durable = true;
    grant.strongest_provenance = ProvenanceClass::DirectHardwareEnumeration;
    grant.allowed_publishers = {publisher};
    if (!registry->DeclareAuthority(grant).HasValue()) {
      FCR_FAIL("fixture authority declaration failed");
    }
    boot = Boot(1);
    auto registered = registry->RegisterPublisher(publisher, grant.scope, boot);
    if (!registered.HasValue()) {
      FCR_FAIL("fixture publisher registration failed");
    }
  }

  AuthorityScopeId scope() const { return grant.scope; }

  /// Publishes a full snapshot with the given claims.
  /// Publication identifiers default to a unique pair per call so that unrelated
  /// publications never collide on the replay cache. A test that exercises replay
  /// pins the same identifiers explicitly.
  Outcome<PublicationResult> PublishSnapshot(const EntityId& entity, EntityGeneration generation,
                                             std::vector<CapabilityClaim> claims,
                                             CapabilitySetGeneration expected = {},
                                             Coverage coverage = Coverage::FullEnumeration,
                                             const char* publication = nullptr,
                                             const char* attempt = nullptr,
                                             std::optional<std::uint64_t> pinned_source = {}) {
    const std::uint64_t sequence = ++sequence_;
    const std::string publication_text =
        publication != nullptr ? std::string(publication) : "p-" + std::to_string(sequence);
    const std::string attempt_text =
        attempt != nullptr ? std::string(attempt) : "a-" + std::to_string(sequence);
    PublicationRequest request;
    request.mode = PublicationMode::FullSnapshot;
    request.publication = *PublicationId::Parse(publication_text);
    request.attempt = *MutationAttemptId::Parse(attempt_text);
    request.epoch = registry->CurrentEpoch();
    request.authority = AuthorityContext{
        grant.scope, publisher, boot, *SourceId::Parse("test-source"),
        SourceGeneration::FromValue(pinned_source.has_value() ? *pinned_source
                                                             : ++source_generation)};
    request.entity = entity;
    request.entity_generation = generation;
    request.expected_set_generation = expected;
    request.coverage = coverage;
    request.claims = std::move(claims);
    request.reason = Reason("test-publication");
    return registry->Publish(request);
  }

  Outcome<PublicationResult> PublishPartial(const EntityId& entity, EntityGeneration generation,
                                            std::vector<CapabilityClaim> claims,
                                            CapabilitySetGeneration expected,
                                            const char* publication, const char* attempt) {
    PublicationRequest request;
    request.mode = PublicationMode::PartialObservation;
    request.publication = *PublicationId::Parse(publication);
    request.attempt = *MutationAttemptId::Parse(attempt);
    request.epoch = registry->CurrentEpoch();
    request.authority = AuthorityContext{grant.scope, publisher, boot,
                                         *SourceId::Parse("test-source"),
                                         SourceGeneration::FromValue(++source_generation)};
    request.entity = entity;
    request.entity_generation = generation;
    request.expected_set_generation = expected;
    request.coverage = Coverage::Partial;
    request.claims = std::move(claims);
    return registry->Publish(request);
  }

  Outcome<PublicationResult> PublishIncremental(const EntityId& entity,
                                                EntityGeneration generation,
                                                std::vector<IncrementalEdit> edits,
                                                CapabilitySetGeneration expected,
                                                const char* publication, const char* attempt) {
    PublicationRequest request;
    request.mode = PublicationMode::Incremental;
    request.publication = *PublicationId::Parse(publication);
    request.attempt = *MutationAttemptId::Parse(attempt);
    request.epoch = registry->CurrentEpoch();
    request.authority = AuthorityContext{grant.scope, publisher, boot,
                                         *SourceId::Parse("test-source"),
                                         SourceGeneration::FromValue(++source_generation)};
    request.entity = entity;
    request.entity_generation = generation;
    request.expected_set_generation = expected;
    request.edits = std::move(edits);
    return registry->Publish(request);
  }

  std::unique_ptr<CapabilityRegistry> registry;
  AuthorityGrant grant;
  PublisherId publisher = *PublisherId::Parse("test-publisher");
  WorkerBootId boot;
  std::uint64_t source_generation = 0;
  std::uint64_t sequence_ = 0;
};

/// Claim builders used across the suites.
inline CapabilityClaim BoolClaim(const char* capability, bool value,
                                 ProvenanceClass provenance = ProvenanceClass::DirectHardwareEnumeration,
                                 EvidenceSourceClass source_class =
                                     EvidenceSourceClass::HardwareEnumeration,
                                 DurabilityClass durability = DurabilityClass::ProcessBound) {
  CapabilityClaim claim;
  claim.capability = Cap(capability);
  claim.state = CapabilityState::Supported;
  claim.value = CapabilityValue::Boolean(value);
  claim.provenance = provenance;
  claim.source_class = source_class;
  claim.durability = durability;
  return claim;
}

inline CapabilityClaim QuantityClaim(const char* capability, Unit unit, std::uint64_t value,
                                     ProvenanceClass provenance =
                                         ProvenanceClass::DirectHardwareEnumeration,
                                     EvidenceSourceClass source_class =
                                         EvidenceSourceClass::HardwareEnumeration) {
  CapabilityClaim claim;
  claim.capability = Cap(capability);
  claim.state = CapabilityState::Supported;
  auto built = CapabilityValue::QuantityValue(value, unit);
  if (built.HasValue()) claim.value = built.Value();
  claim.provenance = provenance;
  claim.source_class = source_class;
  return claim;
}

inline CapabilityClaim SpeedSetClaim(std::initializer_list<std::uint64_t> speeds,
                                     ProvenanceClass provenance =
                                         ProvenanceClass::DirectHardwareEnumeration) {
  CapabilityClaim claim;
  claim.capability = Cap("fabric.port.supported_speeds");
  claim.state = CapabilityState::Supported;
  auto built = CapabilityValue::NumericSetValue(Unit::BitsPerSecond, speeds);
  if (built.HasValue()) claim.value = built.Value();
  claim.provenance = provenance;
  claim.source_class = EvidenceSourceClass::HardwareEnumeration;
  return claim;
}

inline CapabilityClaim MtuRangeClaim(std::uint64_t minimum, std::uint64_t maximum) {
  CapabilityClaim claim;
  claim.capability = Cap("fabric.port.mtu_range");
  claim.state = CapabilityState::Supported;
  auto built = CapabilityValue::NumericRangeValue(Unit::Bytes, minimum, maximum);
  if (built.HasValue()) claim.value = built.Value();
  claim.provenance = ProvenanceClass::DirectHardwareEnumeration;
  claim.source_class = EvidenceSourceClass::HardwareEnumeration;
  return claim;
}

inline CapabilityClaim ProtocolClaim(std::initializer_list<ProtocolId> protocols) {
  CapabilityClaim claim;
  claim.capability = Cap("fabric.protocol.families");
  claim.state = CapabilityState::Supported;
  auto built = CapabilityValue::ProtocolSetValue(protocols);
  if (built.HasValue()) claim.value = built.Value();
  claim.provenance = ProvenanceClass::DirectHardwareEnumeration;
  claim.source_class = EvidenceSourceClass::HardwareEnumeration;
  return claim;
}

inline CapabilityClaim TelemetryClaim(std::initializer_list<std::uint32_t> families) {
  CapabilityClaim claim;
  claim.capability = Cap("fabric.telemetry.families");
  claim.state = CapabilityState::Supported;
  auto domain = EnumDomainId::Parse("fabric.telemetry.family");
  if (domain.HasValue()) {
    auto built = CapabilityValue::EnumerationSetValue(domain.Value(), families);
    if (built.HasValue()) claim.value = built.Value();
  }
  claim.provenance = ProvenanceClass::DirectDeviceOrOsApi;
  claim.source_class = EvidenceSourceClass::DriverApi;
  return claim;
}

}  // namespace fcr::test
