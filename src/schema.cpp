// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/capability/schema.hpp"

#include <algorithm>
#include <array>
#include <set>

#include "fabric/capability/state.hpp"

namespace fabric::capability {
namespace {

CapabilityId CanonicalId(const char* text) {
  auto parsed = CapabilityId::Parse(text);
  return parsed.HasValue() ? parsed.Value() : CapabilityId{};
}

EnumDomainId CanonicalDomainId(const char* text) {
  auto parsed = EnumDomainId::Parse(text);
  return parsed.HasValue() ? parsed.Value() : EnumDomainId{};
}

CompatibilityClassId CanonicalClassId(const char* text) {
  auto parsed = CompatibilityClassId::Parse(text);
  return parsed.HasValue() ? parsed.Value() : CompatibilityClassId{};
}

void AddDomain(std::vector<EnumDomainDescriptor>& domains, const char* id, const char* description,
               std::initializer_list<EnumCodeDescriptor> codes) {
  EnumDomainDescriptor domain;
  domain.id = CanonicalDomainId(id);
  domain.description = description;
  domain.codes.assign(codes.begin(), codes.end());
  domains.push_back(std::move(domain));
}

std::vector<EnumDomainDescriptor> MakeEnumDomains() {
  std::vector<EnumDomainDescriptor> domains;
  AddDomain(domains, "fabric.port.fec_mode", "forward error correction modes of a port",
            {{0, "none"}, {1, "rs"}, {2, "base-r"}, {3, "rs-544"}, {4, "rs-108"}, {5, "interleaved-rs"}});
  AddDomain(domains, "fabric.port.media_type", "physical media classes of a port",
            {{0, "copper"}, {1, "fiber"}, {2, "dac"}, {3, "aoc"}, {4, "backplane"},
             {5, "virtual"}, {6, "wireless"}});
  AddDomain(domains, "fabric.optics.transceiver_type", "transceiver form factors",
            {{0, "sfp"}, {1, "sfp28"}, {2, "sfp-dd"}, {3, "qsfp"}, {4, "qsfp28"},
             {5, "qsfp56"}, {6, "qsfp-dd"}, {7, "osfp"}});
  AddDomain(domains, "fabric.optics.modulation", "optical modulation formats",
            {{0, "nrz"}, {1, "pam4"}});
  AddDomain(domains, "fabric.optics.connector_type", "physical connector types",
            {{0, "lc"}, {1, "sc"}, {2, "mpo"}, {3, "rj45"}, {4, "dac-plug"}});
  AddDomain(domains, "fabric.optics.fec_mode", "optical forward error correction modes",
            {{0, "none"}, {1, "rs"}, {2, "base-r"}});
  AddDomain(domains, "fabric.optics.dom_measurement", "digital optical monitoring measurements",
            {{0, "temperature"}, {1, "supply-voltage"}, {2, "tx-bias"}, {3, "tx-power"},
             {4, "rx-power"}, {5, "laser-temperature"}});
  AddDomain(domains, "fabric.queue.scheduler_class", "queue scheduler classes",
            {{0, "strict-priority"}, {1, "weighted-round-robin"}, {2, "deficit-round-robin"},
             {3, "weighted-fair-queueing"}, {4, "hierarchical"}});
  AddDomain(domains, "fabric.buffer.threshold_mechanism", "buffer threshold mechanisms",
            {{0, "static"}, {1, "dynamic"}, {2, "alpha-based"}, {3, "per-port-shared"},
             {4, "headroom"}, {5, "wred"}});
  AddDomain(domains, "fabric.telemetry.family", "telemetry families a device can expose",
            {{0, "port-counters"}, {1, "queue-counters"}, {2, "ecn-counters"}, {3, "pfc-counters"},
             {4, "loss-counters"}, {5, "buffer-occupancy"}, {6, "latency"}, {7, "flow-telemetry"},
             {8, "optical-diagnostics"}, {9, "transceiver-metrics"}, {10, "programmable-export"}});
  AddDomain(domains, "fabric.offload.programmable_class", "programmable offload classes",
            {{0, "packet-parsing"}, {1, "match-action"}, {2, "metering"}, {3, "stateful-processing"},
             {4, "inline-tls"}, {5, "load-balancing"}});
  AddDomain(domains, "fabric.offload.virtualization_class", "virtualization offload classes",
            {{0, "virtio"}, {1, "sriov"}, {2, "representor"}, {3, "vxlan-offload"},
             {4, "geneve-offload"}, {5, "connection-tracking"}});
  AddDomain(domains, "fabric.offload.tunnel_class", "tunnel offload classes",
            {{0, "vxlan"}, {1, "geneve"}, {2, "gre"}, {3, "ip-in-ip"}, {4, "mpls-over-udp"}});
  AddDomain(domains, "fabric.rdma.transport", "RDMA transport families",
            {{0, "roce-v1"}, {1, "roce-v2"}, {2, "infiniband"}, {3, "iwarp"}});
  AddDomain(domains, "fabric.congestion.ecn_mode", "explicit congestion notification modes",
            {{0, "ect0"}, {1, "ect1"}, {2, "ce-marking"}, {3, "dctcp"}, {4, "l4s"}});
  AddDomain(domains, "fabric.qos.trust_mode", "QoS trust modes",
            {{0, "untrusted"}, {1, "cos"}, {2, "dscp"}, {3, "port"}});
  AddDomain(domains, "fabric.tunneling.encapsulation", "tunnel encapsulations",
            {{0, "vxlan"}, {1, "geneve"}, {2, "gre"}, {3, "ip-in-ip"}, {4, "mpls"}, {5, "srv6"}});
  AddDomain(domains, "fabric.virtualization.isolation_feature", "tenant isolation features",
            {{0, "vlan-isolation"}, {1, "vrf-isolation"}, {2, "namespace-isolation"},
             {3, "pci-function-isolation"}, {4, "rate-isolation"}});
  AddDomain(domains, "fabric.timestamping.clock_source", "timestamping clock sources",
            {{0, "local-free-running"}, {1, "ptp"}, {2, "ntp"}, {3, "gps"}, {4, "gnss"},
             {5, "atomic"}, {6, "syncE"}});
  AddDomain(domains, "fabric.timestamping.precision_class", "timestamp precision classes",
            {{0, "sub-microsecond"}, {1, "sub-100ns"}, {2, "sub-10ns"}, {3, "sub-ns"}});
  AddDomain(domains, "fabric.timestamping.ptp_profile", "PTP profiles",
            {{0, "default"}, {1, "telecom-g8275-1"}, {2, "power-c37-238"}, {3, "enterprise"},
             {4, "smpte"}});
  AddDomain(domains, "fabric.compatibility.class", "capability compatibility classes",
            {{0, "fabric-os-1"}, {1, "generic-ethernet"}, {2, "rdma-capable"},
             {3, "programmable-datapath"}, {4, "optical-facing"}});
  return domains;
}

/// Small declarative builder so that the canonical table stays readable and
/// every descriptor field is set explicitly rather than by position.
class Builder {
 public:
  Builder(const char* id, ValueKind kind) {
    descriptor_.id = CanonicalId(id);
    descriptor_.kind = kind;
  }

  Builder& Quantity(Unit unit, std::uint64_t maximum = limits::kMaxQuantity) {
    descriptor_.unit = unit;
    descriptor_.minimum_quantity = 0;
    descriptor_.maximum_quantity = maximum;
    return *this;
  }
  Builder& Range(Unit unit, std::uint64_t maximum = limits::kMaxQuantity) {
    descriptor_.unit = unit;
    descriptor_.maximum_quantity = maximum;
    return *this;
  }
  Builder& Set(Unit unit, std::size_t cardinality = 64,
               std::uint64_t maximum = limits::kMaxQuantity) {
    descriptor_.unit = unit;
    descriptor_.max_set_cardinality = cardinality;
    descriptor_.maximum_quantity = maximum;
    return *this;
  }
  Builder& Domain(const char* domain) {
    descriptor_.enum_domain = CanonicalDomainId(domain);
    return *this;
  }
  Builder& EnumSet(const char* domain, std::size_t cardinality = 32) {
    descriptor_.enum_domain = CanonicalDomainId(domain);
    descriptor_.max_set_cardinality = cardinality;
    return *this;
  }
  Builder& Tuple(std::uint8_t arity, ValueKind first, ValueKind second) {
    descriptor_.tuple_arity = arity;
    descriptor_.tuple_component_kinds[0] = first;
    if (arity >= 2) descriptor_.tuple_component_kinds[1] = second;
    return *this;
  }
  Builder& Protocols(std::initializer_list<ProtocolId> protocols) {
    descriptor_.allowed_protocols.assign(protocols.begin(), protocols.end());
    return *this;
  }
  Builder& Bits(std::uint32_t bits) {
    descriptor_.bit_count = bits;
    return *this;
  }
  Builder& Class(const char* text) {
    descriptor_.compatibility_class = CanonicalClassId(text);
    return *this;
  }
  /// Absence of a report is authoritative for this capability. Used only where
  /// a current full enumeration structurally settles the question.
  Builder& AbsenceIsNegative() {
    descriptor_.absence_is_negative = true;
    return *this;
  }
  Builder& Describe(std::string_view text) {
    descriptor_.description.assign(text);
    return *this;
  }

  CapabilityDescriptor Build() { return descriptor_; }

 private:
  CapabilityDescriptor descriptor_;
};

std::vector<CapabilityDescriptor> MakeCanonicalDescriptors() {
  std::vector<CapabilityDescriptor> descriptors;
  const auto add = [&descriptors](CapabilityDescriptor descriptor) {
    descriptors.push_back(std::move(descriptor));
  };

  // --- protocol -----------------------------------------------------------
  add(Builder("fabric.protocol.families", ValueKind::ProtocolSet)
          .Protocols({ProtocolId::Ethernet, ProtocolId::Ipv4, ProtocolId::Ipv6, ProtocolId::Vlan,
                      ProtocolId::Mpls, ProtocolId::Vxlan, ProtocolId::Geneve, ProtocolId::Srv6,
                      ProtocolId::Tcp, ProtocolId::Udp, ProtocolId::RoceV1, ProtocolId::RoceV2,
                      ProtocolId::Iwarp, ProtocolId::InfiniBand, ProtocolId::Ptp,
                      ProtocolId::NvmeOf, ProtocolId::FibreChannel, ProtocolId::Fcoe,
                      ProtocolId::Lacp, ProtocolId::LacpPdu})
          .Class("generic-ethernet")
          .Describe("protocol families the entity can carry")
          .Build());
  add(Builder("fabric.protocol.version_range", ValueKind::VersionInterval)
          .Describe("accepted peer protocol version interval")
          .Build());
  add(Builder("fabric.protocol.vlan_tag_depth", ValueKind::Quantity)
          .Quantity(Unit::Count, 8)
          .Describe("number of VLAN tags the datapath can push or pop")
          .Build());
  add(Builder("fabric.protocol.mpls_label_depth", ValueKind::Quantity)
          .Quantity(Unit::Count, 16)
          .Describe("MPLS label stack depth")
          .Build());
  add(Builder("fabric.protocol.srv6_supported", ValueKind::Boolean)
          .Describe("segment routing over IPv6 support")
          .Build());
  add(Builder("fabric.protocol.lag_supported", ValueKind::Boolean)
          .Describe("link aggregation participant support")
          .Build());

  // --- forwarding ---------------------------------------------------------
  add(Builder("fabric.forwarding.l2_forwarding", ValueKind::Boolean)
          .Class("generic-ethernet")
          .Describe("layer 2 forwarding")
          .Build());
  add(Builder("fabric.forwarding.l3_forwarding", ValueKind::Boolean)
          .Describe("layer 3 forwarding")
          .Build());
  add(Builder("fabric.forwarding.ecmp_supported", ValueKind::Boolean)
          .Describe("equal cost multipath support declaration")
          .Build());
  add(Builder("fabric.forwarding.weighted_ecmp_supported", ValueKind::Boolean)
          .Describe("weighted equal cost multipath support declaration")
          .Build());
  add(Builder("fabric.forwarding.max_ecmp_width", ValueKind::Quantity)
          .Quantity(Unit::Count, 4096)
          .Describe("maximum multipath width")
          .Build());
  add(Builder("fabric.forwarding.multicast_supported", ValueKind::Boolean)
          .Describe("multicast forwarding")
          .Build());
  add(Builder("fabric.forwarding.segment_routing_supported", ValueKind::Boolean)
          .Describe("segment routing forwarding")
          .Build());
  add(Builder("fabric.forwarding.programmable_forwarding_supported", ValueKind::Boolean)
          .Describe("programmable forwarding pipeline")
          .Build());
  add(Builder("fabric.forwarding.acl_tables_supported", ValueKind::Boolean)
          .Describe("ACL or filter table support")
          .Build());
  add(Builder("fabric.forwarding.max_acl_entries", ValueKind::Quantity)
          .Quantity(Unit::Count)
          .Describe("ACL entry scale")
          .Build());
  add(Builder("fabric.forwarding.policy_routing_supported", ValueKind::Boolean)
          .Describe("policy based routing support")
          .Build());
  add(Builder("fabric.forwarding.max_routes_ipv4", ValueKind::Quantity)
          .Quantity(Unit::Count)
          .Describe("IPv4 route scale where authoritative")
          .Build());
  add(Builder("fabric.forwarding.max_routes_ipv6", ValueKind::Quantity)
          .Quantity(Unit::Count)
          .Describe("IPv6 route scale where authoritative")
          .Build());
  add(Builder("fabric.forwarding.max_host_entries", ValueKind::Quantity)
          .Quantity(Unit::Count)
          .Describe("host or neighbor table scale")
          .Build());

  // --- port ---------------------------------------------------------------
  add(Builder("fabric.port.supported_speeds", ValueKind::NumericSet)
          .Set(Unit::BitsPerSecond, 64, 1'000'000'000'000'000ull)
          .Class("generic-ethernet")
          .Describe("discrete link speeds the port may be configured for")
          .Build());
  add(Builder("fabric.port.supported_lane_counts", ValueKind::NumericSet)
          .Set(Unit::Count, 32, 64)
          .Describe("lane counts the port may be broken out into")
          .Build());
  add(Builder("fabric.port.lane_rates", ValueKind::NumericSet)
          .Set(Unit::BitsPerSecond, 32, 1'000'000'000'000ull)
          .Describe("per lane signalling rates")
          .Build());
  add(Builder("fabric.port.lane_configurations", ValueKind::TupleSet)
          .Tuple(2, ValueKind::Quantity, ValueKind::Quantity)
          .Describe("valid (lane count, lane rate) combinations")
          .Build());
  add(Builder("fabric.port.breakout_modes", ValueKind::EnumSet)
          .EnumSet("fabric.port.media_type", 16)
          .Describe("declared breakout modes of a physical port")
          .Build());
  add(Builder("fabric.port.mtu_range", ValueKind::NumericRange)
          .Range(Unit::Bytes, 1'000'000)
          .Class("generic-ethernet")
          .Describe("supported MTU interval")
          .Build());
  add(Builder("fabric.port.max_frame_size", ValueKind::Quantity)
          .Quantity(Unit::Bytes, 1'000'000)
          .Describe("maximum frame size the port accepts")
          .Build());
  add(Builder("fabric.port.supported_fec_modes", ValueKind::EnumSet)
          .EnumSet("fabric.port.fec_mode", 8)
          .Describe("forward error correction modes")
          .Build());
  add(Builder("fabric.port.media_types", ValueKind::EnumSet)
          .EnumSet("fabric.port.media_type", 8)
          .Describe("physical media classes")
          .Build());
  add(Builder("fabric.port.connector_types", ValueKind::EnumSet)
          .EnumSet("fabric.optics.connector_type", 8)
          .Describe("connector types")
          .Build());
  add(Builder("fabric.port.max_breakout_children", ValueKind::Quantity)
          .Quantity(Unit::Count, 64)
          .Describe("maximum number of breakout children")
          .Build());
  add(Builder("fabric.port.pause_frame_support", ValueKind::Boolean)
          .Describe("pause frame support declaration")
          .Build());

  // --- optics -------------------------------------------------------------
  add(Builder("fabric.optics.transceiver_types", ValueKind::EnumSet)
          .EnumSet("fabric.optics.transceiver_type", 16)
          .Class("optical-facing")
          .Describe("supported transceiver form factors")
          .Build());
  add(Builder("fabric.optics.supported_rates", ValueKind::NumericSet)
          .Set(Unit::BitsPerSecond, 64, 100'000'000'000'000ull)
          .Describe("supported optical rates")
          .Build());
  add(Builder("fabric.optics.lane_counts", ValueKind::NumericSet)
          .Set(Unit::Count, 32, 64)
          .Describe("supported optical lane counts")
          .Build());
  add(Builder("fabric.optics.modulation_formats", ValueKind::EnumSet)
          .EnumSet("fabric.optics.modulation", 8)
          .Describe("supported modulation formats")
          .Build());
  add(Builder("fabric.optics.fec_modes", ValueKind::EnumSet)
          .EnumSet("fabric.optics.fec_mode", 8)
          .Describe("supported optical FEC modes")
          .Build());
  add(Builder("fabric.optics.wavelengths", ValueKind::NumericSet)
          .Set(Unit::Nanometers, 256, 2'000)
          .Describe("supported centre wavelengths in nanometres")
          .Build());
  add(Builder("fabric.optics.diagnostic_monitoring_supported", ValueKind::Boolean)
          .Describe("digital optical monitoring support declaration")
          .Build());
  add(Builder("fabric.optics.dom_measurements", ValueKind::EnumSet)
          .EnumSet("fabric.optics.dom_measurement", 16)
          .Describe("available digital optical monitoring measurements")
          .Build());

  // --- queue --------------------------------------------------------------
  add(Builder("fabric.queue.max_rx_queues", ValueKind::Quantity)
          .Quantity(Unit::Count, 1'000'000)
          .Describe("maximum receive queue count")
          .Build());
  add(Builder("fabric.queue.max_tx_queues", ValueKind::Quantity)
          .Quantity(Unit::Count, 1'000'000)
          .Describe("maximum transmit queue count")
          .Build());
  add(Builder("fabric.queue.traffic_class_count", ValueKind::Quantity)
          .Quantity(Unit::Count, 64)
          .Describe("number of traffic classes")
          .Build());
  add(Builder("fabric.queue.priority_count", ValueKind::Quantity)
          .Quantity(Unit::Count, 64)
          .Describe("number of priorities")
          .Build());
  add(Builder("fabric.queue.scheduler_classes", ValueKind::EnumSet)
          .EnumSet("fabric.queue.scheduler_class", 8)
          .Describe("supported queue scheduler classes")
          .Build());
  add(Builder("fabric.queue.per_queue_telemetry_supported", ValueKind::Boolean)
          .Describe("per queue counter availability declaration")
          .Build());
  add(Builder("fabric.queue.isolation_supported", ValueKind::Boolean)
          .Describe("queue isolation support declaration")
          .Build());

  // --- buffer exposure ----------------------------------------------------
  add(Builder("fabric.buffer-exposure.total_packet_buffer", ValueKind::Quantity)
          .Quantity(Unit::Bytes)
          .Describe("total exposed packet buffer memory")
          .Build());
  add(Builder("fabric.buffer-exposure.per_port_buffer_ceiling", ValueKind::Quantity)
          .Quantity(Unit::Bytes)
          .Describe("per port exposed buffer ceiling")
          .Build());
  add(Builder("fabric.buffer-exposure.shared_buffer_supported", ValueKind::Boolean)
          .Describe("shared buffer support declaration")
          .Build());
  add(Builder("fabric.buffer-exposure.threshold_mechanisms", ValueKind::EnumSet)
          .EnumSet("fabric.buffer.threshold_mechanism", 8)
          .Describe("supported buffer threshold mechanisms")
          .Build());
  add(Builder("fabric.buffer-exposure.headroom_supported", ValueKind::Boolean)
          .Describe("headroom support declaration")
          .Build());

  // --- telemetry ----------------------------------------------------------
  add(Builder("fabric.telemetry.families", ValueKind::EnumSet)
          .EnumSet("fabric.telemetry.family", 16)
          .Describe("telemetry families the entity can expose")
          .Build());
  add(Builder("fabric.telemetry.max_flow_export_entries", ValueKind::Quantity)
          .Quantity(Unit::Count)
          .Describe("flow telemetry export scale")
          .Build());
  add(Builder("fabric.telemetry.histogram_supported", ValueKind::Boolean)
          .Describe("histogram telemetry support declaration")
          .Build());

  // --- offload ------------------------------------------------------------
  add(Builder("fabric.offload.checksum_rx", ValueKind::Boolean)
          .Describe("receive checksum offload declaration")
          .Build());
  add(Builder("fabric.offload.checksum_tx", ValueKind::Boolean)
          .Describe("transmit checksum offload declaration")
          .Build());
  add(Builder("fabric.offload.segmentation_offload", ValueKind::Boolean)
          .Describe("TSO or GSO style segmentation offload declaration")
          .Build());
  add(Builder("fabric.offload.receive_side_scaling", ValueKind::Boolean)
          .Describe("receive side scaling declaration")
          .Build());
  add(Builder("fabric.offload.max_rss_queues", ValueKind::Quantity)
          .Quantity(Unit::Count, 1'000'000)
          .Describe("maximum receive side scaling queue count")
          .Build());
  add(Builder("fabric.offload.flow_steering", ValueKind::Boolean)
          .Describe("flow steering declaration")
          .Build());
  add(Builder("fabric.offload.crypto", ValueKind::Boolean)
          .Describe("network local crypto offload declaration")
          .Build());
  add(Builder("fabric.offload.compression", ValueKind::Boolean)
          .Describe("network local compression offload declaration")
          .Build());
  add(Builder("fabric.offload.rdma", ValueKind::Boolean)
          .Describe("RDMA offload declaration")
          .Build());
  add(Builder("fabric.offload.accelerator_adjacent_network", ValueKind::Boolean)
          .Describe("accelerator adjacent network path declaration")
          .Build());
  add(Builder("fabric.offload.programmable_offload_classes", ValueKind::EnumSet)
          .EnumSet("fabric.offload.programmable_class", 16)
          .Class("programmable-datapath")
          .Describe("programmable offload classes")
          .Build());
  add(Builder("fabric.offload.virtualization_offloads", ValueKind::EnumSet)
          .EnumSet("fabric.offload.virtualization_class", 16)
          .Describe("virtualization offload classes")
          .Build());
  add(Builder("fabric.offload.tunnel_offloads", ValueKind::EnumSet)
          .EnumSet("fabric.offload.tunnel_class", 16)
          .Describe("tunnel offload classes")
          .Build());

  // --- RDMA ---------------------------------------------------------------
  add(Builder("fabric.rdma.transport_families", ValueKind::EnumSet)
          .EnumSet("fabric.rdma.transport", 8)
          .Class("rdma-capable")
          .Describe("RDMA transport families the entity declares")
          .Build());
  add(Builder("fabric.rdma.max_queue_pairs", ValueKind::Quantity)
          .Quantity(Unit::Count, 10'000'000)
          .Describe("maximum queue pairs")
          .Build());
  add(Builder("fabric.rdma.atomic_operations_supported", ValueKind::Boolean)
          .Describe("RDMA atomic operations declaration")
          .Build());
  add(Builder("fabric.rdma.reliable_connection_supported", ValueKind::Boolean)
          .Describe("reliable connection transport declaration")
          .Build());
  add(Builder("fabric.rdma.memory_window_supported", ValueKind::Boolean)
          .Describe("memory window declaration")
          .Build());
  add(Builder("fabric.rdma.max_message_size", ValueKind::Quantity)
          .Quantity(Unit::Bytes, 1u << 30)
          .Describe("maximum RDMA message size")
          .Build());

  // --- congestion and flow control ----------------------------------------
  add(Builder("fabric.congestion.ecn_supported", ValueKind::Boolean)
          .Describe("explicit congestion notification support declaration")
          .Build());
  add(Builder("fabric.congestion.pfc_supported", ValueKind::Boolean)
          .Describe("priority flow control support declaration")
          .Build());
  add(Builder("fabric.congestion.pause_supported", ValueKind::Boolean)
          .Describe("link level pause support declaration")
          .Build());
  add(Builder("fabric.congestion.credit_flow_control_supported", ValueKind::Boolean)
          .Describe("credit based flow control support declaration")
          .Build());
  add(Builder("fabric.congestion.pacing_supported", ValueKind::Boolean)
          .Describe("transmit pacing support declaration")
          .Build());
  add(Builder("fabric.congestion.congestion_marking_supported", ValueKind::Boolean)
          .Describe("congestion marking support declaration")
          .Build());
  add(Builder("fabric.congestion.congestion_telemetry_supported", ValueKind::Boolean)
          .Describe("congestion telemetry support declaration")
          .Build());
  add(Builder("fabric.congestion.adaptive_routing_signaling_supported", ValueKind::Boolean)
          .Describe("adaptive routing signalling support declaration")
          .Build());
  add(Builder("fabric.congestion.ecn_marking_modes", ValueKind::EnumSet)
          .EnumSet("fabric.congestion.ecn_mode", 8)
          .Describe("supported congestion notification marking modes")
          .Build());

  // --- QoS ----------------------------------------------------------------
  add(Builder("fabric.qos.priority_count", ValueKind::Quantity)
          .Quantity(Unit::Count, 64)
          .Describe("number of QoS priorities")
          .Build());
  add(Builder("fabric.qos.traffic_class_count", ValueKind::Quantity)
          .Quantity(Unit::Count, 64)
          .Describe("number of QoS traffic classes")
          .Build());
  add(Builder("fabric.qos.shaper_supported", ValueKind::Boolean)
          .Describe("traffic shaping support declaration")
          .Build());
  add(Builder("fabric.qos.policer_supported", ValueKind::Boolean)
          .Describe("traffic policing support declaration")
          .Build());
  add(Builder("fabric.qos.marking_supported", ValueKind::Boolean)
          .Describe("packet marking support declaration")
          .Build());
  add(Builder("fabric.qos.trust_modes", ValueKind::EnumSet)
          .EnumSet("fabric.qos.trust_mode", 8)
          .Describe("supported QoS trust modes")
          .Build());
  add(Builder("fabric.qos.strict_priority_supported", ValueKind::Boolean)
          .Describe("strict priority scheduling declaration")
          .Build());
  add(Builder("fabric.qos.weighted_scheduling_supported", ValueKind::Boolean)
          .Describe("weighted scheduling declaration")
          .Build());

  // --- tunneling ----------------------------------------------------------
  add(Builder("fabric.tunneling.encapsulations", ValueKind::EnumSet)
          .EnumSet("fabric.tunneling.encapsulation", 16)
          .Describe("supported tunnel encapsulations")
          .Build());
  add(Builder("fabric.tunneling.max_tunnel_endpoints", ValueKind::Quantity)
          .Quantity(Unit::Count, 10'000'000)
          .Describe("tunnel endpoint scale")
          .Build());
  add(Builder("fabric.tunneling.decap_supported", ValueKind::Boolean)
          .Describe("tunnel decapsulation declaration")
          .Build());
  add(Builder("fabric.tunneling.udp_based_supported", ValueKind::Boolean)
          .Describe("UDP based encapsulation declaration")
          .Build());

  // --- virtualization -----------------------------------------------------
  add(Builder("fabric.virtualization.sriov_supported", ValueKind::Boolean)
          .AbsenceIsNegative()
          .Describe("SR-IOV support declaration; a complete PCI enumeration settles absence")
          .Build());
  add(Builder("fabric.virtualization.max_virtual_functions", ValueKind::Quantity)
          .Quantity(Unit::Count, 4096)
          .Describe("maximum virtual functions")
          .Build());
  add(Builder("fabric.virtualization.max_physical_functions", ValueKind::Quantity)
          .Quantity(Unit::Count, 64)
          .Describe("maximum physical functions")
          .Build());
  add(Builder("fabric.virtualization.virtual_ports_supported", ValueKind::Boolean)
          .Describe("virtual port declaration")
          .Build());
  add(Builder("fabric.virtualization.representors_supported", ValueKind::Boolean)
          .Describe("port representor declaration")
          .Build());
  add(Builder("fabric.virtualization.partitioning_supported", ValueKind::Boolean)
          .Describe("device partitioning declaration")
          .Build());
  add(Builder("fabric.virtualization.tenant_isolation_features", ValueKind::EnumSet)
          .EnumSet("fabric.virtualization.isolation_feature", 16)
          .Describe("tenant isolation features")
          .Build());

  // --- timestamping -------------------------------------------------------
  add(Builder("fabric.timestamping.hardware_timestamp_supported", ValueKind::Boolean)
          .AbsenceIsNegative()
          .Describe("hardware timestamping declaration; a complete device capability query settles absence")
          .Build());
  add(Builder("fabric.timestamping.software_timestamp_supported", ValueKind::Boolean)
          .Describe("software timestamping declaration")
          .Build());
  add(Builder("fabric.timestamping.rx_timestamp_supported", ValueKind::Boolean)
          .Describe("receive timestamp declaration")
          .Build());
  add(Builder("fabric.timestamping.tx_timestamp_supported", ValueKind::Boolean)
          .Describe("transmit timestamp declaration")
          .Build());
  add(Builder("fabric.timestamping.clock_sources", ValueKind::EnumSet)
          .EnumSet("fabric.timestamping.clock_source", 16)
          .Describe("supported clock sources")
          .Build());
  add(Builder("fabric.timestamping.precision_class", ValueKind::Enumeration)
          .Domain("fabric.timestamping.precision_class")
          .Describe("timestamp precision class")
          .Build());
  add(Builder("fabric.timestamping.ptp_profiles", ValueKind::EnumSet)
          .EnumSet("fabric.timestamping.ptp_profile", 8)
          .Describe("supported PTP profiles")
          .Build());

  // --- device management and compatibility --------------------------------
  add(Builder("fabric.device-management.supported_api_versions", ValueKind::VersionInterval)
          .Describe("device management API version interval")
          .Build());
  add(Builder("fabric.device-management.config_rollback_supported", ValueKind::Boolean)
          .Describe("configuration rollback declaration")
          .Build());
  add(Builder("fabric.device-management.secure_boot_supported", ValueKind::Boolean)
          .Describe("secure boot declaration")
          .Build());
  add(Builder("fabric.compatibility.class", ValueKind::Enumeration)
          .Domain("fabric.compatibility.class")
          .Describe("declared capability compatibility class")
          .Build());
  add(Builder("fabric.compatibility.api_version_range", ValueKind::VersionInterval)
          .Describe("capability API version interval the entity is compatible with")
          .Build());
  add(Builder("fabric.compatibility.requires_firmware_at_least", ValueKind::VersionInterval)
          .Describe("firmware version interval the capability truth was established under")
          .Build());

  std::sort(descriptors.begin(), descriptors.end(),
            [](const CapabilityDescriptor& lhs, const CapabilityDescriptor& rhs) {
              return lhs.id < rhs.id;
            });
  return descriptors;
}

Outcome<void> ValidateDescriptor(const CapabilityDescriptor& descriptor) {
  if (!descriptor.id.IsSet()) {
    return Status::Failure(ErrorCode::MalformedIdentifier, "capability descriptor has no identifier");
  }
  if (descriptor.minimum_quantity > descriptor.maximum_quantity) {
    return Status::Failure(ErrorCode::ValueContradiction,
                           "capability descriptor quantity bounds are inverted",
                           descriptor.id.ToString());
  }
  if (descriptor.maximum_quantity > limits::kMaxQuantity) {
    return Status::Failure(ErrorCode::ValueOutOfRange,
                           "capability descriptor maximum quantity exceeds the allowed bound",
                           descriptor.id.ToString());
  }
  if (descriptor.max_set_cardinality == 0 ||
      descriptor.max_set_cardinality > limits::kMaxSetCardinality) {
    return Status::Failure(ErrorCode::ValueOutOfRange,
                           "capability descriptor set cardinality is out of range",
                           descriptor.id.ToString());
  }
  switch (descriptor.kind) {
    case ValueKind::Quantity:
    case ValueKind::NumericSet:
    case ValueKind::NumericRange:
      if (!IsQuantityUnit(descriptor.unit)) {
        return Status::Failure(ErrorCode::ValueTypeMismatch,
                               "numeric capability descriptor requires a quantity unit",
                               descriptor.id.ToString());
      }
      break;
    case ValueKind::Enumeration:
    case ValueKind::EnumSet:
      if (!descriptor.enum_domain.IsSet()) {
        return Status::Failure(ErrorCode::ValueTypeMismatch,
                               "enumeration capability descriptor requires an enum domain",
                               descriptor.id.ToString());
      }
      if (!FindEnumDomain(descriptor.enum_domain).HasValue()) {
        return Status::Failure(ErrorCode::UnknownEnumDomain,
                               "capability descriptor declares an unregistered enum domain",
                               descriptor.enum_domain.Value());
      }
      break;
    case ValueKind::BitSet:
      if (descriptor.bit_count == 0 || descriptor.bit_count > limits::kMaxBitSetBits) {
        return Status::Failure(ErrorCode::ValueOutOfRange,
                               "bitset capability descriptor bit count is out of range",
                               descriptor.id.ToString());
      }
      break;
    case ValueKind::Tuple:
    case ValueKind::TupleSet:
      if (descriptor.tuple_arity < 2 || descriptor.tuple_arity > limits::kMaxTupleArity) {
        return Status::Failure(ErrorCode::ValueOutOfRange,
                               "tuple capability descriptor arity is out of range",
                               descriptor.id.ToString());
      }
      break;
    case ValueKind::Opaque:
      if (!descriptor.vendor_extension) {
        return Status::Failure(ErrorCode::SchemaViolation,
                               "opaque payload capabilities must be vendor extensions",
                               descriptor.id.ToString());
      }
      break;
    default:
      break;
  }
  if (descriptor.vendor_extension && !descriptor.id.IsVendorExtension()) {
    return Status::Failure(ErrorCode::SchemaViolation,
                           "a vendor extension descriptor must live in a vendor namespace",
                           descriptor.id.ToString());
  }
  if (!descriptor.vendor_extension && descriptor.id.IsVendorExtension()) {
    return Status::Failure(ErrorCode::SchemaViolation,
                           "a descriptor in a vendor namespace must be marked as an extension",
                           descriptor.id.ToString());
  }
  return Status::Success();
}

void ValidateTupleComponents(const CapabilityDescriptor& descriptor, const TupleValue& tuple,
                             std::string& failure) {
  if (tuple.elements.size() != descriptor.tuple_arity) {
    failure = "tuple arity " + std::to_string(tuple.elements.size()) + " does not match descriptor " +
              std::to_string(descriptor.tuple_arity);
    return;
  }
  for (std::size_t index = 0; index < tuple.elements.size(); ++index) {
    const ValueKind expected = descriptor.tuple_component_kinds[index];
    if (expected == ValueKind::Absent) continue;
    const ScalarValue& element = tuple.elements[index];
    const bool matches =
        (expected == ValueKind::Boolean && element.GetKind() == ScalarValue::Kind::Boolean) ||
        (expected == ValueKind::Integer && element.GetKind() == ScalarValue::Kind::Signed) ||
        (expected == ValueKind::Quantity && element.GetKind() == ScalarValue::Kind::Unsigned) ||
        (expected == ValueKind::Enumeration && element.GetKind() == ScalarValue::Kind::Enumeration);
    if (!matches) {
      failure = "tuple component " + std::to_string(index) + " has the wrong typed form";
      return;
    }
    if (expected == ValueKind::Quantity && element.AsUnsigned() > descriptor.maximum_quantity) {
      failure = "tuple component " + std::to_string(index) + " exceeds the descriptor bound";
      return;
    }
  }
}

}  // namespace

const std::vector<EnumDomainDescriptor>& BuiltinEnumDomains() {
  static const std::vector<EnumDomainDescriptor> domains = MakeEnumDomains();
  return domains;
}

Outcome<const EnumDomainDescriptor*> FindEnumDomain(const EnumDomainId& domain) {
  for (const EnumDomainDescriptor& entry : BuiltinEnumDomains()) {
    if (entry.id == domain) return &entry;
  }
  return Outcome<const EnumDomainDescriptor*>::Failure(ErrorCode::UnknownEnumDomain,
                                                       "enumeration domain is not declared",
                                                       domain.Value());
}

bool EnumDomainDeclares(const EnumDomainId& domain, std::uint32_t code) {
  auto found = FindEnumDomain(domain);
  if (!found) return false;
  for (const EnumCodeDescriptor& entry : found.Value()->codes) {
    if (entry.code == code) return true;
  }
  return false;
}

std::string EnumCodeText(const EnumDomainId& domain, std::uint32_t code) {
  auto found = FindEnumDomain(domain);
  if (found) {
    for (const EnumCodeDescriptor& entry : found.Value()->codes) {
      if (entry.code == code) return std::string(entry.name);
    }
  }
  return domain.Value() + "#" + std::to_string(code);
}

Outcome<void> ValidateValueAgainstDescriptor(const CapabilityDescriptor& descriptor,
                                             const CapabilityValue& value) {
  auto canonical = value.ValidateCanonical();
  if (!canonical) return canonical.GetError();
  if (value.Kind() != descriptor.kind) {
    return Status::Failure(
        ErrorCode::ValueTypeMismatch,
        "capability value typed form does not match the descriptor",
        descriptor.id.ToString() + " expects " + std::string(ValueKindName(descriptor.kind)) +
            " but received " + std::string(ValueKindName(value.Kind())));
  }
  const auto describe = [&descriptor](std::string_view detail) {
    return descriptor.id.ToString() + ": " + std::string(detail);
  };
  switch (descriptor.kind) {
    case ValueKind::Boolean:
      return Status::Success();
    case ValueKind::Integer: {
      const std::int64_t raw = value.AsInteger();
      if (raw >= 0 && static_cast<std::uint64_t>(raw) > descriptor.maximum_quantity) {
        return Status::Failure(ErrorCode::ValueOutOfRange, "integer exceeds the descriptor bound",
                               describe(std::to_string(raw)));
      }
      return Status::Success();
    }
    case ValueKind::Quantity: {
      const Quantity& quantity = value.AsQuantity();
      if (quantity.unit != descriptor.unit) {
        return Status::Failure(ErrorCode::ValueTypeMismatch,
                               "quantity unit does not match the descriptor",
                               describe(std::string(UnitName(quantity.unit)) + " != " +
                                        std::string(UnitName(descriptor.unit))));
      }
      if (quantity.value < descriptor.minimum_quantity ||
          quantity.value > descriptor.maximum_quantity) {
        return Status::Failure(ErrorCode::ValueOutOfRange, "quantity is outside the descriptor bounds",
                               describe(std::to_string(quantity.value)));
      }
      return Status::Success();
    }
    case ValueKind::Enumeration: {
      const EnumValue& enumeration = value.AsEnumeration();
      if (enumeration.domain != descriptor.enum_domain) {
        return Status::Failure(ErrorCode::ValueTypeMismatch,
                               "enumeration domain does not match the descriptor",
                               describe(enumeration.domain.Value()));
      }
      if (!EnumDomainDeclares(enumeration.domain, enumeration.code)) {
        return Status::Failure(ErrorCode::UnknownEnumDomain,
                               "enumeration code is not declared by the domain",
                               describe(std::to_string(enumeration.code)));
      }
      return Status::Success();
    }
    case ValueKind::BitSet: {
      if (value.AsBitSet().bit_count != descriptor.bit_count) {
        return Status::Failure(ErrorCode::ValueOutOfRange, "bitset width does not match the descriptor",
                               describe(std::to_string(value.AsBitSet().bit_count)));
      }
      return Status::Success();
    }
    case ValueKind::NumericSet: {
      const NumericSet& set = value.AsNumericSet();
      if (set.unit != descriptor.unit) {
        return Status::Failure(ErrorCode::ValueTypeMismatch,
                               "numeric set unit does not match the descriptor",
                               describe(std::string(UnitName(set.unit))));
      }
      if (set.values.size() > descriptor.max_set_cardinality) {
        return Status::Failure(ErrorCode::TooManyItems,
                               "numeric set exceeds the descriptor cardinality",
                               describe(std::to_string(set.values.size())));
      }
      for (const std::uint64_t element : set.values) {
        if (element < descriptor.minimum_quantity || element > descriptor.maximum_quantity) {
          return Status::Failure(ErrorCode::ValueOutOfRange,
                                 "numeric set element is outside the descriptor bounds",
                                 describe(std::to_string(element)));
        }
      }
      return Status::Success();
    }
    case ValueKind::NumericRange: {
      const NumericRange& range = value.AsNumericRange();
      if (range.unit != descriptor.unit) {
        return Status::Failure(ErrorCode::ValueTypeMismatch,
                               "numeric range unit does not match the descriptor",
                               describe(std::string(UnitName(range.unit))));
      }
      if (range.maximum > descriptor.maximum_quantity ||
          range.minimum < descriptor.minimum_quantity) {
        return Status::Failure(ErrorCode::ValueOutOfRange,
                               "numeric range is outside the descriptor bounds",
                               describe(value.ToText()));
      }
      return Status::Success();
    }
    case ValueKind::ProtocolSet: {
      const ProtocolSet& set = value.AsProtocolSet();
      if (!descriptor.allowed_protocols.empty()) {
        for (const ProtocolId protocol : set.protocols) {
          if (std::find(descriptor.allowed_protocols.begin(), descriptor.allowed_protocols.end(),
                        protocol) == descriptor.allowed_protocols.end()) {
            return Status::Failure(ErrorCode::SchemaViolation,
                                   "protocol is not declared by the descriptor",
                                   describe(std::string(ProtocolName(protocol))));
          }
        }
      }
      return Status::Success();
    }
    case ValueKind::EnumSet: {
      const EnumSet& set = value.AsEnumSet();
      if (set.domain != descriptor.enum_domain) {
        return Status::Failure(ErrorCode::ValueTypeMismatch,
                               "enumeration set domain does not match the descriptor",
                               describe(set.domain.Value()));
      }
      if (set.codes.size() > descriptor.max_set_cardinality) {
        return Status::Failure(ErrorCode::TooManyItems,
                               "enumeration set exceeds the descriptor cardinality",
                               describe(std::to_string(set.codes.size())));
      }
      for (const std::uint32_t code : set.codes) {
        if (!EnumDomainDeclares(set.domain, code)) {
          return Status::Failure(ErrorCode::UnknownEnumDomain,
                                 "enumeration code is not declared by the domain",
                                 describe(std::to_string(code)));
        }
      }
      return Status::Success();
    }
    case ValueKind::Tuple:
    case ValueKind::TupleSet: {
      if (descriptor.kind == ValueKind::Tuple) {
        std::string failure;
        ValidateTupleComponents(descriptor, value.AsTuple(), failure);
        if (!failure.empty()) {
          return Status::Failure(ErrorCode::ValueTypeMismatch, "tuple does not match the descriptor",
                                 describe(failure));
        }
        return Status::Success();
      }
      for (const TupleValue& tuple : value.AsTupleSet().tuples) {
        std::string failure;
        ValidateTupleComponents(descriptor, tuple, failure);
        if (!failure.empty()) {
          return Status::Failure(ErrorCode::ValueTypeMismatch,
                                 "tuple set member does not match the descriptor",
                                 describe(failure));
        }
      }
      if (value.AsTupleSet().tuples.size() > descriptor.max_set_cardinality) {
        return Status::Failure(ErrorCode::TooManyItems,
                               "tuple set exceeds the descriptor cardinality",
                               describe(std::to_string(value.AsTupleSet().tuples.size())));
      }
      return Status::Success();
    }
    case ValueKind::VersionInterval:
    case ValueKind::Record:
      return Status::Success();
    case ValueKind::Opaque: {
      const OpaquePayload& payload = value.AsOpaque();
      if (payload.encoding != descriptor.id.LocalName() &&
          payload.bytes.size() > limits::kMaxExtensionPayloadBytes) {
        return Status::Failure(ErrorCode::PayloadTooLarge,
                               "opaque payload exceeds the extension bound",
                               describe(std::to_string(payload.bytes.size())));
      }
      return Status::Success();
    }
    case ValueKind::Absent:
      return Status::Failure(ErrorCode::ValueTypeMismatch,
                             "an absent value cannot satisfy a typed descriptor",
                             describe("absent"));
  }
  return Status::Failure(ErrorCode::UnsupportedValueKind,
                         "descriptor uses an unsupported value kind",
                         descriptor.id.ToString());
}

CapabilitySchema::CapabilitySchema() {
  for (CapabilityDescriptor& descriptor : MakeCanonicalDescriptors()) {
    descriptors_.emplace(descriptor.id, std::move(descriptor));
  }
}

Outcome<void> CapabilitySchema::RegisterVendorDescriptor(CapabilityDescriptor descriptor) {
  auto valid = ValidateDescriptor(descriptor);
  if (!valid) return valid.GetError();
  if (!descriptor.vendor_extension) {
    return Status::Failure(ErrorCode::SchemaViolation,
                           "only vendor extension descriptors may be registered at run time",
                           descriptor.id.ToString());
  }
  std::unique_lock<std::shared_mutex> lock(mutex_);
  const auto existing = descriptors_.find(descriptor.id);
  if (existing != descriptors_.end()) {
    if (existing->second == descriptor) {
      return Status::Success();
    }
    return Status::Failure(ErrorCode::DuplicateClaim,
                           "a different descriptor is already registered for this capability",
                           descriptor.id.ToString());
  }
  if (vendor_count_ >= limits::kMaxVendorDescriptors) {
    return Status::Failure(ErrorCode::TooManyItems,
                           "vendor descriptor limit reached",
                           std::to_string(limits::kMaxVendorDescriptors));
  }
  ++vendor_count_;
  descriptors_.emplace(descriptor.id, std::move(descriptor));
  return Status::Success();
}

Outcome<const CapabilityDescriptor*> CapabilitySchema::Find(const CapabilityId& id) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  const auto found = descriptors_.find(id);
  if (found == descriptors_.end()) {
    return Outcome<const CapabilityDescriptor*>::Failure(
        ErrorCode::UnknownCapability,
        "capability is not declared by the governed schema", id.ToString());
  }
  return &found->second;
}

bool CapabilitySchema::Contains(const CapabilityId& id) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return descriptors_.find(id) != descriptors_.end();
}

std::vector<CapabilityDescriptor> CapabilitySchema::Descriptors() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  std::vector<CapabilityDescriptor> out;
  out.reserve(descriptors_.size());
  for (const auto& entry : descriptors_) out.push_back(entry.second);
  std::sort(out.begin(), out.end(),
            [](const CapabilityDescriptor& lhs, const CapabilityDescriptor& rhs) {
              return lhs.id < rhs.id;
            });
  return out;
}

std::vector<CapabilityNamespaceId> CapabilitySchema::Namespaces() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  std::set<CapabilityNamespaceId> unique;
  for (const auto& entry : descriptors_) unique.insert(entry.second.id.Namespace());
  return std::vector<CapabilityNamespaceId>(unique.begin(), unique.end());
}

std::size_t CapabilitySchema::Size() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return descriptors_.size();
}

const CapabilitySchema& CanonicalSchema() {
  static const CapabilitySchema schema;
  return schema;
}

const std::vector<CapabilityNamespaceId>& CanonicalNamespaces() {
  static const std::vector<CapabilityNamespaceId> namespaces = []() {
    std::set<CapabilityNamespaceId> unique;
    for (const CapabilityDescriptor& descriptor : CanonicalSchema().Descriptors()) {
      unique.insert(descriptor.id.Namespace());
    }
    return std::vector<CapabilityNamespaceId>(unique.begin(), unique.end());
  }();
  return namespaces;
}

}  // namespace fabric::capability
