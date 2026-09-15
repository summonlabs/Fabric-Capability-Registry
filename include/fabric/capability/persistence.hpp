// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "fabric/capability/digest.hpp"
#include "fabric/capability/error.hpp"
#include "fabric/capability/ids.hpp"

namespace fabric::capability {

/// Persistence location. The store is always a single file directly inside a
/// configured directory; the file stem is validated so that a caller supplied
/// name can never traverse out of the directory or address a device.
struct PersistenceConfig {
  std::filesystem::path directory;
  std::string file_stem = "fabric-capability-registry";
  /// Create an empty store when the file does not exist.
  bool create_if_missing = true;
  /// Flush file and directory metadata before the atomic replacement.
  bool durable_flush = true;
};

/// Validates a persistence configuration: absolute or relative directory,
/// bounded path length, a file stem without separators, traversal segments,
/// reserved device names or trailing dots.
Outcome<void> ValidatePersistenceConfig(const PersistenceConfig& config);

/// Absolute path of the store file described by a validated configuration.
Outcome<std::filesystem::path> StorePath(const PersistenceConfig& config);

struct SaveReport {
  std::filesystem::path path;
  std::size_t bytes = 0;
  std::size_t entity_records = 0;
  std::size_t evidence_records = 0;
  std::size_t retired_generations = 0;
  std::size_t fence_records = 0;
  std::size_t authority_grants = 0;
  Digest payload_digest;
  Digest file_digest;
};

/// Header level inspection of a store file, produced without materialising a
/// registry. Used by the CLI and by corruption diagnostics.
struct StoreInspection {
  std::filesystem::path path;
  std::uint32_t format_version = 0;
  std::uint32_t flags = 0;
  std::uint64_t payload_bytes = 0;
  std::size_t entity_records = 0;
  std::size_t evidence_records = 0;
  std::size_t retired_generations = 0;
  std::size_t fence_records = 0;
  std::size_t authority_grants = 0;
  RegistryGeneration registry_generation;
  CoordinatorEpoch epoch;
  Digest payload_digest;
  bool integrity_ok = false;
};

/// Store file layout documentation (also rendered by the CLI):
///
///   offset  size  field
///   0       8     magic "FCRSTORE"
///   8       4     format version (little endian u32)
///   12      4     flags (little endian u32)
///   16      8     payload length (little endian u64)
///   24      8     payload record count (little endian u64)
///   32      32    SHA-256 payload digest
///   64      4     header CRC-32 over bytes [0,64)
///   68      ...   payload
///
/// The payload is a deterministic sequence of length prefixed records with a
/// trailing record count; every decode step is bounds checked and every
/// duplicate entity, duplicate capability and duplicate evidence record is
/// rejected.
inline constexpr std::string_view kStoreMagic = "FCRSTORE";
inline constexpr std::size_t kStoreHeaderBytes = 68;

}  // namespace fabric::capability
