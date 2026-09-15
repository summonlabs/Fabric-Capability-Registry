// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "fabric/capability/evidence.hpp"
#include "fabric/capability/ids.hpp"
#include "fabric/capability/record.hpp"

namespace fabric::capability {

// ---------------------------------------------------------------------------
// REAL host capability discovery.
//
// Only sources that the host actually exposes are read. A host visible NIC
// exposes a subset of true silicon capability, so absence from one operating
// system API is never reported as physical UNSUPPORTED: it is reported as
// NOT_REPORTED and contributes UNKNOWN capability evidence.
// ---------------------------------------------------------------------------

/// How one fact was established.
enum class DiscoveryFactStatus : std::uint8_t {
  /// The host API reported the fact explicitly.
  Reported = 0,
  /// The host API does not report this fact. Contributes UNKNOWN evidence.
  NotReported,
  /// The source itself is unavailable on this host.
  SourceUnavailable,
};

std::string_view DiscoveryFactStatusName(DiscoveryFactStatus status) noexcept;

struct DiscoveredFact {
  CapabilityId capability;
  CapabilityState state = CapabilityState::Unknown;
  CapabilityValue value;
  bool has_value = false;
  DiscoveryFactStatus status = DiscoveryFactStatus::NotReported;
  std::string note;
};

struct DiscoveredEntity {
  EntityId entity;
  FabricEntityKind kind = FabricEntityKind::Nic;
  std::string operator_label;
  ProvenanceClass provenance = ProvenanceClass::DirectDeviceOrOsApi;
  EvidenceSourceClass source_class = EvidenceSourceClass::OperatingSystemApi;
  Coverage coverage = Coverage::Partial;
  std::vector<DiscoveredFact> facts;
  std::vector<std::string> notes;
};

struct HostDiscoveryReport {
  std::size_t adapter_count = 0;
  std::vector<DiscoveredEntity> entities;
  /// Sources that could not be consulted on this host, e.g. "pnp-properties".
  std::vector<std::string> unavailable_sources;
  std::string platform;
  std::string ToText() const;
};

struct HostDiscoveryOptions {
  bool include_loopback = false;
  bool include_virtual = true;
  bool include_pnp_properties = true;
  std::size_t max_adapters = 64;
  std::size_t max_facts_per_entity = 32;
  std::size_t max_notes_per_entity = 16;
};

/// Enumerates genuinely host visible capability evidence.
///
/// On Windows the sources are the IP helper adapter enumeration, the interface
/// property tables (negotiated link speed, MTU, physical medium) and bounded
/// PnP device properties for network class devices. Unsupported platforms
/// return DiscoveryUnavailable rather than fabricated data.
Outcome<HostDiscoveryReport> DiscoverHostCapabilities(const HostDiscoveryOptions& options = {});

/// Renders the discovery report as deterministic text.
std::string RenderDiscoveryReport(const HostDiscoveryReport& report);

}  // namespace fabric::capability
