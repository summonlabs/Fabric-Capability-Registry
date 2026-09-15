// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/capability/synthetic.hpp"

#include <algorithm>
#include <array>
#include <string>

#include "fabric/capability/limits.hpp"
#include "fabric/capability/schema.hpp"

namespace fabric::capability {
namespace {

struct DeviceClassEntry {
  SyntheticDeviceClass device_class;
  std::string_view name;
  FabricEntityKind kind;
  std::string_view token;
};

constexpr std::array<DeviceClassEntry, kSyntheticDeviceClassCount> kDeviceClasses = {{
    {SyntheticDeviceClass::LeafSwitch, "leaf-switch", FabricEntityKind::Switch, "leaf"},
    {SyntheticDeviceClass::SpineSwitch, "spine-switch", FabricEntityKind::Switch, "spine"},
    {SyntheticDeviceClass::HostNic, "host-nic", FabricEntityKind::Nic, "nic"},
    {SyntheticDeviceClass::SmartNic, "smartnic", FabricEntityKind::SmartNic, "smartnic"},
    {SyntheticDeviceClass::Dpu, "dpu", FabricEntityKind::Dpu, "dpu"},
    {SyntheticDeviceClass::Router, "router", FabricEntityKind::Router, "router"},
    {SyntheticDeviceClass::OpticalFacingPort, "optical-facing-port", FabricEntityKind::Port,
     "optics"},
    {SyntheticDeviceClass::HighSpeedEthernetPort, "high-speed-ethernet-port",
     FabricEntityKind::Port, "hsp"},
    {SyntheticDeviceClass::RdmaCapableDevice, "rdma-capable-device", FabricEntityKind::Nic,
     "rdma"},
    {SyntheticDeviceClass::TelemetryRichDevice, "telemetry-rich-device", FabricEntityKind::Switch,
     "telemetry"},
    {SyntheticDeviceClass::ReducedCapabilityDevice, "reduced-capability-device",
     FabricEntityKind::Device, "reduced"},
}};

const DeviceClassEntry& EntryFor(SyntheticDeviceClass device_class) {
  for (const DeviceClassEntry& entry : kDeviceClasses) {
    if (entry.device_class == device_class) return entry;
  }
  return kDeviceClasses[0];
}

constexpr std::uint64_t kGigabit = 1'000'000'000ull;
constexpr std::uint64_t kMegabit = 1'000'000ull;

ReasonToken MakeReasonToken(std::string_view text) {
  auto parsed = ReasonToken::Parse(text);
  return parsed.HasValue() ? parsed.Value() : ReasonToken{};
}

CapabilityClaim MakeClaim(const char* capability_id) {
  CapabilityClaim claim;
  auto parsed = CapabilityId::Parse(capability_id);
  if (parsed.HasValue()) claim.capability = parsed.Value();
  claim.state = CapabilityState::Supported;
  claim.provenance = ProvenanceClass::SyntheticTestBackend;
  claim.source_class = EvidenceSourceClass::SyntheticBackend;
  claim.durability = DurabilityClass::Durable;
  claim.coverage = Coverage::FullEnumeration;
  return claim;
}

void AddBool(std::vector<CapabilityClaim>& claims, const char* id, bool value) {
  CapabilityClaim claim = MakeClaim(id);
  claim.value = CapabilityValue::Boolean(value);
  claims.push_back(std::move(claim));
}

void AddQuantity(std::vector<CapabilityClaim>& claims, const char* id, Unit unit,
                 std::uint64_t value) {
  CapabilityClaim claim = MakeClaim(id);
  auto built = CapabilityValue::QuantityValue(value, unit);
  if (!built.HasValue()) return;
  claim.value = built.Value();
  claims.push_back(std::move(claim));
}

void AddNumericSet(std::vector<CapabilityClaim>& claims, const char* id, Unit unit,
                   std::initializer_list<std::uint64_t> values) {
  CapabilityClaim claim = MakeClaim(id);
  auto built = CapabilityValue::NumericSetValue(unit, values);
  if (!built.HasValue()) return;
  claim.value = built.Value();
  claims.push_back(std::move(claim));
}

void AddRange(std::vector<CapabilityClaim>& claims, const char* id, Unit unit, std::uint64_t minimum,
              std::uint64_t maximum) {
  CapabilityClaim claim = MakeClaim(id);
  auto built = CapabilityValue::NumericRangeValue(unit, minimum, maximum);
  if (!built.HasValue()) return;
  claim.value = built.Value();
  claims.push_back(std::move(claim));
}

void AddEnumSet(std::vector<CapabilityClaim>& claims, const char* id, const char* domain,
                std::initializer_list<std::uint32_t> codes) {
  auto parsed_domain = EnumDomainId::Parse(domain);
  if (!parsed_domain.HasValue()) return;
  CapabilityClaim claim = MakeClaim(id);
  auto built = CapabilityValue::EnumerationSetValue(parsed_domain.Value(), codes);
  if (!built.HasValue()) return;
  claim.value = built.Value();
  claims.push_back(std::move(claim));
}

void AddProtocolSet(std::vector<CapabilityClaim>& claims, const char* id,
                    std::initializer_list<ProtocolId> protocols) {
  CapabilityClaim claim = MakeClaim(id);
  auto built = CapabilityValue::ProtocolSetValue(protocols);
  if (!built.HasValue()) return;
  claim.value = built.Value();
  claims.push_back(std::move(claim));
}

void AddVersionRange(std::vector<CapabilityClaim>& claims, const char* id, const char* minimum,
                     const char* maximum) {
  auto min_version = Version::Parse(minimum);
  auto max_version = Version::Parse(maximum);
  if (!min_version.HasValue() || !max_version.HasValue()) return;
  CapabilityClaim claim = MakeClaim(id);
  auto built = CapabilityValue::VersionIntervalValue(min_version.Value(), true,
                                                     max_version.Value(), true);
  if (!built.HasValue()) return;
  claim.value = built.Value();
  claims.push_back(std::move(claim));
}

std::vector<CapabilityClaim> LeafSwitchClaims(std::uint64_t queue_offset) {
  std::vector<CapabilityClaim> claims;
  AddNumericSet(claims, "fabric.port.supported_speeds", Unit::BitsPerSecond,
                {100 * kGigabit, 400 * kGigabit});
  AddNumericSet(claims, "fabric.port.supported_lane_counts", Unit::Count, {4, 8});
  AddNumericSet(claims, "fabric.port.lane_rates", Unit::BitsPerSecond,
                {25 * kGigabit, 50 * kGigabit, 100 * kGigabit});
  AddRange(claims, "fabric.port.mtu_range", Unit::Bytes, 1500, 9216);
  AddQuantity(claims, "fabric.port.max_frame_size", Unit::Bytes, 9216);
  AddEnumSet(claims, "fabric.port.supported_fec_modes", "fabric.port.fec_mode", {1, 2});
  AddEnumSet(claims, "fabric.port.media_types", "fabric.port.media_type", {0, 1, 2});
  AddQuantity(claims, "fabric.port.max_breakout_children", Unit::Count, 4);
  AddProtocolSet(claims, "fabric.protocol.families",
                 {ProtocolId::Ethernet, ProtocolId::Ipv4, ProtocolId::Ipv6, ProtocolId::Vlan,
                  ProtocolId::Vxlan, ProtocolId::Geneve, ProtocolId::Mpls, ProtocolId::Lacp});
  AddBool(claims, "fabric.forwarding.l2_forwarding", true);
  AddBool(claims, "fabric.forwarding.l3_forwarding", true);
  AddBool(claims, "fabric.forwarding.ecmp_supported", true);
  AddBool(claims, "fabric.forwarding.weighted_ecmp_supported", true);
  AddQuantity(claims, "fabric.forwarding.max_ecmp_width", Unit::Count, 64);
  AddBool(claims, "fabric.forwarding.multicast_supported", true);
  AddBool(claims, "fabric.forwarding.acl_tables_supported", true);
  AddQuantity(claims, "fabric.forwarding.max_acl_entries", Unit::Count, 4096);
  AddQuantity(claims, "fabric.forwarding.max_routes_ipv4", Unit::Count, 65536);
  AddQuantity(claims, "fabric.queue.priority_count", Unit::Count, 8);
  AddQuantity(claims, "fabric.queue.traffic_class_count", Unit::Count, 8 + queue_offset);
  AddEnumSet(claims, "fabric.queue.scheduler_classes", "fabric.queue.scheduler_class", {0, 1, 3});
  AddQuantity(claims, "fabric.buffer-exposure.total_packet_buffer", Unit::Bytes, 64ull << 20);
  AddQuantity(claims, "fabric.buffer-exposure.per_port_buffer_ceiling", Unit::Bytes, 8ull << 20);
  AddBool(claims, "fabric.buffer-exposure.shared_buffer_supported", true);
  AddEnumSet(claims, "fabric.buffer-exposure.threshold_mechanisms",
             "fabric.buffer.threshold_mechanism", {1, 2, 5});
  AddEnumSet(claims, "fabric.telemetry.families", "fabric.telemetry.family", {0, 1, 2, 3, 5});
  AddBool(claims, "fabric.congestion.ecn_supported", true);
  AddBool(claims, "fabric.congestion.pfc_supported", true);
  AddBool(claims, "fabric.congestion.pause_supported", true);
  AddBool(claims, "fabric.qos.shaper_supported", true);
  AddBool(claims, "fabric.qos.policer_supported", true);
  AddBool(claims, "fabric.qos.strict_priority_supported", true);
  AddBool(claims, "fabric.qos.weighted_scheduling_supported", true);
  AddEnumSet(claims, "fabric.tunneling.encapsulations", "fabric.tunneling.encapsulation", {0, 1});
  AddQuantity(claims, "fabric.tunneling.max_tunnel_endpoints", Unit::Count, 16000);
  AddBool(claims, "fabric.timestamping.hardware_timestamp_supported", true);
  AddEnumSet(claims, "fabric.optics.transceiver_types", "fabric.optics.transceiver_type", {4, 6});
  AddNumericSet(claims, "fabric.optics.supported_rates", Unit::BitsPerSecond,
                {100 * kGigabit, 400 * kGigabit});
  return claims;
}

std::vector<CapabilityClaim> SpineSwitchClaims(std::uint64_t queue_offset) {
  std::vector<CapabilityClaim> claims;
  AddNumericSet(claims, "fabric.port.supported_speeds", Unit::BitsPerSecond,
                {400 * kGigabit, 800 * kGigabit});
  AddNumericSet(claims, "fabric.port.supported_lane_counts", Unit::Count, {8, 16});
  AddNumericSet(claims, "fabric.port.lane_rates", Unit::BitsPerSecond,
                {50 * kGigabit, 100 * kGigabit});
  AddRange(claims, "fabric.port.mtu_range", Unit::Bytes, 1500, 9216);
  AddEnumSet(claims, "fabric.port.supported_fec_modes", "fabric.port.fec_mode", {1, 3});
  AddBool(claims, "fabric.forwarding.l2_forwarding", true);
  AddBool(claims, "fabric.forwarding.l3_forwarding", true);
  AddBool(claims, "fabric.forwarding.ecmp_supported", true);
  AddBool(claims, "fabric.forwarding.weighted_ecmp_supported", true);
  AddQuantity(claims, "fabric.forwarding.max_ecmp_width", Unit::Count, 256);
  AddQuantity(claims, "fabric.forwarding.max_routes_ipv4", Unit::Count, 1'000'000);
  AddQuantity(claims, "fabric.forwarding.max_routes_ipv6", Unit::Count, 500'000);
  AddBool(claims, "fabric.forwarding.segment_routing_supported", true);
  AddBool(claims, "fabric.forwarding.programmable_forwarding_supported", true);
  AddQuantity(claims, "fabric.queue.traffic_class_count", Unit::Count, 8 + queue_offset);
  AddEnumSet(claims, "fabric.queue.scheduler_classes", "fabric.queue.scheduler_class",
             {0, 1, 3, 4});
  AddQuantity(claims, "fabric.buffer-exposure.total_packet_buffer", Unit::Bytes, 256ull << 20);
  AddQuantity(claims, "fabric.buffer-exposure.per_port_buffer_ceiling", Unit::Bytes, 16ull << 20);
  AddBool(claims, "fabric.buffer-exposure.shared_buffer_supported", true);
  AddEnumSet(claims, "fabric.telemetry.families", "fabric.telemetry.family", {0, 1, 2, 3, 5, 6, 7});
  AddBool(claims, "fabric.congestion.ecn_supported", true);
  AddBool(claims, "fabric.congestion.pfc_supported", true);
  AddBool(claims, "fabric.congestion.adaptive_routing_signaling_supported", true);
  AddBool(claims, "fabric.timestamping.hardware_timestamp_supported", true);
  AddEnumSet(claims, "fabric.optics.transceiver_types", "fabric.optics.transceiver_type", {6, 7});
  AddEnumSet(claims, "fabric.optics.modulation_formats", "fabric.optics.modulation", {0, 1});
  AddNumericSet(claims, "fabric.optics.supported_rates", Unit::BitsPerSecond,
                {400 * kGigabit, 800 * kGigabit});
  return claims;
}

std::vector<CapabilityClaim> HostNicClaims(std::uint64_t queue_offset) {
  std::vector<CapabilityClaim> claims;
  AddNumericSet(claims, "fabric.port.supported_speeds", Unit::BitsPerSecond,
                {kGigabit, 10 * kGigabit, 25 * kGigabit});
  AddRange(claims, "fabric.port.mtu_range", Unit::Bytes, 1500, 9000);
  AddQuantity(claims, "fabric.queue.max_rx_queues", Unit::Count, 16 + queue_offset);
  AddQuantity(claims, "fabric.queue.max_tx_queues", Unit::Count, 16 + queue_offset);
  AddQuantity(claims, "fabric.queue.priority_count", Unit::Count, 8);
  AddBool(claims, "fabric.queue.per_queue_telemetry_supported", true);
  AddBool(claims, "fabric.offload.checksum_rx", true);
  AddBool(claims, "fabric.offload.checksum_tx", true);
  AddBool(claims, "fabric.offload.segmentation_offload", true);
  AddBool(claims, "fabric.offload.receive_side_scaling", true);
  AddQuantity(claims, "fabric.offload.max_rss_queues", Unit::Count, 16);
  AddBool(claims, "fabric.offload.flow_steering", true);
  AddBool(claims, "fabric.offload.rdma", false);
  AddBool(claims, "fabric.virtualization.sriov_supported", true);
  AddQuantity(claims, "fabric.virtualization.max_virtual_functions", Unit::Count, 8);
  AddBool(claims, "fabric.virtualization.virtual_ports_supported", true);
  AddEnumSet(claims, "fabric.telemetry.families", "fabric.telemetry.family", {0, 1});
  AddBool(claims, "fabric.timestamping.software_timestamp_supported", true);
  AddBool(claims, "fabric.timestamping.hardware_timestamp_supported", false);
  AddProtocolSet(claims, "fabric.protocol.families",
                 {ProtocolId::Ethernet, ProtocolId::Ipv4, ProtocolId::Ipv6, ProtocolId::Vlan});
  AddQuantity(claims, "fabric.buffer-exposure.total_packet_buffer", Unit::Bytes, 2ull << 20);
  return claims;
}

std::vector<CapabilityClaim> SmartNicClaims(std::uint64_t queue_offset) {
  std::vector<CapabilityClaim> claims = HostNicClaims(queue_offset);
  AddBool(claims, "fabric.offload.rdma", true);
  AddBool(claims, "fabric.offload.crypto", true);
  AddBool(claims, "fabric.offload.compression", true);
  AddEnumSet(claims, "fabric.offload.programmable_offload_classes",
             "fabric.offload.programmable_class", {0, 1, 2, 3, 5});
  AddEnumSet(claims, "fabric.offload.virtualization_offloads",
             "fabric.offload.virtualization_class", {0, 1, 2, 3, 4, 5});
  AddEnumSet(claims, "fabric.offload.tunnel_offloads", "fabric.offload.tunnel_class", {0, 1, 2});
  AddQuantity(claims, "fabric.virtualization.max_virtual_functions", Unit::Count, 64);
  AddBool(claims, "fabric.virtualization.representors_supported", true);
  AddBool(claims, "fabric.virtualization.partitioning_supported", true);
  AddBool(claims, "fabric.timestamping.hardware_timestamp_supported", true);
  AddEnumSet(claims, "fabric.telemetry.families", "fabric.telemetry.family", {0, 1, 2, 3, 5, 7});
  AddNumericSet(claims, "fabric.port.supported_speeds", Unit::BitsPerSecond,
                {10 * kGigabit, 25 * kGigabit, 100 * kGigabit});
  return claims;
}

std::vector<CapabilityClaim> DpuClaims(std::uint64_t queue_offset) {
  std::vector<CapabilityClaim> claims = SmartNicClaims(queue_offset);
  AddEnumSet(claims, "fabric.rdma.transport_families", "fabric.rdma.transport", {0, 1});
  AddQuantity(claims, "fabric.rdma.max_queue_pairs", Unit::Count, 8192);
  AddBool(claims, "fabric.rdma.atomic_operations_supported", true);
  AddBool(claims, "fabric.rdma.reliable_connection_supported", true);
  AddQuantity(claims, "fabric.rdma.max_message_size", Unit::Bytes, 1ull << 30);
  AddBool(claims, "fabric.offload.accelerator_adjacent_network", true);
  AddEnumSet(claims, "fabric.telemetry.families", "fabric.telemetry.family",
             {0, 1, 2, 3, 4, 5, 6, 7, 10});
  AddEnumSet(claims, "fabric.timestamping.clock_sources", "fabric.timestamping.clock_source",
             {0, 1, 3});
  return claims;
}

std::vector<CapabilityClaim> RouterClaims(std::uint64_t queue_offset) {
  std::vector<CapabilityClaim> claims;
  AddBool(claims, "fabric.forwarding.l2_forwarding", true);
  AddBool(claims, "fabric.forwarding.l3_forwarding", true);
  AddBool(claims, "fabric.forwarding.ecmp_supported", true);
  AddBool(claims, "fabric.forwarding.weighted_ecmp_supported", true);
  AddQuantity(claims, "fabric.forwarding.max_ecmp_width", Unit::Count, 128);
  AddBool(claims, "fabric.forwarding.policy_routing_supported", true);
  AddBool(claims, "fabric.forwarding.multicast_supported", true);
  AddBool(claims, "fabric.forwarding.segment_routing_supported", true);
  AddQuantity(claims, "fabric.forwarding.max_routes_ipv4", Unit::Count, 2'000'000);
  AddQuantity(claims, "fabric.forwarding.max_routes_ipv6", Unit::Count, 1'000'000);
  AddQuantity(claims, "fabric.forwarding.max_host_entries", Unit::Count, 100'000);
  AddQuantity(claims, "fabric.queue.priority_count", Unit::Count, 8 + queue_offset);
  AddQuantity(claims, "fabric.qos.priority_count", Unit::Count, 8);
  AddBool(claims, "fabric.qos.shaper_supported", true);
  AddBool(claims, "fabric.qos.policer_supported", true);
  AddBool(claims, "fabric.qos.marking_supported", true);
  AddEnumSet(claims, "fabric.qos.trust_modes", "fabric.qos.trust_mode", {0, 1, 2});
  AddProtocolSet(claims, "fabric.protocol.families",
                 {ProtocolId::Ethernet, ProtocolId::Ipv4, ProtocolId::Ipv6, ProtocolId::Vlan,
                  ProtocolId::Mpls, ProtocolId::Srv6, ProtocolId::Lacp});
  AddEnumSet(claims, "fabric.tunneling.encapsulations", "fabric.tunneling.encapsulation",
             {0, 1, 4, 5});
  AddEnumSet(claims, "fabric.telemetry.families", "fabric.telemetry.family", {0, 4, 7});
  AddBool(claims, "fabric.congestion.ecn_supported", true);
  AddBool(claims, "fabric.congestion.pfc_supported", true);
  return claims;
}

std::vector<CapabilityClaim> OpticalPortClaims(std::uint64_t queue_offset) {
  std::vector<CapabilityClaim> claims;
  AddEnumSet(claims, "fabric.optics.transceiver_types", "fabric.optics.transceiver_type",
             {1, 2, 6, 7});
  AddNumericSet(claims, "fabric.optics.supported_rates", Unit::BitsPerSecond,
                {100 * kGigabit, 200 * kGigabit, 400 * kGigabit});
  AddNumericSet(claims, "fabric.optics.lane_counts", Unit::Count, {4, 8});
  AddEnumSet(claims, "fabric.optics.modulation_formats", "fabric.optics.modulation", {0, 1});
  AddEnumSet(claims, "fabric.optics.fec_modes", "fabric.optics.fec_mode", {1, 2});
  AddNumericSet(claims, "fabric.optics.wavelengths", Unit::Nanometers, {1310, 1550});
  AddBool(claims, "fabric.optics.diagnostic_monitoring_supported", true);
  AddEnumSet(claims, "fabric.optics.dom_measurements", "fabric.optics.dom_measurement",
             {0, 1, 2, 3, 4});
  AddNumericSet(claims, "fabric.port.supported_speeds", Unit::BitsPerSecond,
                {100 * kGigabit, 400 * kGigabit});
  AddEnumSet(claims, "fabric.port.media_types", "fabric.port.media_type", {1, 3});
  AddQuantity(claims, "fabric.port.max_breakout_children", Unit::Count, 8);
  AddEnumSet(claims, "fabric.telemetry.families", "fabric.telemetry.family", {0, 8, 9});
  AddQuantity(claims, "fabric.queue.priority_count", Unit::Count, 8 + queue_offset);
  return claims;
}

std::vector<CapabilityClaim> HighSpeedPortClaims(std::uint64_t queue_offset) {
  std::vector<CapabilityClaim> claims;
  AddNumericSet(claims, "fabric.port.supported_speeds", Unit::BitsPerSecond,
                {100 * kGigabit, 200 * kGigabit, 400 * kGigabit, 800 * kGigabit});
  AddNumericSet(claims, "fabric.port.supported_lane_counts", Unit::Count, {1, 2, 4, 8});
  AddNumericSet(claims, "fabric.port.lane_rates", Unit::BitsPerSecond,
                {25 * kGigabit, 50 * kGigabit, 100 * kGigabit});
  AddRange(claims, "fabric.port.mtu_range", Unit::Bytes, 1500, 9216);
  AddQuantity(claims, "fabric.port.max_frame_size", Unit::Bytes, 9216);
  AddEnumSet(claims, "fabric.port.supported_fec_modes", "fabric.port.fec_mode", {1, 2, 3, 4});
  AddEnumSet(claims, "fabric.port.media_types", "fabric.port.media_type", {1, 2, 3});
  AddEnumSet(claims, "fabric.port.connector_types", "fabric.optics.connector_type", {0, 2});
  AddQuantity(claims, "fabric.port.max_breakout_children", Unit::Count, 8);
  AddQuantity(claims, "fabric.queue.priority_count", Unit::Count, 8);
  AddQuantity(claims, "fabric.queue.traffic_class_count", Unit::Count, 8 + queue_offset);
  AddBool(claims, "fabric.forwarding.l2_forwarding", true);
  AddBool(claims, "fabric.forwarding.l3_forwarding", true);
  AddBool(claims, "fabric.timestamping.hardware_timestamp_supported", true);
  AddEnumSet(claims, "fabric.telemetry.families", "fabric.telemetry.family", {0, 1, 2, 3, 5});
  return claims;
}

std::vector<CapabilityClaim> RdmaClaims(std::uint64_t queue_offset) {
  std::vector<CapabilityClaim> claims = HostNicClaims(queue_offset);
  AddEnumSet(claims, "fabric.rdma.transport_families", "fabric.rdma.transport", {0, 1, 3});
  AddQuantity(claims, "fabric.rdma.max_queue_pairs", Unit::Count, 4096);
  AddBool(claims, "fabric.rdma.atomic_operations_supported", true);
  AddBool(claims, "fabric.rdma.reliable_connection_supported", true);
  AddBool(claims, "fabric.rdma.memory_window_supported", true);
  AddQuantity(claims, "fabric.rdma.max_message_size", Unit::Bytes, 1ull << 30);
  AddBool(claims, "fabric.offload.rdma", true);
  AddEnumSet(claims, "fabric.telemetry.families", "fabric.telemetry.family", {0, 1, 2});
  return claims;
}

std::vector<CapabilityClaim> TelemetryRichClaims(std::uint64_t queue_offset) {
  std::vector<CapabilityClaim> claims = SpineSwitchClaims(queue_offset);
  AddEnumSet(claims, "fabric.telemetry.families", "fabric.telemetry.family",
             {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
  AddQuantity(claims, "fabric.telemetry.max_flow_export_entries", Unit::Count, 65536);
  AddBool(claims, "fabric.telemetry.histogram_supported", true);
  AddBool(claims, "fabric.timestamping.hardware_timestamp_supported", true);
  AddBool(claims, "fabric.timestamping.rx_timestamp_supported", true);
  AddBool(claims, "fabric.timestamping.tx_timestamp_supported", true);
  AddEnumSet(claims, "fabric.timestamping.clock_sources", "fabric.timestamping.clock_source",
             {0, 1, 3, 4});
  AddEnumSet(claims, "fabric.timestamping.ptp_profiles", "fabric.timestamping.ptp_profile",
             {0, 1});
  return claims;
}

std::vector<CapabilityClaim> ReducedClaims(std::uint64_t queue_offset) {
  std::vector<CapabilityClaim> claims;
  AddNumericSet(claims, "fabric.port.supported_speeds", Unit::BitsPerSecond, {kGigabit});
  AddRange(claims, "fabric.port.mtu_range", Unit::Bytes, 1500, 1500);
  AddQuantity(claims, "fabric.queue.max_rx_queues", Unit::Count, 4 + queue_offset);
  AddBool(claims, "fabric.offload.checksum_rx", true);
  AddEnumSet(claims, "fabric.telemetry.families", "fabric.telemetry.family", {0});
  return claims;
}

std::vector<CapabilityClaim> ClaimsFor(SyntheticDeviceClass device_class,
                                       std::uint64_t queue_offset) {
  switch (device_class) {
    case SyntheticDeviceClass::LeafSwitch:
      return LeafSwitchClaims(queue_offset);
    case SyntheticDeviceClass::SpineSwitch:
      return SpineSwitchClaims(queue_offset);
    case SyntheticDeviceClass::HostNic:
      return HostNicClaims(queue_offset);
    case SyntheticDeviceClass::SmartNic:
      return SmartNicClaims(queue_offset);
    case SyntheticDeviceClass::Dpu:
      return DpuClaims(queue_offset);
    case SyntheticDeviceClass::Router:
      return RouterClaims(queue_offset);
    case SyntheticDeviceClass::OpticalFacingPort:
      return OpticalPortClaims(queue_offset);
    case SyntheticDeviceClass::HighSpeedEthernetPort:
      return HighSpeedPortClaims(queue_offset);
    case SyntheticDeviceClass::RdmaCapableDevice:
      return RdmaClaims(queue_offset);
    case SyntheticDeviceClass::TelemetryRichDevice:
      return TelemetryRichClaims(queue_offset);
    case SyntheticDeviceClass::ReducedCapabilityDevice:
      return ReducedClaims(queue_offset);
  }
  return ReducedClaims(queue_offset);
}

std::string SourceName(const DeviceClassEntry& entry, std::string_view flavour) {
  return "synthetic-source-" + std::string(entry.token) + "-" + std::string(flavour);
}

}  // namespace

std::string_view SyntheticDeviceClassName(SyntheticDeviceClass device_class) noexcept {
  return EntryFor(device_class).name;
}

Outcome<SyntheticDeviceClass> ParseSyntheticDeviceClass(std::string_view text) {
  for (const DeviceClassEntry& entry : kDeviceClasses) {
    if (entry.name == text) return entry.device_class;
  }
  return Outcome<SyntheticDeviceClass>::Failure(ErrorCode::MalformedValue,
                                                "unknown synthetic device class",
                                                std::string(text));
}

std::vector<SyntheticDeviceClass> SyntheticDeviceClasses() {
  std::vector<SyntheticDeviceClass> classes;
  classes.reserve(kDeviceClasses.size());
  for (const DeviceClassEntry& entry : kDeviceClasses) classes.push_back(entry.device_class);
  return classes;
}

EntityId MakeSyntheticEntity(SyntheticDeviceClass device_class, std::size_t index) {
  const DeviceClassEntry& entry = EntryFor(device_class);
  std::string name = "syn-";
  name.append(entry.token);
  name.push_back('-');
  std::string digits = std::to_string(index);
  if (digits.size() < 4) name.append(4 - digits.size(), '0');
  name.append(digits);
  auto created = EntityId::Create(entry.kind, name);
  return created.HasValue() ? created.Value() : EntityId{};
}

bool IsSyntheticEntity(const EntityId& entity) noexcept {
  return entity.IsSet() && entity.CanonicalName().rfind("syn-", 0) == 0;
}

std::vector<SyntheticPublication> BuildSyntheticFabric(SyntheticDeviceClass device_class,
                                                       const SyntheticOptions& options) {
  std::vector<SyntheticPublication> publications;
  const DeviceClassEntry& entry = EntryFor(device_class);
  if (options.device_count == 0) return publications;

  for (std::size_t index = 0; index < options.device_count; ++index) {
    const std::size_t name_index = options.name_offset + index;
    const EntityId entity = MakeSyntheticEntity(device_class, name_index);
    if (!entity.IsSet()) continue;

    std::uint64_t state = options.seed * 6364136223846793005ull + name_index * 1442695040888963407ull;
    const auto next = [&state]() {
      state ^= state << 13;
      state ^= state >> 7;
      state ^= state << 17;
      return state;
    };
    const std::uint64_t queue_offset = next() % 5;

    SyntheticPublication primary;
    primary.entity = entity;
    primary.entity_generation = EntityGeneration::FromValue(options.first_generation);
    auto source = SourceId::Parse(SourceName(entry, "primary"));
    primary.source = source.HasValue() ? source.Value() : SourceId{};
    auto publisher = PublisherId::Parse("synthetic-publisher");
    primary.publisher = publisher.HasValue() ? publisher.Value() : PublisherId{};
    auto scope = AuthorityScopeId::Parse("synthetic-scope");
    primary.scope = scope.HasValue() ? scope.Value() : AuthorityScopeId{};
    primary.provenance = ProvenanceClass::SyntheticTestBackend;
    primary.source_class = EvidenceSourceClass::SyntheticBackend;
    primary.coverage = Coverage::FullEnumeration;
    primary.claims = ClaimsFor(device_class, queue_offset);
    primary.reason = MakeReasonToken("synthetic-profile");
    primary.description = std::string("synthetic ") + std::string(entry.name) + " profile";
    publications.push_back(std::move(primary));

    if (options.include_conflicting_source) {
      SyntheticPublication conflict = publications.back();
      auto conflict_source = SourceId::Parse(SourceName(entry, "conflict"));
      conflict.source = conflict_source.HasValue() ? conflict_source.Value() : SourceId{};
      conflict.coverage = Coverage::Partial;
      conflict.claims.clear();
      for (const CapabilityClaim& claim : publications.back().claims) {
        if (claim.capability.ToString() != "fabric.queue.max_rx_queues" &&
            claim.capability.ToString() != "fabric.forwarding.max_ecmp_width") {
          continue;
        }
        CapabilityClaim altered = claim;
        if (altered.value.Kind() == ValueKind::Quantity) {
          const std::uint64_t bumped = altered.value.AsQuantity().value + 64;
          auto rebuilt = CapabilityValue::QuantityValue(bumped, altered.value.AsQuantity().unit);
          if (rebuilt.HasValue()) altered.value = rebuilt.Value();
        }
        conflict.claims.push_back(std::move(altered));
      }
      if (!conflict.claims.empty()) {
        conflict.reason = MakeReasonToken("synthetic-conflicting-source");
        conflict.description = "synthetic conflicting evidence source";
        publications.push_back(std::move(conflict));
      }
    }

    if (options.include_changing_source) {
      SyntheticPublication changed = publications.front();
      auto changed_source = SourceId::Parse(SourceName(entry, "change"));
      changed.source = changed_source.HasValue() ? changed_source.Value() : SourceId{};
      changed.coverage = Coverage::Partial;
      changed.claims.clear();
      for (const CapabilityClaim& claim : publications.front().claims) {
        if (claim.capability.ToString() != "fabric.port.supported_speeds") continue;
        changed.claims.push_back(claim);
      }
      auto higher = changed_source;
      (void)higher;
      if (!changed.claims.empty()) {
        CapabilityClaim upgraded = changed.claims.front();
        const std::array<std::uint64_t, 3> upgraded_speeds = {kGigabit, 25 * kGigabit,
                                                              100 * kGigabit};
        auto built = CapabilityValue::NumericSetValue(Unit::BitsPerSecond, upgraded_speeds);
        if (built.HasValue()) {
          upgraded.value = built.Value();
        }
        changed.claims.clear();
        changed.claims.push_back(std::move(upgraded));
        changed.reason = MakeReasonToken("synthetic-vendor-update");
        changed.description = "synthetic vendor update that changes a capability value";
        publications.push_back(std::move(changed));
      }
    }
  }
  return publications;
}

}  // namespace fabric::capability
