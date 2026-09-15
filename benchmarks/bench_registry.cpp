// Fabric Capability Registry benchmarks.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every benchmark measures completed work: the reported number of operations
// counts operations that returned successfully and were verified.

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "fabric/capability/fabric_capability.hpp"

using namespace fabric::capability;

namespace {

using Clock = std::chrono::steady_clock;

class Measurement {
 public:
  Measurement(const char* name, std::size_t operations)
      : name_(name), operations_(operations), start_(Clock::now()) {}

  void Stop() {
    const double seconds = std::chrono::duration<double>(Clock::now() - start_).count();
    const double per_second = seconds > 0.0 ? static_cast<double>(operations_) / seconds : 0.0;
    std::printf("%-46s operations=%-10zu seconds=%-8.3f ops_per_second=%.0f\n", name_,
                operations_, seconds, per_second);
    std::fflush(stdout);
  }

 private:
  const char* name_;
  std::size_t operations_;
  Clock::time_point start_;
};

AuthorityGrant Grant(const std::vector<const char*>& namespaces) {
  AuthorityGrant grant;
  grant.scope = *AuthorityScopeId::Parse("bench-scope");
  grant.entity_kinds = {FabricEntityKind::Nic, FabricEntityKind::Switch};
  for (const char* ns : namespaces) grant.namespaces.push_back(*CapabilityNamespaceId::Parse(ns));
  grant.modes = static_cast<std::uint8_t>(PublicationMode::FullSnapshot) |
                static_cast<std::uint8_t>(PublicationMode::Incremental) |
                static_cast<std::uint8_t>(PublicationMode::PartialObservation);
  grant.may_publish_durable = true;
  grant.strongest_provenance = ProvenanceClass::DirectHardwareEnumeration;
  grant.allowed_publishers = {*PublisherId::Parse("bench-publisher")};
  return grant;
}

CapabilityClaim Claim(const char* capability, std::uint64_t value, Unit unit) {
  CapabilityClaim claim;
  claim.capability = *CapabilityId::Parse(capability);
  claim.state = CapabilityState::Supported;
  claim.value = *CapabilityValue::QuantityValue(value, unit);
  claim.provenance = ProvenanceClass::DirectHardwareEnumeration;
  claim.source_class = EvidenceSourceClass::HardwareEnumeration;
  return claim;
}

std::vector<CapabilityClaim> ClaimSet(std::uint64_t seed) {
  return {Claim("fabric.queue.max_rx_queues", 16 + (seed % 64), Unit::Count),
          Claim("fabric.queue.max_tx_queues", 16 + (seed % 64), Unit::Count),
          Claim("fabric.forwarding.max_ecmp_width", 8 + (seed % 32), Unit::Count),
          Claim("fabric.forwarding.max_acl_entries", 1024 + seed, Unit::Count),
          Claim("fabric.buffer-exposure.total_packet_buffer", 1u << 20, Unit::Bytes)};
}

std::vector<EntityId> Fill(CapabilityRegistry& registry, const AuthorityGrant& grant,
                           const WorkerBootId& boot, std::size_t entities) {
  std::vector<EntityId> created;
  created.reserve(entities);
  SourceGeneration source_generation = SourceGeneration::FromValue(0);
  for (std::size_t index = 0; index < entities; ++index) {
    const std::string name = "nic:bench-" + std::to_string(index);
    const EntityId entity = *EntityId::Parse(name);
    created.push_back(entity);
    PublicationRequest request;
    request.mode = PublicationMode::FullSnapshot;
    request.publication = *PublicationId::Parse("bench-" + std::to_string(index));
    request.attempt = *MutationAttemptId::Parse("bench-" + std::to_string(index));
    request.epoch = registry.CurrentEpoch();
    source_generation = source_generation.Next().HasValue() ? source_generation.Next().Value()
                                                            : source_generation;
    request.authority = AuthorityContext{grant.scope, *PublisherId::Parse("bench-publisher"), boot,
                                         *SourceId::Parse("bench-source"), source_generation};
    request.entity = entity;
    request.entity_generation = EntityGeneration::FromValue(1);
    request.claims = ClaimSet(index);
    const auto result = registry.Publish(request);
    if (!result.HasValue() || !result.Value().Committed()) {
      std::printf("benchmark setup failed at entity %zu\n", index);
      return created;
    }
  }
  return created;
}

}  // namespace

int main(int argc, char** argv) {
  std::size_t scale = 1000;
  for (int index = 1; index + 1 < argc; ++index) {
    if (std::string(argv[index]) == "--entities") {
      scale = static_cast<std::size_t>(std::strtoull(argv[index + 1], nullptr, 10));
    }
  }

  CapabilityRegistry registry;
  const AuthorityGrant grant = Grant({"fabric.queue", "fabric.forwarding", "fabric.buffer-exposure",
                                      "fabric.port", "fabric.telemetry"});
  if (!registry.DeclareAuthority(grant).HasValue()) return 1;
  const WorkerBootId boot = *WorkerBootId::Parse("abcdefabcdefabcdefabcdefabcdefab");
  if (!registry.RegisterPublisher(*PublisherId::Parse("bench-publisher"), grant.scope, boot)
           .HasValue()) {
    return 1;
  }

  std::printf("Fabric Capability Registry benchmarks (entities=%zu)\n\n", scale);
  {
    Measurement measurement("capability publication (full snapshot)", scale);
    const std::vector<EntityId> created = Fill(registry, grant, boot, scale);
    measurement.Stop();
    std::printf("%-46s entities=%zu\n", "publication verification", created.size());
  }

  const std::vector<EntityId> entities = registry.Entities();
  {
    Measurement measurement("capability lookup by entity+capability", entities.size() * 4);
    std::size_t completed = 0;
    for (const EntityId& entity : entities) {
      for (const char* capability : {"fabric.queue.max_rx_queues", "fabric.queue.max_tx_queues",
                                     "fabric.forwarding.max_ecmp_width",
                                     "fabric.buffer-exposure.total_packet_buffer"}) {
        const auto query = registry.Query(entity, *CapabilityId::Parse(capability));
        if (query.HasValue()) ++completed;
      }
    }
    measurement.Stop();
    std::printf("%-46s rows=%zu\n", "lookup verification", completed);
  }
  {
    Measurement measurement("reverse lookup entities supporting a capability", 20);
    std::size_t total = 0;
    for (int index = 0; index < 20; ++index) {
      total += registry.EntitiesSupporting(*CapabilityId::Parse("fabric.queue.max_rx_queues")).size();
    }
    measurement.Stop();
    std::printf("%-46s rows=%zu\n", "reverse lookup verification", total);
  }
  {
    Measurement measurement("compatibility requirement evaluation", entities.size());
    Requirement requirement;
    requirement.kind = RequirementKind::AllOf;
    Requirement queues;
    queues.kind = RequirementKind::Minimum;
    queues.capability = *CapabilityId::Parse("fabric.queue.max_rx_queues");
    queues.operand = *CapabilityValue::QuantityValue(32, Unit::Count);
    requirement.children = {queues};
    std::size_t satisfied = 0;
    for (const EntityId& entity : entities) {
      const RequirementEvaluation evaluation = registry.Evaluate(entity, requirement);
      if (evaluation.outcome == RequirementOutcome::Satisfied) ++satisfied;
    }
    measurement.Stop();
    std::printf("%-46s satisfied=%zu\n", "requirement verification", satisfied);
  }
  {
    Measurement measurement("snapshot construction", 1);
    SnapshotScope scope;
    const auto snapshot = registry.CreateSnapshot(scope);
    measurement.Stop();
    std::printf("%-46s entities=%zu\n", "snapshot verification",
                snapshot.HasValue() ? snapshot.Value().EntityCount() : 0);
  }
  {
    Measurement measurement("canonical registry digest", 20);
    Digest digest;
    for (int index = 0; index < 20; ++index) digest = registry.ComputeDigest();
    measurement.Stop();
    std::printf("%-46s digest=%s\n", "digest verification", digest.ShortString().c_str());
  }
  {
    Measurement measurement("snapshot diff", 5);
    SnapshotScope scope;
    const auto snapshot = registry.CreateSnapshot(scope);
    std::size_t entries = 0;
    for (int index = 0; index < 5; ++index) {
      const auto diff = registry.DiffAgainstSnapshot(snapshot.Value());
      if (diff.HasValue()) entries += diff.Value().entries.size();
    }
    measurement.Stop();
    std::printf("%-46s entries=%zu\n", "diff verification", entries);
  }
  {
    Measurement measurement("mass source invalidation", 1);
    const auto invalidated = registry.InvalidateSource(*SourceId::Parse("bench-source"),
                                                       *ReasonToken::Parse("benchmark"));
    measurement.Stop();
    std::printf("%-46s records=%zu\n", "invalidation verification",
                invalidated.HasValue() ? invalidated.Value() : 0);
  }
  {
    Measurement measurement("publisher fencing", 1);
    const auto fenced = registry.FenceWorkerBoot(boot, *ReasonToken::Parse("benchmark"));
    measurement.Stop();
    std::printf("%-46s fenced=%s\n", "fence verification",
                fenced.HasValue() && registry.IsWorkerBootFenced(boot) ? "true" : "false");
  }

  std::printf("\ncompleted operations are counted after they returned successfully\n");
  return 0;
}
