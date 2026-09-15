// Fabric Capability Registry test suite: SYNTHETIC capability backend.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <set>
#include <string>

#include "fabric/capability/fabric_capability.hpp"
#include "registry_fixture.hpp"
#include "test_framework.hpp"

using namespace fabric::capability;
using namespace fcr::test;

FCR_TEST(synthetic, device_classes_and_names) {
  const std::vector<SyntheticDeviceClass> classes = SyntheticDeviceClasses();
  FCR_CHECK_EQ(classes.size(), kSyntheticDeviceClassCount);
  for (const SyntheticDeviceClass device_class : classes) {
    const std::string_view name = SyntheticDeviceClassName(device_class);
    FCR_CHECK(!name.empty());
    auto parsed = ParseSyntheticDeviceClass(name);
    FCR_REQUIRE_OK(parsed);
    FCR_CHECK(parsed.Value() == device_class);
    const EntityId entity = MakeSyntheticEntity(device_class, 3);
    FCR_CHECK(entity.IsSet());
    FCR_CHECK(IsSyntheticEntity(entity));
    FCR_CHECK(entity.CanonicalName().find("syn-") == 0);
  }
  FCR_CHECK_CODE(ParseSyntheticDeviceClass("quantum-switch"), ErrorCode::MalformedValue);
  FCR_CHECK(!IsSyntheticEntity(Entity("nic:real")));
}

FCR_TEST(synthetic, profiles_are_deterministic_and_labelled) {
  for (const SyntheticDeviceClass device_class : SyntheticDeviceClasses()) {
    SyntheticOptions options;
    options.device_count = 3;
    options.seed = 7;
    const std::vector<SyntheticPublication> first = BuildSyntheticFabric(device_class, options);
    const std::vector<SyntheticPublication> second = BuildSyntheticFabric(device_class, options);
    FCR_CHECK_EQ(first.size(), second.size());
    FCR_CHECK(!first.empty());
    for (std::size_t index = 0; index < first.size(); ++index) {
      FCR_CHECK(first[index].entity == second[index].entity);
      FCR_CHECK_EQ(first[index].claims.size(), second[index].claims.size());
      FCR_CHECK(first[index].provenance == ProvenanceClass::SyntheticTestBackend);
      FCR_CHECK(first[index].source_class == EvidenceSourceClass::SyntheticBackend);
      FCR_CHECK(IsSyntheticEntity(first[index].entity));
      for (const CapabilityClaim& claim : first[index].claims) {
        FCR_CHECK(claim.capability.IsSet());
        FCR_CHECK(claim.state == CapabilityState::Supported);
        FCR_CHECK(!claim.value.IsAbsent());
        FCR_CHECK(claim.provenance == ProvenanceClass::SyntheticTestBackend);
      }
    }
  }
}

FCR_TEST(synthetic, profiles_publish_into_a_real_registry) {
  Fixture fixture;
  SyntheticOptions options;
  options.device_count = 4;
  options.include_conflicting_source = true;
  options.include_changing_source = true;
  const std::vector<SyntheticPublication> publications =
      BuildSyntheticFabric(SyntheticDeviceClass::LeafSwitch, options);
  FCR_CHECK(!publications.empty());

  std::size_t committed = 0;
  for (const SyntheticPublication& publication : publications) {
    PublicationRequest request;
    request.mode = PublicationMode::PartialObservation;
    request.publication = *PublicationId::Parse("p-synthetic-" + publication.entity.CanonicalName());
    request.attempt = *MutationAttemptId::Parse("a-synthetic-" + publication.entity.CanonicalName() +
                                               "-" + publication.source.Value());
    request.epoch = fixture.registry->CurrentEpoch();
    request.authority = AuthorityContext{fixture.scope(), fixture.publisher, fixture.boot,
                                         publication.source, SourceGeneration::FromValue(1)};
    request.entity = publication.entity;
    request.entity_generation = publication.entity_generation;
    request.coverage = publication.coverage;
    request.claims = publication.claims;
    auto record = fixture.registry->EntityRecord(publication.entity);
    if (record.HasValue() && record.Value().has_current_set) {
      request.expected_set_generation = record.Value().current_set.set_generation;
    }
    auto result = fixture.registry->Publish(request);
    FCR_REQUIRE_OK(result);
    if (result.Value().Committed()) ++committed;
  }
  FCR_CHECK(committed >= 4);

  // A synthetic switch profile yields real queryable capability truth, still
  // labelled SYNTHETIC.
  const EntityId first = publications.front().entity;
  auto speeds = fixture.registry->Query(first, Cap("fabric.port.supported_speeds"));
  FCR_REQUIRE_OK(speeds);
  FCR_CHECK(speeds.Value().state == CapabilityState::Supported ||
            speeds.Value().state == CapabilityState::Conflicted);
  FCR_CHECK(speeds.Value().provenance == ProvenanceClass::SyntheticTestBackend);
  FCR_CHECK_EQ(speeds.Value().source_class, EvidenceSourceClass::SyntheticBackend);
}

FCR_TEST(synthetic, reduced_profile_claims_fewer_capabilities) {
  SyntheticOptions options;
  options.device_count = 1;
  const std::vector<SyntheticPublication> full =
      BuildSyntheticFabric(SyntheticDeviceClass::SpineSwitch, options);
  const std::vector<SyntheticPublication> reduced =
      BuildSyntheticFabric(SyntheticDeviceClass::ReducedCapabilityDevice, options);
  FCR_REQUIRE(!full.empty());
  FCR_REQUIRE(!reduced.empty());
  FCR_CHECK(reduced.front().claims.size() < full.front().claims.size());
}
