// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/capability/discovery.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include "fabric/capability/limits.hpp"
#include "fabric/capability/schema.hpp"
#include "fabric/capability/version.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <setupapi.h>
#include <devguid.h>
#include <windows.h>
#endif

namespace fabric::capability {
namespace {

constexpr std::size_t kMaxAdapterDescription = 96;
constexpr std::size_t kMaxNoteLength = 160;
constexpr std::size_t kMaxHardwareIds = 4;

/// The only protocol family this build maps from a host interface type.
constexpr ProtocolId kEthernetOnlyProtocol = ProtocolId::Ethernet;

struct FactBuilder {
  std::vector<DiscoveredFact>* facts = nullptr;
  std::size_t limit = 0;

  void Reported(const char* capability_id, CapabilityValue value, std::string note) {
    if (facts->size() >= limit) return;
    DiscoveredFact fact;
    auto parsed = CapabilityId::Parse(capability_id);
    if (!parsed.HasValue()) return;
    fact.capability = parsed.Value();
    fact.state = CapabilityState::Supported;
    fact.value = std::move(value);
    fact.has_value = true;
    fact.status = DiscoveryFactStatus::Reported;
    fact.note = std::move(note);
    facts->push_back(std::move(fact));
  }

  void NotReported(const char* capability_id, std::string note) {
    if (facts->size() >= limit) return;
    DiscoveredFact fact;
    auto parsed = CapabilityId::Parse(capability_id);
    if (!parsed.HasValue()) return;
    fact.capability = parsed.Value();
    fact.state = CapabilityState::Unknown;
    fact.has_value = false;
    fact.status = DiscoveryFactStatus::NotReported;
    fact.note = std::move(note);
    facts->push_back(std::move(fact));
  }
};

void AddNote(DiscoveredEntity& entity, std::string note) {
  if (entity.notes.size() >= limits::kMaxTextFieldsPerExplanation) return;
  if (note.empty() || note.size() > kMaxNoteLength) return;
  entity.notes.push_back(std::move(note));
}

std::string Sanitize(std::string_view text, std::size_t max_length) {
  std::string out;
  out.reserve(std::min(text.size(), max_length));
  for (const char ch : text) {
    if (out.size() >= max_length) break;
    const unsigned char raw = static_cast<unsigned char>(ch);
    if (raw < 0x20u || raw > 0x7Eu) {
      out.push_back('?');
      continue;
    }
    if (ch == ' ' || ch == '=' || ch == '\t') {
      out.push_back('-');
      continue;
    }
    out.push_back(ch);
  }
  return out;
}

#if defined(_WIN32)

std::string Narrow(const wchar_t* text, std::size_t max_length) {
  if (text == nullptr) return std::string();
  const int length = static_cast<int>(std::wcslen(text));
  if (length <= 0) return std::string();
  const int size = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
  if (size <= 0) return std::string();
  std::string narrow(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, length, narrow.data(), size, nullptr, nullptr);
  if (narrow.size() > max_length) narrow.resize(max_length);
  return narrow;
}

std::string LowerGuid(const char* guid) {
  std::string out;
  for (const char* cursor = guid; cursor != nullptr && *cursor != '\0'; ++cursor) {
    const char ch = *cursor;
    if (ch == '{' || ch == '}') continue;
    if (ch >= 'A' && ch <= 'F') {
      out.push_back(static_cast<char>(ch - 'A' + 'a'));
      continue;
    }
    if ((ch >= 'a' && ch <= 'f') || (ch >= '0' && ch <= '9') || ch == '-') {
      out.push_back(ch);
      continue;
    }
    out.push_back('-');
  }
  return out;
}

/// Maps an NDIS physical medium to a canonical media type code. Only the
/// unambiguous cases are mapped; everything else is left unclaimed.
bool MapPhysicalMedium(NDIS_PHYSICAL_MEDIUM medium, std::uint32_t* code) {
  switch (medium) {
    case NdisPhysicalMedium802_3:
      *code = 0;  // copper
      return true;
    case NdisPhysicalMediumNative802_11:
    case NdisPhysicalMediumWirelessLan:
    case NdisPhysicalMediumWirelessWan:
    case NdisPhysicalMediumBluetooth:
    case NdisPhysicalMediumUWB:
    case NdisPhysicalMediumWiMax:
      *code = 6;  // wireless
      return true;
    default:
      return false;
  }
}

std::string PhysicalMediumName(NDIS_PHYSICAL_MEDIUM medium) {
  switch (medium) {
    case NdisPhysicalMediumUnspecified:
      return "unspecified";
    case NdisPhysicalMediumWirelessLan:
      return "wireless-lan";
    case NdisPhysicalMediumCableModem:
      return "cable-modem";
    case NdisPhysicalMediumPhoneLine:
      return "phone-line";
    case NdisPhysicalMediumPowerLine:
      return "power-line";
    case NdisPhysicalMediumDSL:
      return "dsl";
    case NdisPhysicalMediumFibreChannel:
      return "fibre-channel";
    case NdisPhysicalMedium1394:
      return "ieee1394";
    case NdisPhysicalMediumWirelessWan:
      return "wireless-wan";
    case NdisPhysicalMediumNative802_11:
      return "native-802-11";
    case NdisPhysicalMediumBluetooth:
      return "bluetooth";
    case NdisPhysicalMediumInfiniband:
      return "infiniband";
    case NdisPhysicalMediumWiMax:
      return "wimax";
    case NdisPhysicalMediumUWB:
      return "uwb";
    case NdisPhysicalMedium802_3:
      return "802-3";
    default:
      return "other";
  }
}

/// Reads bounded PnP properties for network class devices and indexes them by
/// the network configuration instance identifier so that a device can be
/// matched to the adapter it implements.
struct PnpEntry {
  std::string instance_id;
  std::string description;
  std::string manufacturer;
  std::vector<std::string> hardware_ids;
};

std::vector<PnpEntry> EnumeratePnpNetworkDevices(std::size_t max_devices) {
  std::vector<PnpEntry> entries;
  HDEVINFO devices = SetupDiGetClassDevsW(&GUID_DEVCLASS_NET, nullptr, nullptr, DIGCF_PRESENT);
  if (devices == INVALID_HANDLE_VALUE) return entries;

  SP_DEVINFO_DATA info{};
  info.cbSize = sizeof(info);
  for (DWORD index = 0; entries.size() < max_devices; ++index) {
    if (SetupDiEnumDeviceInfo(devices, index, &info) == 0) break;
    PnpEntry entry;

    HKEY key = SetupDiOpenDevRegKey(devices, &info, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
    if (key != INVALID_HANDLE_VALUE) {
      wchar_t buffer[128] = {};
      DWORD size = sizeof(buffer) - sizeof(wchar_t);
      DWORD type = 0;
      if (RegQueryValueExW(key, L"NetCfgInstanceId", nullptr, &type,
                           reinterpret_cast<LPBYTE>(buffer), &size) == ERROR_SUCCESS &&
          type == REG_SZ) {
        entry.instance_id = Narrow(buffer, 64);
      }
      RegCloseKey(key);
    }

    const auto property = [&devices, &info](DWORD property_id, std::size_t max_length) {
      wchar_t buffer[512] = {};
      DWORD required = 0;
      DWORD type = 0;
      if (SetupDiGetDeviceRegistryPropertyW(devices, &info, property_id, &type,
                                            reinterpret_cast<PBYTE>(buffer), sizeof(buffer),
                                            &required) == 0) {
        return std::string();
      }
      std::string text = Narrow(buffer, max_length);
      return Sanitize(text, max_length);
    };

    entry.description = property(SPDRP_DEVICEDESC, kMaxAdapterDescription);
    entry.manufacturer = property(SPDRP_MFG, 48);

    wchar_t multi[1024] = {};
    DWORD required = 0;
    DWORD type = 0;
    if (SetupDiGetDeviceRegistryPropertyW(devices, &info, SPDRP_HARDWAREID, &type,
                                          reinterpret_cast<PBYTE>(multi), sizeof(multi),
                                          &required) != 0) {
      // REG_MULTI_SZ is a run of NUL terminated strings closed by an empty one. The
      // walk is bounded by the buffer extent as well as by the terminator, so a
      // device that reports a value without that closing terminator can never make
      // this read past the end of the buffer.
      std::size_t offset = 0;
      while (offset < std::size(multi) && multi[offset] != L'\0' &&
             entry.hardware_ids.size() < kMaxHardwareIds) {
        entry.hardware_ids.push_back(Sanitize(Narrow(multi + offset, 64), 64));
        offset += std::wcslen(multi + offset) + 1;
      }
    }
    if (!entry.instance_id.empty()) {
      entries.push_back(std::move(entry));
    }
  }
  SetupDiDestroyDeviceInfoList(devices);
  return entries;
}

#endif  // _WIN32

}  // namespace

std::string_view DiscoveryFactStatusName(DiscoveryFactStatus status) noexcept {
  switch (status) {
    case DiscoveryFactStatus::Reported:
      return "reported";
    case DiscoveryFactStatus::NotReported:
      return "not-reported";
    case DiscoveryFactStatus::SourceUnavailable:
      return "source-unavailable";
  }
  return "not-reported";
}

std::string RenderDiscoveryReport(const HostDiscoveryReport& report) {
  std::string text;
  text.append("platform=").append(report.platform);
  text.append(" adapters=").append(std::to_string(report.adapter_count));
  text.append(" entities=").append(std::to_string(report.entities.size()));
  for (const std::string& source : report.unavailable_sources) {
    text.append("\nunavailable-source=").append(source);
  }
  for (const DiscoveredEntity& entity : report.entities) {
    text.append("\nentity=").append(entity.entity.ToString());
    text.append(" kind=").append(FabricEntityKindName(entity.kind));
    text.append(" label=").append(entity.operator_label);
    text.append(" provenance=").append(ProvenanceClassName(entity.provenance));
    text.append(" source_class=").append(EvidenceSourceClassName(entity.source_class));
    text.append(" coverage=").append(CoverageName(entity.coverage));
    for (const std::string& note : entity.notes) {
      text.append("\n  note=").append(note);
    }
    for (const DiscoveredFact& fact : entity.facts) {
      text.append("\n  capability=").append(fact.capability.ToString());
      text.append(" status=").append(DiscoveryFactStatusName(fact.status));
      text.append(" state=").append(CapabilityStateName(fact.state));
      text.append(" value=");
      text.append(fact.has_value ? fact.value.ToText() : std::string("absent"));
      if (!fact.note.empty()) {
        text.append(" note=").append(fact.note);
      }
    }
  }
  return text;
}

std::string HostDiscoveryReport::ToText() const { return RenderDiscoveryReport(*this); }

Outcome<HostDiscoveryReport> DiscoverHostCapabilities(const HostDiscoveryOptions& options) {
  HostDiscoveryReport report;
#if defined(_WIN32)
  report.platform = "windows";
  if (options.max_adapters == 0 || options.max_facts_per_entity == 0) {
    return Outcome<HostDiscoveryReport>::Failure(ErrorCode::InvalidArgument,
                                                 "discovery bounds must be non zero");
  }
  std::vector<PnpEntry> pnp;
  if (options.include_pnp_properties) {
    pnp = EnumeratePnpNetworkDevices(options.max_adapters);
    if (pnp.empty()) {
      report.unavailable_sources.push_back("pnp-properties");
    }
  } else {
    report.unavailable_sources.push_back("pnp-properties");
  }

  ULONG size = 16 * 1024;
  std::vector<std::byte> buffer;
  IP_ADAPTER_ADDRESSES* addresses = nullptr;
  for (int attempt = 0; attempt < 4; ++attempt) {
    buffer.assign(size, std::byte{0});
    addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    const ULONG result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addresses, &size);
    if (result == ERROR_SUCCESS) break;
    if (result != ERROR_BUFFER_OVERFLOW) {
      return Outcome<HostDiscoveryReport>::Failure(ErrorCode::DiscoveryFailed,
                                                   "adapter enumeration failed",
                                                   std::to_string(result));
    }
    addresses = nullptr;
  }
  if (addresses == nullptr) {
    return Outcome<HostDiscoveryReport>::Failure(ErrorCode::DiscoveryFailed,
                                                 "adapter enumeration did not converge");
  }

  report.unavailable_sources.push_back("ndis-offload-oids");

  std::size_t adapters = 0;
  for (IP_ADAPTER_ADDRESSES* adapter = addresses; adapter != nullptr && adapters < options.max_adapters;
       adapter = adapter->Next) {
    if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK && !options.include_loopback) continue;
    const bool virtual_adapter = adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
                                 adapter->IfType == IF_TYPE_TUNNEL;
    if (virtual_adapter && !options.include_virtual && !options.include_loopback) continue;

    MIB_IF_ROW2 row{};
    row.InterfaceLuid = adapter->Luid;
    const DWORD row_result = GetIfEntry2(&row);
    const bool have_row = row_result == NO_ERROR;

    DiscoveredEntity entity;
    const std::string guid = LowerGuid(adapter->AdapterName);
    const std::string canonical = guid.empty() ? Sanitize(Narrow(adapter->Description, 48), 48)
                                               : guid;
    auto entity_id = EntityId::Create(FabricEntityKind::Nic, "host-" + canonical);
    if (!entity_id.HasValue()) continue;
    entity.entity = entity_id.Value();
    entity.kind = FabricEntityKind::Nic;
    entity.operator_label = Sanitize(Narrow(adapter->FriendlyName, kMaxAdapterDescription),
                                     kMaxAdapterDescription);
    entity.provenance = ProvenanceClass::DirectDeviceOrOsApi;
    entity.source_class = EvidenceSourceClass::OperatingSystemApi;
    // A host visible NIC exposes only a subset of the true silicon capability
    // surface, so host evidence is always partial coverage.
    entity.coverage = Coverage::Partial;

    for (const PnpEntry& pnp_entry : pnp) {
      if (LowerGuid(pnp_entry.instance_id.c_str()) != guid) continue;
      if (!pnp_entry.description.empty()) {
        AddNote(entity, "pnp-description=" + pnp_entry.description);
      }
      if (!pnp_entry.manufacturer.empty()) {
        AddNote(entity, "pnp-manufacturer=" + pnp_entry.manufacturer);
      }
      for (const std::string& hardware_id : pnp_entry.hardware_ids) {
        AddNote(entity, "pnp-hardware-id=" + hardware_id);
      }
      break;
    }

    FactBuilder facts;
    facts.facts = &entity.facts;
    const std::size_t fact_limit = options.max_facts_per_entity;
    facts.limit = fact_limit;

    const std::uint64_t unknown_speed = 0xFFFFFFFFFFFFFFFFull;
    std::vector<std::uint64_t> speeds;
    if (have_row) {
      for (const std::uint64_t candidate : {row.TransmitLinkSpeed, row.ReceiveLinkSpeed}) {
        if (candidate == 0 || candidate == unknown_speed) continue;
        if (std::find(speeds.begin(), speeds.end(), candidate) == speeds.end()) {
          speeds.push_back(candidate);
        }
      }
    }
    if (!speeds.empty()) {
      auto value = CapabilityValue::NumericSetValue(Unit::BitsPerSecond, speeds);
      if (value.HasValue()) {
        facts.Reported("fabric.port.supported_speeds", value.Value(),
                       "negotiated link speed reported by the interface table; this is evidence "
                       "of one supported speed and not of the complete supported set");
      }
    } else {
      facts.NotReported("fabric.port.supported_speeds",
                        "the interface table did not report a usable link speed");
    }

    if (have_row) {
      std::uint32_t media_code = 0;
      if (MapPhysicalMedium(row.PhysicalMediumType, &media_code)) {
        const std::array<std::uint32_t, 1> codes = {media_code};
        auto parsed_domain = EnumDomainId::Parse("fabric.port.media_type");
        if (parsed_domain.HasValue()) {
          auto value = CapabilityValue::EnumerationSetValue(parsed_domain.Value(), codes);
          if (value.HasValue()) {
            facts.Reported("fabric.port.media_types", value.Value(),
                           std::string("physical medium reported by the interface table: ") +
                               PhysicalMediumName(row.PhysicalMediumType));
          }
        }
      } else {
        facts.NotReported("fabric.port.media_types",
                          std::string("interface table reports physical medium ") +
                              PhysicalMediumName(row.PhysicalMediumType) +
                              " which this build does not map to a canonical media class");
      }
      facts.NotReported("fabric.port.mtu_range",
                        "the operating system reports the configured MTU (" +
                            std::to_string(row.Mtu) +
                            " bytes), not the supported MTU interval");
      AddNote(entity, "os-reported-mtu=" + std::to_string(row.Mtu));
    } else {
      facts.NotReported("fabric.port.media_types",
                        "the interface property table is unavailable for this adapter");
      facts.NotReported("fabric.port.mtu_range",
                        "the interface property table is unavailable for this adapter");
    }

    if (adapter->IfType == IF_TYPE_ETHERNET_CSMACD) {
      auto value = CapabilityValue::ProtocolSetValue(
          std::span<const ProtocolId>(&kEthernetOnlyProtocol, 1));
      if (value.HasValue()) {
        facts.Reported("fabric.protocol.families", value.Value(),
                       "the interface type is Ethernet, which settles the link layer family");
      }
    } else {
      facts.NotReported("fabric.protocol.families",
                        "the interface type " + std::to_string(adapter->IfType) +
                            " is not mapped to a canonical protocol family by this build");
    }

    facts.NotReported("fabric.offload.checksum_rx",
                      "NDIS offload OID or WMI enumeration is not performed by this build");
    facts.NotReported("fabric.offload.checksum_tx",
                      "NDIS offload OID or WMI enumeration is not performed by this build");
    facts.NotReported("fabric.offload.segmentation_offload",
                      "NDIS offload OID or WMI enumeration is not performed by this build");
    facts.NotReported("fabric.offload.receive_side_scaling",
                      "NDIS offload OID or WMI enumeration is not performed by this build");
    facts.NotReported("fabric.virtualization.sriov_supported",
                      "SR-IOV capability enumeration requires a PCI configuration space read");
    facts.NotReported("fabric.timestamping.hardware_timestamp_supported",
                      "hardware timestamping capability is not exposed by the adapter APIs this "
                      "build consults");
    facts.NotReported("fabric.telemetry.families",
                      "telemetry family enumeration is not performed by this build");

    std::sort(entity.facts.begin(), entity.facts.end(),
              [](const DiscoveredFact& lhs, const DiscoveredFact& rhs) {
                return lhs.capability < rhs.capability;
              });
    std::sort(entity.notes.begin(), entity.notes.end());
    report.entities.push_back(std::move(entity));
    ++adapters;
  }
  std::sort(report.entities.begin(), report.entities.end(),
            [](const DiscoveredEntity& lhs, const DiscoveredEntity& rhs) {
              return lhs.entity < rhs.entity;
            });
  report.adapter_count = report.entities.size();
  std::sort(report.unavailable_sources.begin(), report.unavailable_sources.end());
  report.unavailable_sources.erase(
      std::unique(report.unavailable_sources.begin(), report.unavailable_sources.end()),
      report.unavailable_sources.end());
  return report;
#else
  report.platform = "unsupported";
  (void)options;
  return Outcome<HostDiscoveryReport>::Failure(
      ErrorCode::DiscoveryUnavailable,
      "REAL host capability discovery is implemented for Windows hosts only; this platform "
      "provides no truthful source");
#endif
}

}  // namespace fabric::capability
