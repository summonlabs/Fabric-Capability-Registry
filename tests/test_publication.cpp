// Fabric Capability Registry test suite: transactional capability publication.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/capability/fabric_capability.hpp"
#include "registry_fixture.hpp"
#include "test_framework.hpp"

using namespace fabric::capability;
using namespace fcr::test;

namespace {

EntityId TestNic() { return Entity("nic:test-0"); }

}  // namespace

FCR_TEST(publication, full_snapshot_commits_atomically) {
  Fixture fixture;
  const EntityId entity = TestNic();
  const EntityGeneration generation = EntityGeneration::FromValue(1);

  std::vector<CapabilityClaim> claims = {
      SpeedSetClaim({100'000'000'000ull, 400'000'000'000ull}),
      MtuRangeClaim(1500, 9216),
      BoolClaim("fabric.forwarding.ecmp_supported", true),
      QuantityClaim("fabric.queue.max_rx_queues", Unit::Count, 128)};

  auto result = fixture.PublishSnapshot(entity, generation, claims);
  FCR_REQUIRE_OK(result);
  FCR_CHECK(result.Value().Committed());
  FCR_CHECK_EQ(result.Value().claims_applied, std::size_t(4));
  FCR_CHECK_EQ(result.Value().new_set_generation.Value(), 1ull);
  FCR_CHECK_EQ(result.Value().diff.entries.size(), std::size_t(4));

  auto query = fixture.registry->Query(entity, Cap("fabric.port.supported_speeds"));
  FCR_REQUIRE_OK(query);
  FCR_CHECK(query.Value().actionable);
  FCR_CHECK(query.Value().state == CapabilityState::Supported);
  FCR_CHECK(query.Value().has_value);
  FCR_CHECK_EQ(query.Value().value.ToText(),
               std::string("{100000000000 bit/s, 400000000000 bit/s}"));
  FCR_CHECK(query.Value().provenance == ProvenanceClass::DirectHardwareEnumeration);

  auto queues = fixture.registry->Query(entity, Cap("fabric.queue.max_rx_queues"));
  FCR_REQUIRE_OK(queues);
  FCR_CHECK_EQ(queues.Value().value.ToText(), std::string("128 count"));

  // A capability that was never claimed is UNKNOWN, not UNSUPPORTED.
  auto unclaimed = fixture.registry->Query(entity, Cap("fabric.offload.rdma"));
  FCR_REQUIRE_OK(unclaimed);
  FCR_CHECK(unclaimed.Value().state == CapabilityState::Unknown);
  FCR_CHECK(!unclaimed.Value().actionable);
  FCR_CHECK(unclaimed.Value().fails_closed);
  FCR_CHECK(!unclaimed.Value().record_exists);
}

FCR_TEST(publication, rejected_publication_leaves_no_trace) {
  Fixture fixture;
  const EntityId entity = TestNic();
  const EntityGeneration generation = EntityGeneration::FromValue(1);
  FCR_REQUIRE_OK(fixture.PublishSnapshot(entity, generation,
                                         {SpeedSetClaim({100'000'000'000ull})}));

  auto before = fixture.registry->QueryEntity(entity);
  FCR_REQUIRE_OK(before);
  const Digest before_digest = before.Value().digest;
  const std::uint64_t before_generation = before.Value().set_generation.Value();

  // One malformed claim rejects the whole authoritative snapshot.
  std::vector<CapabilityClaim> claims = {SpeedSetClaim({400'000'000'000ull}),
                                         QuantityClaim("fabric.queue.max_rx_queues", Unit::Bytes, 1)};
  auto rejected = fixture.PublishSnapshot(entity, generation, claims, CapabilitySetGeneration::FromValue(1),
                                          Coverage::FullEnumeration, "p-2", "a-2");
  FCR_REQUIRE_OK(rejected);
  FCR_CHECK(rejected.Value().Rejected());
  FCR_CHECK(rejected.Value().code == ErrorCode::ValueTypeMismatch);
  FCR_CHECK(!rejected.Value().explanation.summary.empty());

  auto after = fixture.registry->QueryEntity(entity);
  FCR_REQUIRE_OK(after);
  FCR_CHECK(after.Value().digest == before_digest);
  FCR_CHECK_EQ(after.Value().set_generation.Value(), before_generation);
  auto speeds = fixture.registry->Query(entity, Cap("fabric.port.supported_speeds"));
  FCR_REQUIRE_OK(speeds);
  FCR_CHECK_EQ(speeds.Value().value.ToText(), std::string("{100000000000 bit/s}"));
}

FCR_TEST(publication, idempotent_replay_does_not_advance_generation) {
  Fixture fixture;
  const EntityId entity = TestNic();
  const EntityGeneration generation = EntityGeneration::FromValue(1);
  std::vector<CapabilityClaim> claims = {SpeedSetClaim({100'000'000'000ull})};

  auto first = fixture.PublishSnapshot(entity, generation, claims, {}, Coverage::FullEnumeration,
                                       "p-1", "a-1", 1);
  FCR_REQUIRE_OK(first);
  FCR_CHECK(first.Value().Committed());
  FCR_CHECK_EQ(first.Value().new_set_generation.Value(), 1ull);

  // Exact replay: same attempt, same content.
  auto replay = fixture.PublishSnapshot(entity, generation, claims, CapabilitySetGeneration::FromValue(1),
                                        Coverage::FullEnumeration, "p-1", "a-1", 1);
  FCR_REQUIRE_OK(replay);
  FCR_CHECK(replay.Value().Idempotent());
  FCR_CHECK_EQ(replay.Value().new_set_generation.Value(), 1ull);
  FCR_CHECK_EQ(replay.Value().previous_set_generation.Value(), 1ull);

  // The same attempt with different content is a conflict, not an idempotent replay.
  std::vector<CapabilityClaim> different = {SpeedSetClaim({200'000'000'000ull})};
  auto conflict = fixture.PublishSnapshot(entity, generation, different,
                                          CapabilitySetGeneration::FromValue(1),
                                          Coverage::FullEnumeration, "p-1b", "a-1", 1);
  FCR_REQUIRE_OK(conflict);
  FCR_CHECK(conflict.Value().Rejected());
  FCR_CHECK(conflict.Value().code == ErrorCode::DuplicateAttemptConflict);

  // An older attempt that was already superseded is a stale replay, never an
  // idempotent success.
  auto second = fixture.PublishSnapshot(entity, generation, different,
                                        CapabilitySetGeneration::FromValue(1),
                                        Coverage::FullEnumeration, "p-2", "a-2", 2);
  FCR_REQUIRE_OK(second);
  FCR_CHECK(second.Value().Committed());
  FCR_CHECK_EQ(second.Value().new_set_generation.Value(), 2ull);

  auto stale = fixture.PublishSnapshot(entity, generation, claims, CapabilitySetGeneration::FromValue(2),
                                       Coverage::FullEnumeration, "p-1", "a-1", 1);
  FCR_REQUIRE_OK(stale);
  FCR_CHECK(stale.Value().Rejected());
  FCR_CHECK_EQ(std::string(ErrorCodeName(stale.Value().code)), std::string("stale-replay"));
  FCR_CHECK(IsStaleRejection(stale.Value().code));
}

FCR_TEST(publication, generation_guards) {
  Fixture fixture;
  const EntityId entity = TestNic();
  const EntityGeneration generation = EntityGeneration::FromValue(2);
  FCR_REQUIRE_OK(fixture.PublishSnapshot(entity, generation, {SpeedSetClaim({100'000'000'000ull})}));

  // Stale expected capability set generation.
  auto stale = fixture.PublishSnapshot(entity, generation, {SpeedSetClaim({200'000'000'000ull})},
                                       CapabilitySetGeneration::FromValue(0),
                                       Coverage::FullEnumeration, "p-2", "a-2");
  FCR_REQUIRE_OK(stale);
  FCR_CHECK(stale.Value().Rejected());
  FCR_CHECK(stale.Value().code == ErrorCode::CapabilitySetGenerationStale);

  // Future capability set generation.
  auto future = fixture.PublishSnapshot(entity, generation, {SpeedSetClaim({200'000'000'000ull})},
                                        CapabilitySetGeneration::FromValue(7),
                                        Coverage::FullEnumeration, "p-3", "a-3");
  FCR_REQUIRE_OK(future);
  FCR_CHECK(future.Value().Rejected());
  FCR_CHECK(future.Value().code == ErrorCode::CapabilitySetGenerationMismatch);

  // Stale entity generation: superseded identities never accept claims again.
  auto stale_entity = fixture.PublishSnapshot(entity, EntityGeneration::FromValue(1),
                                              {SpeedSetClaim({200'000'000'000ull})}, {},
                                              Coverage::FullEnumeration, "p-4", "a-4");
  FCR_REQUIRE_OK(stale_entity);
  FCR_CHECK(stale_entity.Value().Rejected());
  FCR_CHECK(stale_entity.Value().code == ErrorCode::EntityGenerationStale);

  // First publication for a new generation must expect no existing set.
  auto advanced = fixture.PublishSnapshot(entity, EntityGeneration::FromValue(3),
                                          {SpeedSetClaim({400'000'000'000ull})}, {},
                                          Coverage::FullEnumeration, "p-5", "a-5");
  FCR_REQUIRE_OK(advanced);
  FCR_CHECK(advanced.Value().Committed());
  FCR_CHECK_EQ(advanced.Value().new_set_generation.Value(), 1ull);
}

FCR_TEST(publication, claim_shape_validation) {
  Fixture fixture;
  const EntityId entity = TestNic();
  const EntityGeneration generation = EntityGeneration::FromValue(1);

  // Unknown capability is rejected.
  CapabilityClaim unknown;
  unknown.capability = Cap("fabric.port.unknown_capability");
  unknown.state = CapabilityState::Supported;
  unknown.value = CapabilityValue::Boolean(true);
  auto rejected = fixture.PublishSnapshot(entity, generation, {unknown});
  FCR_REQUIRE_OK(rejected);
  FCR_CHECK(rejected.Value().Rejected());
  FCR_CHECK(rejected.Value().code == ErrorCode::UnknownCapability);

  // Duplicate declaration of one capability inside a single publication.
  auto duplicate = fixture.PublishSnapshot(entity, generation,
                                           {SpeedSetClaim({100'000'000'000ull}),
                                            SpeedSetClaim({200'000'000'000ull})});
  FCR_REQUIRE_OK(duplicate);
  FCR_CHECK(duplicate.Value().Rejected());
  FCR_CHECK(duplicate.Value().code == ErrorCode::DuplicateClaim);

  // A typed capability claim without a value cannot be published.
  CapabilityClaim missing_value;
  missing_value.capability = Cap("fabric.queue.max_rx_queues");
  missing_value.state = CapabilityState::Supported;
  auto missing = fixture.PublishSnapshot(entity, generation, {missing_value});
  FCR_REQUIRE_OK(missing);
  FCR_CHECK(missing.Value().Rejected());
  FCR_CHECK(missing.Value().code == ErrorCode::ValueTypeMismatch);

  // An UNKNOWN claim must not carry a value.
  CapabilityClaim contradictory;
  contradictory.capability = Cap("fabric.queue.max_rx_queues");
  contradictory.state = CapabilityState::Unknown;
  auto quantity = CapabilityValue::QuantityValue(64, Unit::Count);
  FCR_REQUIRE_OK(quantity);
  contradictory.value = quantity.Value();
  auto contradiction = fixture.PublishSnapshot(entity, generation, {contradictory});
  FCR_REQUIRE_OK(contradiction);
  FCR_CHECK(contradiction.Value().Rejected());
  FCR_CHECK(contradiction.Value().code == ErrorCode::ValueContradiction);

  // Nothing was committed by any of the rejected publications: the entity either does not
  // exist at all or holds no capability record.
  auto record = fixture.registry->EntityRecord(entity);
  if (record.HasValue()) {
    FCR_CHECK(record.Value().current_set.capabilities.empty());
  } else {
    FCR_CHECK(record.Code() == ErrorCode::UnknownEntity);
  }
}

FCR_TEST(publication, publication_modes_have_distinct_semantics) {
  Fixture fixture;
  const EntityId entity = TestNic();
  const EntityGeneration generation = EntityGeneration::FromValue(1);

  FCR_REQUIRE_OK(fixture.PublishSnapshot(entity, generation,
                                         {SpeedSetClaim({100'000'000'000ull}),
                                          BoolClaim("fabric.forwarding.ecmp_supported", true)}));

  // A partial observation only touches the capabilities it mentions.
  auto partial = fixture.PublishPartial(entity, generation,
                                        {QuantityClaim("fabric.queue.max_rx_queues", Unit::Count, 64)},
                                        CapabilitySetGeneration::FromValue(1), "p-2", "a-2");
  FCR_REQUIRE_OK(partial);
  FCR_CHECK(partial.Value().Committed());
  auto speeds = fixture.registry->Query(entity, Cap("fabric.port.supported_speeds"));
  FCR_REQUIRE_OK(speeds);
  FCR_CHECK(speeds.Value().actionable);
  auto ecmp = fixture.registry->Query(entity, Cap("fabric.forwarding.ecmp_supported"));
  FCR_REQUIRE_OK(ecmp);
  FCR_CHECK(ecmp.Value().actionable);

  // An incremental withdrawal removes only that claim.
  IncrementalEdit edit;
  edit.operation = IncrementalOperation::WithdrawClaim;
  edit.capability = Cap("fabric.forwarding.ecmp_supported");
  auto withdrawn = fixture.PublishIncremental(entity, generation, {edit},
                                              CapabilitySetGeneration::FromValue(2), "p-3", "a-3");
  FCR_REQUIRE_OK(withdrawn);
  FCR_CHECK(withdrawn.Value().Committed());
  auto after_withdrawal = fixture.registry->Query(entity, Cap("fabric.forwarding.ecmp_supported"));
  FCR_REQUIRE_OK(after_withdrawal);
  FCR_CHECK(after_withdrawal.Value().state == CapabilityState::Unknown);
  FCR_CHECK(!after_withdrawal.Value().actionable);

  // A snapshot publishes only the capabilities it enumerates.
  auto snapshot = fixture.PublishSnapshot(entity, generation, {SpeedSetClaim({400'000'000'000ull})},
                                          CapabilitySetGeneration::FromValue(3),
                                          Coverage::FullEnumeration, "p-4", "a-4");
  FCR_REQUIRE_OK(snapshot);
  FCR_CHECK(snapshot.Value().Committed());
  auto queues = fixture.registry->Query(entity, Cap("fabric.queue.max_rx_queues"));
  FCR_REQUIRE_OK(queues);
  FCR_CHECK(queues.Value().state == CapabilityState::Unknown);
}

FCR_TEST(publication, batch_publication_is_ordered) {
  Fixture fixture;
  const EntityGeneration generation = EntityGeneration::FromValue(1);
  std::vector<PublicationRequest> requests;
  for (int index = 0; index < 3; ++index) {
    PublicationRequest request;
    request.mode = PublicationMode::FullSnapshot;
    request.publication = *PublicationId::Parse("p-batch-" + std::to_string(index));
    request.attempt = *MutationAttemptId::Parse("a-batch-" + std::to_string(index));
    request.epoch = fixture.registry->CurrentEpoch();
    request.authority = AuthorityContext{fixture.scope(), fixture.publisher, fixture.boot,
                                         *SourceId::Parse("test-source"),
                                         SourceGeneration::FromValue(
                                             static_cast<std::uint64_t>(index + 1))};
    const std::string batch_name = "nic:batch-" + std::to_string(index);
    request.entity = Entity(batch_name.c_str());
    request.entity_generation = generation;
    request.expected_set_generation = CapabilitySetGeneration{};
    request.claims = {SpeedSetClaim({100'000'000'000ull})};
    requests.push_back(request);
  }
  auto results = fixture.registry->PublishBatch(requests);
  FCR_REQUIRE_OK(results);
  FCR_CHECK_EQ(results.Value().size(), std::size_t(3));
  for (const PublicationResult& result : results.Value()) {
    FCR_CHECK(result.Committed());
  }
  FCR_CHECK_EQ(fixture.registry->Entities().size(), std::size_t(3));
}
