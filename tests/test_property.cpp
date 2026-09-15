// Fabric Capability Registry test suite: seeded property and invariant tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <functional>
#include <map>
#include <set>
#include <vector>

#include "fabric/capability/fabric_capability.hpp"
#include "registry_fixture.hpp"
#include "temp_dir.hpp"
#include "test_framework.hpp"

using namespace fabric::capability;
using namespace fcr::test;

namespace {

const std::vector<const char*> kQuantityCapabilities = {
    "fabric.queue.max_rx_queues", "fabric.queue.max_tx_queues",
    "fabric.forwarding.max_ecmp_width", "fabric.forwarding.max_acl_entries"};

CapabilityClaim RandomClaim(DeterministicRng& rng) {
  switch (rng.NextBelow(4)) {
    case 0:
      return SpeedSetClaim({100'000'000'000ull, 200'000'000'000ull, 400'000'000'000ull});
    case 1:
      return QuantityClaim(kQuantityCapabilities[rng.NextBelow(kQuantityCapabilities.size())],
                           Unit::Count, 1 + rng.NextBelow(512));
    case 2:
      return MtuRangeClaim(1500, 1500 + rng.NextBelow(8192));
    default:
      return BoolClaim("fabric.forwarding.ecmp_supported", (rng.Next() & 1u) != 0);
  }
}

/// A publication that declares the same capability twice is rejected by design, so the
/// generator must not build one.
void AppendRandomClaim(std::vector<CapabilityClaim>& claims, DeterministicRng& rng) {
  CapabilityClaim claim = RandomClaim(rng);
  for (const CapabilityClaim& existing : claims) {
    if (existing.capability == claim.capability) return;
  }
  claims.push_back(std::move(claim));
}

}  // namespace

FCR_TEST(property, generations_only_advance_and_never_repeat) {
  const std::uint64_t seed = 0x5eed1234ull;
  DeterministicRng rng(seed);
  Fixture fixture;
  std::map<std::string, std::uint64_t> last_generation;

  for (int round = 0; round < 120; ++round) {
    const std::string name = "nic:p-" + std::to_string(rng.NextBelow(6));
    std::vector<CapabilityClaim> claims;
    const std::size_t count = 1 + rng.NextBelow(4);
    for (std::size_t index = 0; index < count; ++index) AppendRandomClaim(claims, rng);

    const std::uint64_t expected = last_generation.count(name) == 0 ? 0 : last_generation[name];
    auto result = fixture.PublishSnapshot(Entity(name.c_str()), EntityGeneration::FromValue(1),
                                          claims, CapabilitySetGeneration::FromValue(expected),
                                          Coverage::FullEnumeration,
                                          ("p-" + std::to_string(round)).c_str(),
                                          ("a-" + std::to_string(round)).c_str());
    FCR_REQUIRE_OK(result);
    if (result.Value().Committed()) {
      FCR_CHECK_EQ(result.Value().previous_set_generation.Value(), expected);
      FCR_CHECK_EQ(result.Value().new_set_generation.Value(), expected + 1);
      last_generation[name] = result.Value().new_set_generation.Value();
    } else {
      FCR_CHECK(result.Value().Rejected());
    }
  }
  for (const auto& entry : last_generation) {
    auto set = fixture.registry->QueryEntity(Entity(entry.first.c_str()));
    FCR_REQUIRE_OK(set);
    FCR_CHECK_EQ(set.Value().set_generation.Value(), entry.second);
  }
}

FCR_TEST(property, persisted_state_round_trips_for_random_sets) {
  const std::uint64_t seed = 0xC0FFEEull;
  DeterministicRng rng(seed);
  TempDir dir("property-store");
  PersistenceConfig config;
  config.directory = dir.path();
  config.file_stem = "property";

  Fixture fixture;
  for (int index = 0; index < 24; ++index) {
    const std::string name = "nic:r-" + std::to_string(index);
    std::vector<CapabilityClaim> claims;
    const std::size_t count = 1 + rng.NextBelow(4);
    for (std::size_t claim = 0; claim < count; ++claim) AppendRandomClaim(claims, rng);
    auto result = fixture.PublishSnapshot(Entity(name.c_str()), EntityGeneration::FromValue(1),
                                          claims, {}, Coverage::FullEnumeration,
                                          ("p-" + std::to_string(index)).c_str(),
                                          ("a-" + std::to_string(index)).c_str());
    FCR_REQUIRE_OK(result);
  }
  FCR_REQUIRE_OK(fixture.registry->Save(config));

  CapabilityRegistry reloaded;
  FCR_REQUIRE_OK(reloaded.Load(config));
  FCR_CHECK_EQ(reloaded.Entities().size(), std::size_t(24));

  // Durable capability content round trips exactly when the evidence is
  // durable; the fixture publishes process bound evidence, so the reloaded
  // state must be conservative but structurally identical.
  for (const EntityId& entity : reloaded.Entities()) {
    auto set = reloaded.QueryEntity(entity);
    FCR_REQUIRE_OK(set);
    FCR_CHECK_EQ(set.Value().capabilities.size(),
                 fixture.registry->QueryEntity(entity).Value().capabilities.size());
    FCR_CHECK_EQ(set.Value().set_generation.Value(),
                 fixture.registry->QueryEntity(entity).Value().set_generation.Value());
    for (const CapabilityResolution& resolution : set.Value().capabilities) {
      if (resolution.current_evidence_count == 0) {
        FCR_CHECK(resolution.state == CapabilityState::RevalidationRequired);
      }
    }
  }
}

FCR_TEST(property, indexes_match_records_after_random_traffic) {
  DeterministicRng rng(0xBEEFull);
  Fixture fixture;
  for (int round = 0; round < 80; ++round) {
    const std::string name = "switch:p-" + std::to_string(rng.NextBelow(5));
    const std::uint64_t generation = 1 + rng.NextBelow(2);
    std::vector<CapabilityClaim> claims;
    const std::size_t count = 1 + rng.NextBelow(3);
    for (std::size_t index = 0; index < count; ++index) AppendRandomClaim(claims, rng);
    auto attempt = fixture.PublishSnapshot(Entity(name.c_str()), EntityGeneration::FromValue(generation),
                                           claims, CapabilitySetGeneration::FromValue(2),
                                           Coverage::FullEnumeration,
                                           ("p-" + std::to_string(round)).c_str(),
                                           ("a-" + std::to_string(round)).c_str());
    FCR_REQUIRE_OK(attempt);
  }

  // The reverse index agrees with the resolved sets, capability by capability.
  for (const EntityId& entity : fixture.registry->Entities()) {
    auto set = fixture.registry->QueryEntity(entity);
    FCR_REQUIRE_OK(set);
    for (const CapabilityResolution& resolution : set.Value().capabilities) {
      const std::vector<EntityId> supporting =
          fixture.registry->EntitiesSupporting(resolution.capability);
      const bool listed = std::find(supporting.begin(), supporting.end(), entity) != supporting.end();
      FCR_CHECK_EQ(listed, resolution.state == CapabilityState::Supported);
      const std::vector<EntityId> absent =
          fixture.registry->EntitiesNotSupporting(resolution.capability);
      const bool listed_absent = std::find(absent.begin(), absent.end(), entity) != absent.end();
      FCR_CHECK_EQ(listed_absent, resolution.state == CapabilityState::Unsupported);
    }
  }
  const RegistryStatistics stats = fixture.registry->Statistics();
  FCR_CHECK_EQ(stats.capability_records, stats.supported + stats.unsupported + stats.unknown +
                                             stats.revalidation_required + stats.conflicted);
}

FCR_TEST(property, digest_is_stable_under_permutation) {
  // Two registries publishing the same logical content in different orders and
  // from different entity insertion order agree on the canonical digest.
  Fixture left;
  Fixture right;
  const char* entities[] = {"nic:x", "nic:y", "nic:z"};
  for (const char* name : entities) {
    FCR_REQUIRE_OK(left.PublishSnapshot(Entity(name), EntityGeneration::FromValue(1),
                                        {SpeedSetClaim({100'000'000'000ull}),
                                         BoolClaim("fabric.forwarding.ecmp_supported", true)},
                                        {}, Coverage::FullEnumeration, nullptr, nullptr, 1));
  }
  for (auto iterator = std::rbegin(entities); iterator != std::rend(entities); ++iterator) {
    FCR_REQUIRE_OK(right.PublishSnapshot(Entity(*iterator), EntityGeneration::FromValue(1),
                                         {BoolClaim("fabric.forwarding.ecmp_supported", true),
                                          SpeedSetClaim({100'000'000'000ull})},
                                         {}, Coverage::FullEnumeration, nullptr, nullptr, 1));
  }
  const Digest left_digest = left.registry->ComputeDigest();
  const Digest right_digest = right.registry->ComputeDigest();
  if (!(left_digest == right_digest)) {
    for (const EntityId& entity : left.registry->Entities()) {
      const auto lhs = left.registry->QueryEntity(entity);
      const auto rhs = right.registry->QueryEntity(entity);
      if (!lhs.HasValue() || !rhs.HasValue()) continue;
      if (lhs.Value().digest == rhs.Value().digest) continue;
      for (const CapabilityResolution& resolution : lhs.Value().capabilities) {
        const CapabilityResolution* other = rhs.Value().Find(resolution.capability);
        if (other == nullptr) continue;
        FCR_CHECK_EQ(resolution.winning_evidence.Value(), other->winning_evidence.Value());
        FCR_CHECK_EQ(resolution.evidence_count, other->evidence_count);
        FCR_CHECK_EQ(resolution.capability_generation.Value(),
                     other->capability_generation.Value());
        FCR_CHECK_EQ(resolution.evidence_generation.Value(), other->evidence_generation.Value());
      }
    }
  }
  FCR_CHECK_EQ(left_digest.ToString(), right_digest.ToString());
}
