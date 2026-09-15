// Fabric Capability Registry test suite: queries, snapshots, diffs, explanations.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/capability/fabric_capability.hpp"
#include "registry_fixture.hpp"
#include "test_framework.hpp"

using namespace fabric::capability;
using namespace fcr::test;

namespace {

void PublishNic(Fixture& fixture, const char* name, std::uint64_t rx_queues) {
  const EntityId entity = Entity(name);
  FCR_REQUIRE_OK(fixture.PublishSnapshot(entity, EntityGeneration::FromValue(1),
                                         {SpeedSetClaim({100'000'000'000ull}),
                                          QuantityClaim("fabric.queue.max_rx_queues", Unit::Count,
                                                        rx_queues),
                                          TelemetryClaim({0, 1})},
                                         {}, Coverage::FullEnumeration, "p-snapshot", "a-snapshot"));
}

}  // namespace

FCR_TEST(query, entity_and_capability_lookups) {
  Fixture fixture;
  PublishNic(fixture, "nic:a", 32);
  PublishNic(fixture, "nic:b", 64);

  FCR_CHECK_EQ(fixture.registry->Entities().size(), std::size_t(2));
  const std::vector<EntityId> supporting = fixture.registry->EntitiesSupporting(
      Cap("fabric.queue.max_rx_queues"));
  FCR_CHECK_EQ(supporting.size(), std::size_t(2));
  FCR_CHECK(supporting[0] < supporting[1]);

  const std::vector<CapabilityId> namespace_capabilities =
      fixture.registry->CapabilitiesInNamespace(*CapabilityNamespaceId::Parse("fabric.port"));
  FCR_CHECK(!namespace_capabilities.empty());

  auto entity_set = fixture.registry->QueryEntity(Entity("nic:a"));
  FCR_REQUIRE_OK(entity_set);
  FCR_CHECK_EQ(entity_set.Value().capabilities.size(), std::size_t(3));
  FCR_CHECK_EQ(entity_set.Value().CountOf(CapabilityState::Supported), std::size_t(3));
  FCR_CHECK(entity_set.Value().Find(Cap("fabric.port.supported_speeds")) != nullptr);

  FCR_CHECK_CODE(fixture.registry->QueryEntity(Entity("nic:missing")), ErrorCode::UnknownEntity);

  // An entity that is known but has no claim for the capability is UNKNOWN and
  // never implicitly unsupported.
  FCR_REQUIRE_OK(fixture.PublishSnapshot(Entity("nic:c"), EntityGeneration::FromValue(1),
                                         {SpeedSetClaim({100'000'000'000ull})}, {},
                                         Coverage::FullEnumeration, "p-c", "a-c"));
  auto unknown = fixture.registry->Query(Entity("nic:c"), Cap("fabric.queue.max_rx_queues"));
  FCR_REQUIRE_OK(unknown);
  FCR_CHECK(unknown.Value().entity_known);
  FCR_CHECK(!unknown.Value().record_exists);
  FCR_CHECK(unknown.Value().state == CapabilityState::Unknown);
  FCR_CHECK(!unknown.Value().actionable);
  FCR_CHECK(unknown.Value().fails_closed);
  FCR_CHECK(!unknown.Value().explanation.steps.empty());

  // An entity that is not known at all is also UNKNOWN and fails closed.
  auto missing = fixture.registry->Query(Entity("nic:never-seen"), Cap("fabric.queue.max_rx_queues"));
  FCR_REQUIRE_OK(missing);
  FCR_CHECK(!missing.Value().entity_known);
  FCR_CHECK(missing.Value().state == CapabilityState::Unknown);
  FCR_CHECK(missing.Value().fails_closed);
}

FCR_TEST(query, evidence_and_explanations_are_deterministic) {
  Fixture fixture;
  PublishNic(fixture, "nic:a", 32);

  const std::vector<EvidenceSummary> evidence =
      fixture.registry->EvidenceFor(Entity("nic:a"), Cap("fabric.port.supported_speeds"));
  FCR_CHECK_EQ(evidence.size(), std::size_t(1));
  FCR_CHECK(evidence.front().winning);
  FCR_CHECK(evidence.front().currentness == EvidenceCurrentness::Current);

  const Explanation first = fixture.registry->ExplainCapability(Entity("nic:a"),
                                                               Cap("fabric.port.supported_speeds"));
  const Explanation second = fixture.registry->ExplainCapability(Entity("nic:a"),
                                                                Cap("fabric.port.supported_speeds"));
  FCR_CHECK_EQ(first.ToText(), second.ToText());
  FCR_CHECK(!first.code.empty());
  FCR_CHECK(first.ToText().find("direct-hardware-enumeration") != std::string::npos);
  FCR_CHECK(first.steps.size() >= 2);

  // Publication explanations are retained and explainable.
  const Explanation publication = fixture.registry->ExplainPublication(*PublicationId::Parse("p-snapshot")).HasValue()
                                      ? fixture.registry->ExplainPublication(*PublicationId::Parse("p-snapshot")).Value()
                                      : Explanation{};
  FCR_CHECK(!publication.steps.empty());
  FCR_CHECK(publication.ToText().find("publication.journal") != std::string::npos);
  FCR_CHECK_CODE(fixture.registry->ExplainPublication(*PublicationId::Parse("p-unknown")),
                 ErrorCode::NotFound);
  FCR_CHECK_CODE(fixture.registry->ExplainReplay(*MutationAttemptId::Parse("a-unknown")),
                 ErrorCode::NotFound);

  auto generation = fixture.registry->ExplainGeneration(Entity("nic:a"),
                                                        CapabilitySetGeneration::FromValue(1));
  FCR_REQUIRE_OK(generation);
  FCR_CHECK(generation.Value().ToText().find("generation.advanced") != std::string::npos);
}

FCR_TEST(query, snapshots_are_immutable_and_currentness_is_checkable) {
  Fixture fixture;
  PublishNic(fixture, "nic:a", 32);
  PublishNic(fixture, "nic:b", 64);

  SnapshotScope scope;
  auto snapshot = fixture.registry->CreateSnapshot(scope);
  FCR_REQUIRE_OK(snapshot);
  FCR_CHECK_EQ(snapshot.Value().EntityCount(), std::size_t(2));
  FCR_CHECK(snapshot.Value().id.IsSet());
  FCR_CHECK(!snapshot.Value().digest.IsZero());
  FCR_CHECK(fixture.registry->CheckSnapshot(snapshot.Value()).current);

  const std::string text = snapshot.Value().ToText();
  FCR_CHECK(text.find("snapshot=") != std::string::npos);

  // A later mutation makes the snapshot non current, and says why.
  FCR_REQUIRE_OK(fixture.PublishSnapshot(Entity("nic:a"), EntityGeneration::FromValue(1),
                                         {SpeedSetClaim({400'000'000'000ull})},
                                         CapabilitySetGeneration::FromValue(1),
                                         Coverage::FullEnumeration, "p-2", "a-2"));
  const SnapshotCurrentness currentness = fixture.registry->CheckSnapshot(snapshot.Value());
  FCR_CHECK(!currentness.current);
  FCR_CHECK(!currentness.reasons.empty());
  FCR_CHECK(currentness.ToText().find("current=false") != std::string::npos);

  // The old snapshot still describes the old truth and diffs cleanly.
  const SnapshotEntityEntry* entry = snapshot.Value().Find(Entity("nic:a"));
  FCR_REQUIRE(entry != nullptr);
  FCR_REQUIRE(entry->set != nullptr);
  FCR_CHECK_EQ(entry->set->Find(Cap("fabric.port.supported_speeds"))->value.ToText(),
               std::string("{100000000000 bit/s}"));

  auto diff = fixture.registry->DiffEntityAgainstSnapshot(snapshot.Value(), Entity("nic:a"));
  FCR_REQUIRE_OK(diff);
  FCR_CHECK(!diff.Value().Empty());
  bool saw_value_change = false;
  for (const CapabilityDiffEntry& change : diff.Value().entries) {
    if (change.kind == CapabilityDiffKind::ValueChanged) saw_value_change = true;
  }
  FCR_CHECK(saw_value_change);
  FCR_CHECK(diff.Value().ToText().find("value-changed") != std::string::npos);

  // A snapshot scope filtered by capability namespace only covers entities
  // that hold a capability in that namespace.
  SnapshotScope filtered;
  filtered.all_entities = true;
  filtered.namespaces = {*CapabilityNamespaceId::Parse("fabric.telemetry")};
  auto telemetry_snapshot = fixture.registry->CreateSnapshot(filtered);
  FCR_REQUIRE_OK(telemetry_snapshot);
  FCR_CHECK_EQ(telemetry_snapshot.Value().EntityCount(), std::size_t(2));
}

FCR_TEST(query, digests_are_insertion_order_independent) {
  Fixture fixture;
  PublishNic(fixture, "nic:a", 32);
  const Digest first = fixture.registry->ComputeDigest();
  FCR_CHECK_EQ(fixture.registry->ComputeDigest().ToString(), first.ToString());

  // Re-publishing the identical claim set from a fresh attempt changes the
  // evidence generations, so the digest legitimately changes; publishing to a
  // second registry in a different order must produce the same digest.
  Fixture other;
  PublishNic(other, "nic:b", 64);
  PublishNic(other, "nic:a", 32);
  Fixture reference;
  PublishNic(reference, "nic:a", 32);
  PublishNic(reference, "nic:b", 64);
  FCR_CHECK_EQ(other.registry->ComputeDigest().ToString(),
               reference.registry->ComputeDigest().ToString());

  const RegistryStatistics stats = fixture.registry->Statistics();
  FCR_CHECK_EQ(stats.entities, std::size_t(1));
  FCR_CHECK_EQ(stats.supported, std::size_t(3));
  FCR_CHECK_EQ(stats.unknown, std::size_t(0));
  FCR_CHECK_EQ(stats.epoch.Value(), 1ull);
  FCR_CHECK(fixture.registry->Generation().Value() >= 1);
}
