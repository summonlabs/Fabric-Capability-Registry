// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "fabric/capability/ids.hpp"
#include "fabric/capability/record.hpp"

namespace fabric::capability {

// ---------------------------------------------------------------------------
// SYNTHETIC capability backend.
//
// Synthetic profiles model devices this environment cannot physically
// interrogate: leaf and spine switches, SmartNICs, DPUs, routers, optical
// facing ports and high speed Ethernet ports. Every fact produced here is
// labelled SYNTHETIC and is never physical proof.
// ---------------------------------------------------------------------------

enum class SyntheticDeviceClass : std::uint8_t {
  LeafSwitch = 0,
  SpineSwitch,
  HostNic,
  SmartNic,
  Dpu,
  Router,
  OpticalFacingPort,
  HighSpeedEthernetPort,
  RdmaCapableDevice,
  TelemetryRichDevice,
  ReducedCapabilityDevice,
};

inline constexpr std::size_t kSyntheticDeviceClassCount = 11;

std::string_view SyntheticDeviceClassName(SyntheticDeviceClass device_class) noexcept;
Outcome<SyntheticDeviceClass> ParseSyntheticDeviceClass(std::string_view text);
std::vector<SyntheticDeviceClass> SyntheticDeviceClasses();

struct SyntheticOptions {
  /// Number of devices of the class to model.
  std::size_t device_count = 1;
  /// Deterministic seed. The same seed always produces the same model.
  std::uint64_t seed = 1;
  /// First entity generation to model.
  std::uint64_t first_generation = 1;
  /// Emit a second, weaker evidence source that disagrees with the first for
  /// a bounded subset of capabilities.
  bool include_conflicting_source = false;
  /// Emit a source whose claims are later withdrawn, modelling change.
  bool include_changing_source = false;
  /// Index used to derive entity names, so several batches can coexist.
  std::size_t name_offset = 0;
};

/// One synthetic publication intent.
struct SyntheticPublication {
  EntityId entity;
  EntityGeneration entity_generation;
  SourceId source;
  PublisherId publisher;
  AuthorityScopeId scope;
  ProvenanceClass provenance = ProvenanceClass::SyntheticTestBackend;
  EvidenceSourceClass source_class = EvidenceSourceClass::SyntheticBackend;
  Coverage coverage = Coverage::Partial;
  std::vector<CapabilityClaim> claims;
  ReasonToken reason;
  std::string description;
};

/// Deterministic synthetic fabric model for one device class.
std::vector<SyntheticPublication> BuildSyntheticFabric(SyntheticDeviceClass device_class,
                                                       const SyntheticOptions& options = {});

/// True when the identifier is a synthetic test entity name.
bool IsSyntheticEntity(const EntityId& entity) noexcept;

/// Deterministic entity identifier for a synthetic device.
EntityId MakeSyntheticEntity(SyntheticDeviceClass device_class, std::size_t index);

}  // namespace fabric::capability
