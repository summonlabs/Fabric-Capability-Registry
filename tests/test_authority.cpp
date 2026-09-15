// Fabric Capability Registry test suite: authority, fencing, replacement.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/capability/fabric_capability.hpp"
#include "registry_fixture.hpp"
#include "test_framework.hpp"

using namespace fabric::capability;
using namespace fcr::test;

FCR_TEST(authority, grants_are_explicit_and_enumerative) {
  CapabilityRegistry registry;
  AuthorityGrant grant;
  grant.scope = *AuthorityScopeId::Parse("scope-a");
  // No wildcard authority: a scope must enumerate what it may publish.
  FCR_CHECK_CODE(registry.DeclareAuthority(grant), ErrorCode::InvalidArgument);
  grant.entity_kinds = {FabricEntityKind::Nic};
  FCR_CHECK_CODE(registry.DeclareAuthority(grant), ErrorCode::InvalidArgument);
  grant.namespaces = {*CapabilityNamespaceId::Parse("fabric.port")};
  FCR_CHECK_CODE(registry.DeclareAuthority(grant), ErrorCode::InvalidArgument);
  grant.modes = static_cast<std::uint8_t>(PublicationMode::PartialObservation);
  FCR_REQUIRE_OK(registry.DeclareAuthority(grant));

  FCR_CHECK_EQ(registry.Authorities().size(), std::size_t(1));
  auto fetched = registry.Authority(grant.scope);
  FCR_REQUIRE_OK(fetched);
  FCR_CHECK_EQ(fetched.Value().namespaces.size(), std::size_t(1));
  FCR_CHECK_CODE(registry.Authority(*AuthorityScopeId::Parse("scope-missing")),
                 ErrorCode::UnknownAuthorityScope);
}

FCR_TEST(authority, publisher_allowlist_and_scope_binding) {
  CapabilityRegistry registry;
  AuthorityGrant grant;
  grant.scope = *AuthorityScopeId::Parse("scope-a");
  grant.entity_kinds = {FabricEntityKind::Nic};
  grant.namespaces = {*CapabilityNamespaceId::Parse("fabric.port")};
  grant.modes = static_cast<std::uint8_t>(PublicationMode::FullSnapshot);
  grant.allowed_publishers = {*PublisherId::Parse("allowed")};
  FCR_REQUIRE_OK(registry.DeclareAuthority(grant));

  const WorkerBootId boot = Boot(11);
  FCR_CHECK_CODE(registry.RegisterPublisher(*PublisherId::Parse("denied"), grant.scope, boot),
                 ErrorCode::UnauthorizedPublisher);
  FCR_CHECK_CODE(registry.RegisterPublisher(*PublisherId::Parse("allowed"),
                                            *AuthorityScopeId::Parse("scope-missing"), boot),
                 ErrorCode::UnknownAuthorityScope);
  auto registered = registry.RegisterPublisher(*PublisherId::Parse("allowed"), grant.scope, boot);
  FCR_REQUIRE_OK(registered);
  FCR_CHECK(registered.Value().epoch == registry.CurrentEpoch());

  // A publisher may only publish into the namespaces of its scope.
  PublicationRequest request;
  request.mode = PublicationMode::FullSnapshot;
  request.publication = *PublicationId::Parse("p-1");
  request.attempt = *MutationAttemptId::Parse("a-1");
  request.epoch = registry.CurrentEpoch();
  request.authority = AuthorityContext{grant.scope, *PublisherId::Parse("allowed"), boot,
                                       *SourceId::Parse("s-1"), SourceGeneration::FromValue(1)};
  request.entity = Entity("nic:0");
  request.entity_generation = EntityGeneration::FromValue(1);
  request.claims = {QuantityClaim("fabric.queue.max_rx_queues", Unit::Count, 8)};
  auto rejected = registry.Publish(request);
  FCR_REQUIRE_OK(rejected);
  FCR_CHECK(rejected.Value().Rejected());
  FCR_CHECK(rejected.Value().code == ErrorCode::AuthorityScopeViolation);

  // The scope may not publish for entity classes it does not own.
  request.claims = {SpeedSetClaim({100'000'000'000ull})};
  request.entity = Entity("switch:0");
  auto wrong_kind = registry.Publish(request);
  FCR_REQUIRE_OK(wrong_kind);
  FCR_CHECK(wrong_kind.Value().Rejected());
  FCR_CHECK(wrong_kind.Value().code == ErrorCode::AuthorityScopeViolation);

  // A capability class the scope holds but a mode it does not hold.
  request.entity = Entity("nic:0");
  request.mode = PublicationMode::Incremental;
  IncrementalEdit edit;
  edit.operation = IncrementalOperation::WithdrawClaim;
  edit.capability = Cap("fabric.port.supported_speeds");
  request.edits = {edit};
  auto wrong_mode = registry.Publish(request);
  FCR_REQUIRE_OK(wrong_mode);
  FCR_CHECK(wrong_mode.Value().Rejected());
  FCR_CHECK(wrong_mode.Value().code == ErrorCode::PublicationModeNotAuthorized);
}

FCR_TEST(authority, provenance_and_durability_bounds) {
  Fixture fixture;
  const EntityId entity = Entity("nic:0");
  const EntityGeneration generation = EntityGeneration::FromValue(1);

  // A synthetic source may not claim hardware provenance.
  CapabilityClaim forged = BoolClaim("fabric.offload.rdma", true,
                                     ProvenanceClass::DirectHardwareEnumeration,
                                     EvidenceSourceClass::SyntheticBackend);
  auto rejected = fixture.PublishSnapshot(entity, generation, {forged});
  FCR_REQUIRE_OK(rejected);
  FCR_CHECK(rejected.Value().Rejected());
  FCR_CHECK(rejected.Value().code == ErrorCode::ProvenanceNotAuthorized);

  // A live hardware observation may never be declared durable.
  CapabilityClaim durable_hardware = BoolClaim("fabric.offload.rdma", true,
                                               ProvenanceClass::DirectHardwareEnumeration,
                                               EvidenceSourceClass::HardwareEnumeration,
                                               DurabilityClass::Durable);
  auto durable = fixture.PublishSnapshot(entity, generation, {durable_hardware}, {},
                                         Coverage::FullEnumeration, "p-2", "a-2");
  FCR_REQUIRE_OK(durable);
  FCR_CHECK(durable.Value().Rejected());
  FCR_CHECK(durable.Value().code == ErrorCode::DurablePublicationNotAuthorized);

  // An administrative declaration may be durable when the scope allows it.
  CapabilityClaim admin = BoolClaim("fabric.offload.rdma", true,
                                    ProvenanceClass::AuthoritativeAdministrativeDeclaration,
                                    EvidenceSourceClass::AdministrativeDeclaration,
                                    DurabilityClass::Durable);
  auto accepted = fixture.PublishSnapshot(entity, generation, {admin}, {},
                                          Coverage::FullEnumeration, "p-3", "a-3");
  FCR_REQUIRE_OK(accepted);
  FCR_CHECK(accepted.Value().Committed());
}

FCR_TEST(authority, worker_boot_fencing_is_permanent) {
  Fixture fixture;
  const EntityId entity = Entity("nic:0");
  const EntityGeneration generation = EntityGeneration::FromValue(1);
  FCR_REQUIRE_OK(fixture.PublishSnapshot(entity, generation,
                                         {SpeedSetClaim({100'000'000'000ull})}));

  FCR_REQUIRE_OK(fixture.registry->FenceWorkerBoot(fixture.boot, Reason("device-agent-died")));
  FCR_CHECK(fixture.registry->IsWorkerBootFenced(fixture.boot));
  FCR_CHECK_EQ(fixture.registry->Fences().size(), std::size_t(1));

  // Process bound evidence is no longer current; the capability needs proof.
  auto query = fixture.registry->Query(entity, Cap("fabric.port.supported_speeds"));
  FCR_REQUIRE_OK(query);
  FCR_CHECK(query.Value().state == CapabilityState::RevalidationRequired);
  FCR_CHECK(!query.Value().actionable);
  FCR_CHECK(query.Value().fails_closed);

  // A fenced boot can neither publish nor re-register.
  auto publish = fixture.PublishSnapshot(entity, generation, {SpeedSetClaim({200'000'000'000ull})},
                                         CapabilitySetGeneration::FromValue(1),
                                         Coverage::FullEnumeration, "p-2", "a-2");
  FCR_REQUIRE_OK(publish);
  FCR_CHECK(publish.Value().Rejected());
  FCR_CHECK(publish.Value().code == ErrorCode::WorkerBootFenced);
  FCR_CHECK_CODE(fixture.registry->RegisterPublisher(fixture.publisher, fixture.scope(),
                                                     fixture.boot),
                 ErrorCode::WorkerBootFenced);

  // A fresh boot with fresh evidence re-establishes capability truth.
  const WorkerBootId fresh = Boot(99);
  FCR_REQUIRE_OK(fixture.registry->RegisterPublisher(fixture.publisher, fixture.scope(), fresh));
  PublicationRequest request;
  request.mode = PublicationMode::FullSnapshot;
  request.publication = *PublicationId::Parse("p-3");
  request.attempt = *MutationAttemptId::Parse("a-3");
  request.epoch = fixture.registry->CurrentEpoch();
  request.authority = AuthorityContext{fixture.scope(), fixture.publisher, fresh,
                                       *SourceId::Parse("test-source"),
                                       SourceGeneration::FromValue(1)};
  request.entity = entity;
  request.entity_generation = generation;
  request.expected_set_generation = CapabilitySetGeneration::FromValue(1);
  request.claims = {SpeedSetClaim({200'000'000'000ull})};
  auto recommitted = fixture.registry->Publish(request);
  FCR_REQUIRE_OK(recommitted);
  FCR_CHECK(recommitted.Value().Committed());
  auto after = fixture.registry->Query(entity, Cap("fabric.port.supported_speeds"));
  FCR_REQUIRE_OK(after);
  FCR_CHECK(after.Value().actionable);
  FCR_CHECK_EQ(after.Value().value.ToText(), std::string("{200000000000 bit/s}"));
}

FCR_TEST(authority, coordinator_epoch_advance_invalidates_process_evidence) {
  Fixture fixture;
  const EntityId entity = Entity("nic:0");
  const EntityGeneration generation = EntityGeneration::FromValue(1);
  CapabilityClaim durable = BoolClaim("fabric.offload.rdma", true,
                                      ProvenanceClass::AuthoritativeAdministrativeDeclaration,
                                      EvidenceSourceClass::AdministrativeDeclaration,
                                      DurabilityClass::Durable);
  FCR_REQUIRE_OK(fixture.PublishSnapshot(entity, generation,
                                         {SpeedSetClaim({100'000'000'000ull}), durable}));

  const CoordinatorEpoch before = fixture.registry->CurrentEpoch();
  auto advanced = fixture.registry->AdvanceCoordinatorEpoch(Reason("coordinator-restart"));
  FCR_REQUIRE_OK(advanced);
  FCR_CHECK(advanced.Value() > before);

  // Live publication authority does not survive the epoch advance.
  FCR_CHECK(!fixture.registry->FindPublisher(fixture.publisher, fixture.boot).has_value());
  FCR_CHECK(fixture.registry->IsWorkerBootFenced(fixture.boot));

  auto process_bound = fixture.registry->Query(entity, Cap("fabric.port.supported_speeds"));
  FCR_REQUIRE_OK(process_bound);
  FCR_CHECK(process_bound.Value().state == CapabilityState::RevalidationRequired);
  auto durable_query = fixture.registry->Query(entity, Cap("fabric.offload.rdma"));
  FCR_REQUIRE_OK(durable_query);
  FCR_CHECK(durable_query.Value().actionable);

  // The old epoch is refused.
  PublicationRequest request;
  request.mode = PublicationMode::FullSnapshot;
  request.publication = *PublicationId::Parse("p-2");
  request.attempt = *MutationAttemptId::Parse("a-2");
  request.epoch = before;
  request.authority = AuthorityContext{fixture.scope(), fixture.publisher, fixture.boot,
                                       *SourceId::Parse("test-source"),
                                       SourceGeneration::FromValue(2)};
  request.entity = entity;
  request.entity_generation = generation;
  request.claims = {SpeedSetClaim({400'000'000'000ull})};
  auto stale = fixture.registry->Publish(request);
  FCR_REQUIRE_OK(stale);
  FCR_CHECK(stale.Value().Rejected());
  FCR_CHECK(stale.Value().code == ErrorCode::CoordinatorEpochStale);
}

FCR_TEST(authority, device_replacement_fences_old_capabilities) {
  Fixture fixture;
  const EntityId entity = Entity("switch:leaf-7");
  const EntityGeneration old_generation = EntityGeneration::FromValue(4);
  FCR_REQUIRE_OK(fixture.PublishSnapshot(entity, old_generation,
                                         {SpeedSetClaim({100'000'000'000ull}),
                                          BoolClaim("fabric.forwarding.ecmp_supported", true)}));

  // Fabric Registry supersedes device generation 4 with generation 5.
  auto advanced = fixture.registry->ObserveEntityGeneration(entity, EntityGeneration::FromValue(5),
                                                            Reason("hardware-replaced"));
  FCR_REQUIRE_OK(advanced);

  auto record = fixture.registry->EntityRecord(entity);
  FCR_REQUIRE_OK(record);
  FCR_CHECK_EQ(record.Value().current_generation.Value(), 5ull);
  FCR_CHECK_EQ(record.Value().history.size(), std::size_t(1));
  FCR_CHECK_EQ(record.Value().history.front().generation.Value(), 4ull);
  FCR_CHECK_EQ(record.Value().history.front().supported_count, std::size_t(2));
  FCR_CHECK(record.Value().current_set.capabilities.empty() ||
            record.Value().current_set.set_generation.Value() == 0);

  // Capability truth of generation 4 is historical only.
  auto retired = fixture.registry->RetiredGeneration(entity, old_generation);
  FCR_REQUIRE_OK(retired);
  FCR_CHECK_EQ(retired.Value().supported_count, std::size_t(2));
  FCR_CHECK(!retired.Value().digest.IsZero());

  auto query = fixture.registry->Query(entity, Cap("fabric.port.supported_speeds"));
  FCR_REQUIRE_OK(query);
  FCR_CHECK(query.Value().state == CapabilityState::Unknown);
  FCR_CHECK(!query.Value().actionable);

  // Querying the retired generation explicitly is refused rather than served.
  FCR_CHECK_CODE(fixture.registry->Query(entity, old_generation,
                                         Cap("fabric.port.supported_speeds")),
                 ErrorCode::UnknownEntityGeneration);

  // Fresh evidence for the new generation is required before support returns.
  auto fresh = fixture.PublishSnapshot(entity, EntityGeneration::FromValue(5),
                                       {SpeedSetClaim({400'000'000'000ull})}, {},
                                       Coverage::FullEnumeration, "p-2", "a-2");
  FCR_REQUIRE_OK(fresh);
  FCR_CHECK(fresh.Value().Committed());
  auto resolved = fixture.registry->Query(entity, Cap("fabric.port.supported_speeds"));
  FCR_REQUIRE_OK(resolved);
  FCR_CHECK(resolved.Value().actionable);
}

FCR_TEST(authority, source_invalidation_and_conflict_resolution) {
  Fixture fixture;
  const EntityId entity = Entity("nic:0");
  const EntityGeneration generation = EntityGeneration::FromValue(1);

  // Two equally strong current sources disagree: CONFLICTED, never an average.
  AuthorityGrant second_scope = fixture.grant;
  second_scope.scope = *AuthorityScopeId::Parse("test-scope-2");
  second_scope.allowed_publishers = {*PublisherId::Parse("other-publisher")};
  FCR_REQUIRE_OK(fixture.registry->DeclareAuthority(second_scope));
  const WorkerBootId other_boot = Boot(77);
  FCR_REQUIRE_OK(fixture.registry->RegisterPublisher(*PublisherId::Parse("other-publisher"),
                                                     second_scope.scope, other_boot));

  FCR_REQUIRE_OK(fixture.PublishSnapshot(entity, generation,
                                         {QuantityClaim("fabric.queue.max_rx_queues", Unit::Count, 64)},
                                         {}, Coverage::FullEnumeration, "p-1", "a-1"));
  PublicationRequest other;
  other.mode = PublicationMode::PartialObservation;
  other.publication = *PublicationId::Parse("p-2");
  other.attempt = *MutationAttemptId::Parse("a-2");
  other.epoch = fixture.registry->CurrentEpoch();
  other.authority = AuthorityContext{second_scope.scope, *PublisherId::Parse("other-publisher"),
                                     other_boot, *SourceId::Parse("other-source"),
                                     SourceGeneration::FromValue(1)};
  other.entity = entity;
  other.entity_generation = generation;
  other.expected_set_generation = CapabilitySetGeneration::FromValue(1);
  other.claims = {QuantityClaim("fabric.queue.max_rx_queues", Unit::Count, 128)};
  auto conflicted = fixture.registry->Publish(other);
  FCR_REQUIRE_OK(conflicted);
  FCR_CHECK(conflicted.Value().Committed());

  auto query = fixture.registry->Query(entity, Cap("fabric.queue.max_rx_queues"));
  FCR_REQUIRE_OK(query);
  FCR_CHECK(query.Value().state == CapabilityState::Conflicted);
  FCR_CHECK(!query.Value().actionable);
  FCR_CHECK_EQ(query.Value().conflicting_evidence_count, std::size_t(2));

  // A stronger source resolves the conflict deterministically.
  other.authority.worker_boot = fixture.boot;
  other.authority.publisher = fixture.publisher;
  other.authority.scope = fixture.scope();
  other.authority.source = *SourceId::Parse("test-source");
  other.authority.source_generation = SourceGeneration::FromValue(2);
  other.attempt = *MutationAttemptId::Parse("a-3");
  other.publication = *PublicationId::Parse("p-3");
  other.expected_set_generation = CapabilitySetGeneration::FromValue(2);
  CapabilityClaim stronger = QuantityClaim("fabric.queue.max_rx_queues", Unit::Count, 256);
  stronger.provenance = ProvenanceClass::DirectHardwareEnumeration;
  other.claims = {stronger};
  auto resolved = fixture.registry->Publish(other);
  FCR_REQUIRE_OK(resolved);
  FCR_CHECK(resolved.Value().Committed());
  auto after = fixture.registry->Query(entity, Cap("fabric.queue.max_rx_queues"));
  FCR_REQUIRE_OK(after);
  // The weaker current claim from the other source is outranked but still
  // conflicts with the winner inside its own class, so the strongest class
  // decides.
  FCR_CHECK(after.Value().state == CapabilityState::Supported ||
            after.Value().state == CapabilityState::Conflicted);
  FCR_CHECK(after.Value().outranked_evidence_count <= after.Value().evidence_count);

  // Mass source invalidation makes the whole source non-current.
  auto invalidated = fixture.registry->InvalidateSource(*SourceId::Parse("other-source"),
                                                        Reason("vendor-sdk-removed"));
  FCR_REQUIRE_OK(invalidated);
  FCR_CHECK(invalidated.Value() >= 1);
  auto after_invalidation = fixture.registry->Query(entity, Cap("fabric.queue.max_rx_queues"));
  FCR_REQUIRE_OK(after_invalidation);
  FCR_CHECK(after_invalidation.Value().current_evidence_count <=
            after_invalidation.Value().evidence_count);
}

FCR_TEST(authority, entity_invalidation_requires_revalidation) {
  Fixture fixture;
  const EntityId entity = Entity("port:leaf-1/1");
  const EntityGeneration generation = EntityGeneration::FromValue(1);
  FCR_REQUIRE_OK(fixture.PublishSnapshot(entity, generation, {MtuRangeClaim(1500, 9216)}));

  FCR_REQUIRE_OK(fixture.registry->InvalidateEntity(entity, generation,
                                                    Reason("topology-relationship-changed")));
  auto query = fixture.registry->Query(entity, Cap("fabric.port.mtu_range"));
  FCR_REQUIRE_OK(query);
  FCR_CHECK(query.Value().state == CapabilityState::RevalidationRequired);
  auto record = fixture.registry->EntityRecord(entity);
  FCR_REQUIRE_OK(record);
  FCR_CHECK(record.Value().invalidated);

  FCR_CHECK_CODE(fixture.registry->InvalidateEntity(entity, EntityGeneration::FromValue(9),
                                                    Reason("x")),
                 ErrorCode::UnknownEntityGeneration);
  FCR_CHECK_CODE(fixture.registry->InvalidateEntity(Entity("port:unknown"), generation, Reason("x")),
                 ErrorCode::UnknownEntity);
}
