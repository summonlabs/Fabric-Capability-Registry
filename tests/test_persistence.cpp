// Fabric Capability Registry test suite: persistence, recovery, corruption.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdio>
#include <fstream>
#include <vector>

#include "fabric/capability/fabric_capability.hpp"
#include "registry_fixture.hpp"
#include "temp_dir.hpp"
#include "test_framework.hpp"

using namespace fabric::capability;
using namespace fcr::test;

namespace {

PersistenceConfig ConfigFor(const TempDir& dir, const char* stem = "registry") {
  PersistenceConfig config;
  config.directory = dir.path();
  config.file_stem = stem;
  return config;
}

std::vector<std::byte> ReadFileBytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  std::vector<std::byte> bytes;
  char buffer[4096];
  while (stream.read(buffer, sizeof(buffer)) || stream.gcount() > 0) {
    const std::streamsize count = stream.gcount();
    for (std::streamsize index = 0; index < count; ++index) {
      bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(buffer[index])));
    }
    if (stream.eof()) break;
  }
  return bytes;
}

void WriteFileBytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

FCR_TEST(persistence, round_trip_preserves_durable_state) {
  TempDir dir("store");
  const PersistenceConfig config = ConfigFor(dir);
  Fixture fixture;
  const EntityId entity = Entity("nic:persist-0");
  const EntityGeneration generation = EntityGeneration::FromValue(1);
  CapabilityClaim admin = BoolClaim("fabric.offload.rdma", true,
                                    ProvenanceClass::AuthoritativeAdministrativeDeclaration,
                                    EvidenceSourceClass::AdministrativeDeclaration,
                                    DurabilityClass::Durable);
  FCR_REQUIRE_OK(fixture.PublishSnapshot(entity, generation,
                                         {SpeedSetClaim({100'000'000'000ull}), admin},
                                         {}, Coverage::FullEnumeration, "p-1", "a-1"));

  auto saved = fixture.registry->Save(config);
  FCR_REQUIRE_OK(saved);
  FCR_CHECK(saved.Value().entity_records == std::size_t(1));
  FCR_CHECK(saved.Value().evidence_records == std::size_t(2));
  FCR_CHECK(saved.Value().bytes > kStoreHeaderBytes);
  FCR_CHECK(std::filesystem::exists(saved.Value().path));

  auto inspection = InspectStore(config);
  FCR_REQUIRE_OK(inspection);
  FCR_CHECK(inspection.Value().integrity_ok);
  FCR_CHECK_EQ(inspection.Value().format_version, kFormatVersion);
  FCR_CHECK_EQ(inspection.Value().entity_records, std::size_t(1));

  CapabilityRegistry loaded;
  auto report = loaded.Load(config);
  FCR_REQUIRE_OK(report);
  FCR_CHECK_EQ(report.Value().entities_loaded, std::size_t(1));
  FCR_CHECK_EQ(report.Value().evidence_loaded, std::size_t(2));
  FCR_CHECK_EQ(report.Value().durable_evidence_retained, std::size_t(1));
  FCR_CHECK_EQ(report.Value().evidence_revalidation_required, std::size_t(1));
  FCR_CHECK_EQ(report.Value().epoch.Value(), 1ull);

  // Durable administrative knowledge survives; live process evidence does not.
  auto durable = loaded.Query(entity, Cap("fabric.offload.rdma"));
  FCR_REQUIRE_OK(durable);
  FCR_CHECK(durable.Value().actionable);
  auto process_bound = loaded.Query(entity, Cap("fabric.port.supported_speeds"));
  FCR_REQUIRE_OK(process_bound);
  FCR_CHECK(process_bound.Value().state == CapabilityState::RevalidationRequired);
  FCR_CHECK(!process_bound.Value().actionable);

  // The stored capability set generation and entity generation survive.
  auto record = loaded.EntityRecord(entity);
  FCR_REQUIRE_OK(record);
  FCR_CHECK_EQ(record.Value().current_generation.Value(), 1ull);
  FCR_CHECK_EQ(record.Value().current_set.set_generation.Value(), 1ull);
}

FCR_TEST(persistence, schema_and_counts_round_trip) {
  TempDir dir("store-counts");
  const PersistenceConfig config = ConfigFor(dir, "counts");
  Fixture fixture;
  for (int index = 0; index < 8; ++index) {
    const std::string name = "nic:round-" + std::to_string(index);
    FCR_REQUIRE_OK(fixture.PublishSnapshot(Entity(name.c_str()), EntityGeneration::FromValue(2),
                                           {SpeedSetClaim({100'000'000'000ull}),
                                            QuantityClaim("fabric.queue.max_rx_queues", Unit::Count,
                                                          static_cast<std::uint64_t>(index + 1))},
                                           {}, Coverage::FullEnumeration,
                                           ("p-" + std::to_string(index)).c_str(),
                                           ("a-" + std::to_string(index)).c_str()));
  }
  FCR_REQUIRE_OK(fixture.registry->Save(config));

  CapabilityRegistry loaded;
  FCR_REQUIRE_OK(loaded.Load(config));
  FCR_CHECK_EQ(loaded.Entities().size(), std::size_t(8));
  const RegistryStatistics stats = loaded.Statistics();
  FCR_CHECK_EQ(stats.entities, std::size_t(8));
  FCR_CHECK_EQ(stats.unsupported, std::size_t(0));
  for (const EntityId& entity : loaded.Entities()) {
    auto set = loaded.QueryEntity(entity);
    FCR_REQUIRE_OK(set);
    FCR_CHECK_EQ(set.Value().capabilities.size(), std::size_t(2));
    FCR_CHECK_EQ(set.Value().entity_generation.Value(), 2ull);
  }
}

FCR_TEST(persistence, recovery_is_conservative_across_epoch_advance) {
  TempDir dir("store-recovery");
  const PersistenceConfig config = ConfigFor(dir, "recovery");
  Fixture fixture;
  const EntityId entity = Entity("nic:recover-0");
  CapabilityClaim admin = BoolClaim("fabric.offload.rdma", true,
                                    ProvenanceClass::AuthoritativeAdministrativeDeclaration,
                                    EvidenceSourceClass::AdministrativeDeclaration,
                                    DurabilityClass::Durable);
  FCR_REQUIRE_OK(fixture.PublishSnapshot(entity, EntityGeneration::FromValue(1),
                                         {SpeedSetClaim({100'000'000'000ull}), admin}));
  FCR_REQUIRE_OK(fixture.registry->Save(config));

  CapabilityRegistry restarted;
  FCR_REQUIRE_OK(restarted.Load(config));
  auto advanced = restarted.AdvanceCoordinatorEpoch(Reason("coordinator-restart"));
  FCR_REQUIRE_OK(advanced);

  auto durable = restarted.Query(entity, Cap("fabric.offload.rdma"));
  FCR_REQUIRE_OK(durable);
  FCR_CHECK(durable.Value().actionable);
  auto process_bound = restarted.Query(entity, Cap("fabric.port.supported_speeds"));
  FCR_REQUIRE_OK(process_bound);
  FCR_CHECK(process_bound.Value().state == CapabilityState::RevalidationRequired);

  // A fresh store keeps the conservative classification too.
  FCR_REQUIRE_OK(restarted.Save(config));
  CapabilityRegistry twice;
  FCR_REQUIRE_OK(twice.Load(config));
  auto again = twice.Query(entity, Cap("fabric.port.supported_speeds"));
  FCR_REQUIRE_OK(again);
  FCR_CHECK(again.Value().state == CapabilityState::RevalidationRequired);
  auto durable_again = twice.Query(entity, Cap("fabric.offload.rdma"));
  FCR_REQUIRE_OK(durable_again);
  FCR_CHECK(durable_again.Value().actionable);
}

FCR_TEST(persistence, path_configuration_is_validated) {
  PersistenceConfig config;
  FCR_CHECK_CODE(ValidatePersistenceConfig(config), ErrorCode::PersistencePathInvalid);
  config.directory = std::filesystem::temp_directory_path();
  config.file_stem = "..";
  FCR_CHECK_CODE(ValidatePersistenceConfig(config), ErrorCode::PersistencePathInvalid);
  config.file_stem = "sub/dir";
  FCR_CHECK_CODE(ValidatePersistenceConfig(config), ErrorCode::PersistencePathInvalid);
  config.file_stem = "CON";
  FCR_CHECK_CODE(ValidatePersistenceConfig(config), ErrorCode::PersistencePathInvalid);
  config.file_stem = "trailing.";
  FCR_CHECK_CODE(ValidatePersistenceConfig(config), ErrorCode::PersistencePathInvalid);
  config.file_stem = std::string(80, 'a');
  FCR_CHECK_CODE(ValidatePersistenceConfig(config), ErrorCode::PersistencePathInvalid);
  config.file_stem = "valid-stem";
  FCR_REQUIRE_OK(ValidatePersistenceConfig(config));
  auto path = StorePath(config);
  FCR_REQUIRE_OK(path);
  FCR_CHECK(path.Value().string().find("valid-stem.fcrstore") != std::string::npos);
}

FCR_TEST(persistence, corrupt_stores_are_rejected) {
  TempDir dir("store-corrupt");
  const PersistenceConfig config = ConfigFor(dir, "corrupt");
  Fixture fixture;
  FCR_REQUIRE_OK(fixture.PublishSnapshot(Entity("nic:corrupt-0"), EntityGeneration::FromValue(1),
                                         {SpeedSetClaim({100'000'000'000ull})}));
  auto saved = fixture.registry->Save(config);
  FCR_REQUIRE_OK(saved);
  const std::vector<std::byte> original = ReadFileBytes(saved.Value().path);
  FCR_CHECK(original.size() > kStoreHeaderBytes);

  // Bad magic.
  {
    std::vector<std::byte> bytes = original;
    bytes[0] = std::byte{'X'};
    WriteFileBytes(saved.Value().path, bytes);
    CapabilityRegistry registry;
    FCR_CHECK_CODE(registry.Load(config), ErrorCode::PersistenceFormatInvalid);
    FCR_CHECK(!InspectStore(config).HasValue());
  }

  // Corrupted payload without a header update: the integrity digest fails.
  {
    std::vector<std::byte> bytes = original;
    bytes[bytes.size() - 1] = static_cast<std::byte>(
        static_cast<unsigned char>(bytes[bytes.size() - 1]) ^ 0x5Au);
    WriteFileBytes(saved.Value().path, bytes);
    CapabilityRegistry registry;
    FCR_CHECK_CODE(registry.Load(config), ErrorCode::PersistenceIntegrityFailure);
  }

  // Corrupted header CRC.
  {
    std::vector<std::byte> bytes = original;
    bytes[12] = static_cast<std::byte>(static_cast<unsigned char>(bytes[12]) ^ 0xFFu);
    WriteFileBytes(saved.Value().path, bytes);
    CapabilityRegistry registry;
    FCR_CHECK_CODE(registry.Load(config), ErrorCode::PersistenceIntegrityFailure);
  }

  // Unsupported format version (header CRC recomputed so the version check is
  // the one that fires).
  {
    std::vector<std::byte> bytes = original;
    bytes[8] = std::byte{99};
    const std::uint32_t crc = Crc32(std::span<const std::byte>(bytes.data(), 64));
    for (int index = 0; index < 4; ++index) {
      bytes[64 + static_cast<std::size_t>(index)] =
          static_cast<std::byte>((crc >> (8 * index)) & 0xFFu);
    }
    WriteFileBytes(saved.Value().path, bytes);
    CapabilityRegistry registry;
    FCR_CHECK_CODE(registry.Load(config), ErrorCode::PersistenceVersionUnsupported);
  }

  // Truncation at many byte positions must be rejected without crashing.
  for (std::size_t length = 0; length < original.size(); length += 7) {
    std::vector<std::byte> bytes(original.begin(),
                                 original.begin() + static_cast<std::ptrdiff_t>(length));
    WriteFileBytes(saved.Value().path, bytes);
    CapabilityRegistry registry;
    auto loaded = registry.Load(config);
    FCR_CHECK(!loaded.HasValue());
    FCR_CHECK(loaded.Code() != ErrorCode::Ok);
  }

  // The intact store still loads.
  WriteFileBytes(saved.Value().path, original);
  CapabilityRegistry registry;
  FCR_REQUIRE_OK(registry.Load(config));
  FCR_CHECK_EQ(registry.Entities().size(), std::size_t(1));
}
