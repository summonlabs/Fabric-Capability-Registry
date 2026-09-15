// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/capability/evidence.hpp"

#include "fabric/capability/encoding.hpp"
#include "fabric/capability/limits.hpp"

namespace fabric::capability {
namespace {

struct ProvenanceName {
  ProvenanceClass provenance;
  std::string_view name;
};

constexpr std::array<ProvenanceName, kProvenanceClassCount> kProvenanceNames = {{
    {ProvenanceClass::DirectHardwareEnumeration, "direct-hardware-enumeration"},
    {ProvenanceClass::DirectDeviceOrOsApi, "direct-device-or-os-api"},
    {ProvenanceClass::AuthoritativeAdministrativeDeclaration,
     "authoritative-administrative-declaration"},
    {ProvenanceClass::VendorFirmwareOrSdkManifest, "vendor-firmware-or-sdk-manifest"},
    {ProvenanceClass::ImportedStaticProfile, "imported-static-profile"},
    {ProvenanceClass::Inferred, "inferred"},
    {ProvenanceClass::SyntheticTestBackend, "synthetic-test-backend"},
}};

struct SourceClassName {
  EvidenceSourceClass source;
  std::string_view name;
};

constexpr std::array<SourceClassName, kEvidenceSourceClassCount> kSourceClassNames = {{
    {EvidenceSourceClass::HardwareEnumeration, "hardware-enumeration"},
    {EvidenceSourceClass::OperatingSystemApi, "operating-system-api"},
    {EvidenceSourceClass::DriverApi, "driver-api"},
    {EvidenceSourceClass::FirmwareManifest, "firmware-manifest"},
    {EvidenceSourceClass::VendorSdk, "vendor-sdk"},
    {EvidenceSourceClass::DeviceAgent, "device-agent"},
    {EvidenceSourceClass::SwitchAgent, "switch-agent"},
    {EvidenceSourceClass::AdministrativeDeclaration, "administrative-declaration"},
    {EvidenceSourceClass::ImportedManifest, "imported-manifest"},
    {EvidenceSourceClass::SyntheticBackend, "synthetic-backend"},
}};

}  // namespace

std::string_view ProvenanceClassName(ProvenanceClass provenance) noexcept {
  for (const ProvenanceName& entry : kProvenanceNames) {
    if (entry.provenance == provenance) return entry.name;
  }
  return "synthetic-test-backend";
}

Outcome<ProvenanceClass> ParseProvenanceClass(std::string_view text) {
  for (const ProvenanceName& entry : kProvenanceNames) {
    if (entry.name == text) return entry.provenance;
  }
  return Outcome<ProvenanceClass>::Failure(ErrorCode::MalformedValue,
                                           "unknown evidence provenance class", std::string(text));
}

std::uint8_t ProvenanceRank(ProvenanceClass provenance) noexcept {
  return static_cast<std::uint8_t>(provenance);
}

bool IsStrongerThan(ProvenanceClass lhs, ProvenanceClass rhs) noexcept {
  return ProvenanceRank(lhs) < ProvenanceRank(rhs);
}

bool IsDurableByNature(ProvenanceClass provenance) noexcept {
  return ProvenanceRank(provenance) <=
         ProvenanceRank(ProvenanceClass::AuthoritativeAdministrativeDeclaration);
}

bool IsLiveProcessObservation(ProvenanceClass provenance) noexcept {
  return ProvenanceRank(provenance) <= ProvenanceRank(ProvenanceClass::DirectDeviceOrOsApi);
}

std::string_view EvidenceSourceClassName(EvidenceSourceClass source) noexcept {
  for (const SourceClassName& entry : kSourceClassNames) {
    if (entry.source == source) return entry.name;
  }
  return "synthetic-backend";
}

Outcome<EvidenceSourceClass> ParseEvidenceSourceClass(std::string_view text) {
  for (const SourceClassName& entry : kSourceClassNames) {
    if (entry.name == text) return entry.source;
  }
  return Outcome<EvidenceSourceClass>::Failure(ErrorCode::MalformedValue,
                                               "unknown evidence source class", std::string(text));
}

ProvenanceClass StrongestProvenanceForSource(EvidenceSourceClass source) noexcept {
  switch (source) {
    case EvidenceSourceClass::HardwareEnumeration:
      return ProvenanceClass::DirectHardwareEnumeration;
    case EvidenceSourceClass::OperatingSystemApi:
    case EvidenceSourceClass::DriverApi:
    case EvidenceSourceClass::DeviceAgent:
    case EvidenceSourceClass::SwitchAgent:
      return ProvenanceClass::DirectDeviceOrOsApi;
    case EvidenceSourceClass::AdministrativeDeclaration:
      return ProvenanceClass::AuthoritativeAdministrativeDeclaration;
    case EvidenceSourceClass::FirmwareManifest:
    case EvidenceSourceClass::VendorSdk:
      return ProvenanceClass::VendorFirmwareOrSdkManifest;
    case EvidenceSourceClass::ImportedManifest:
      return ProvenanceClass::ImportedStaticProfile;
    case EvidenceSourceClass::SyntheticBackend:
      return ProvenanceClass::SyntheticTestBackend;
  }
  return ProvenanceClass::SyntheticTestBackend;
}

std::string_view DurabilityClassName(DurabilityClass durability) noexcept {
  return durability == DurabilityClass::Durable ? "durable" : "process-bound";
}

std::string_view CoverageName(Coverage coverage) noexcept {
  return coverage == Coverage::FullEnumeration ? "full-enumeration" : "partial";
}

std::string_view EvidenceCurrentnessName(EvidenceCurrentness currentness) noexcept {
  switch (currentness) {
    case EvidenceCurrentness::Current:
      return "current";
    case EvidenceCurrentness::RevalidationRequired:
      return "revalidation-required";
    case EvidenceCurrentness::Fenced:
      return "fenced";
    case EvidenceCurrentness::Superseded:
      return "superseded";
    case EvidenceCurrentness::EntityGenerationSuperseded:
      return "entity-generation-superseded";
  }
  return "current";
}

bool IsCurrent(EvidenceCurrentness currentness) noexcept {
  return currentness == EvidenceCurrentness::Current;
}

Digest EvidenceRecord::SemanticDigest() const {
  std::vector<std::byte> bytes;
  bytes.reserve(256);
  ByteWriter writer(bytes);
  writer.Text(capability.ToString(), limits::kMaxCapabilityIdLength);
  writer.Text(entity.ToString(), limits::kMaxEntityIdLength);
  writer.U64(entity_generation.Value());
  writer.Text(source.Value(), limits::kMaxSourceIdLength);
  writer.Text(publisher.Value(), limits::kMaxPublisherIdLength);
  writer.Text(scope.Value(), limits::kMaxAuthorityScopeIdLength);
  writer.Text(worker_boot.Value(), 64);
  writer.U64(epoch.Value());
  writer.U64(source_generation.Value());
  writer.U64(evidence_generation.Value());
  writer.U8(ProvenanceRank(provenance));
  writer.U8(static_cast<std::uint8_t>(source_class));
  writer.U8(static_cast<std::uint8_t>(durability));
  writer.U8(static_cast<std::uint8_t>(coverage));
  writer.U8(static_cast<std::uint8_t>(state));
  value.Encode(writer);
  writer.Text(firmware_version.Value(), limits::kMaxVersionTokenLength);
  writer.Text(driver_version.Value(), limits::kMaxVersionTokenLength);
  writer.Text(reason.Value(), limits::kMaxTextLength);
  return ComputeDigest(bytes);
}

std::string EvidenceRecord::ToText() const {
  std::string text;
  text.reserve(256);
  text.append("capability=").append(capability.ToString());
  text.append(" state=").append(CapabilityStateName(state));
  text.append(" value=").append(value.IsAbsent() ? "absent" : value.ToText());
  text.append(" provenance=").append(ProvenanceClassName(provenance));
  text.append(" source_class=").append(EvidenceSourceClassName(source_class));
  text.append(" durability=").append(DurabilityClassName(durability));
  text.append(" coverage=").append(CoverageName(coverage));
  text.append(" currentness=").append(EvidenceCurrentnessName(currentness));
  text.append(" source=").append(source.Value());
  text.append(" publisher=").append(publisher.Value());
  text.append(" scope=").append(scope.Value());
  text.append(" boot=").append(worker_boot.Value());
  text.append(" epoch=").append(epoch.ToString());
  text.append(" evidence_generation=").append(evidence_generation.ToString());
  text.append(" source_generation=").append(source_generation.ToString());
  text.append(" entity=").append(entity.ToString());
  text.append(" entity_generation=").append(entity_generation.ToString());
  if (firmware_version.IsSet()) {
    text.append(" firmware=").append(firmware_version.Value());
  }
  if (driver_version.IsSet()) {
    text.append(" driver=").append(driver_version.Value());
  }
  if (reason.IsSet()) {
    text.append(" reason=").append(reason.Value());
  }
  return text;
}

EvidenceId MakeEvidenceId(const PublisherId& publisher, const WorkerBootId& boot,
                          const MutationAttemptId& attempt, const EntityId& entity,
                          EntityGeneration entity_generation, const CapabilityId& capability,
                          std::uint64_t evidence_generation) {
  std::vector<std::byte> bytes;
  bytes.reserve(192);
  ByteWriter writer(bytes);
  writer.Text(publisher.Value(), limits::kMaxPublisherIdLength);
  writer.Text(boot.Value(), 64);
  writer.Text(attempt.Value(), limits::kMaxAttemptIdLength);
  writer.Text(entity.ToString(), limits::kMaxEntityIdLength);
  writer.U64(entity_generation.Value());
  writer.Text(capability.ToString(), limits::kMaxCapabilityIdLength);
  writer.U64(evidence_generation);
  const Digest digest = ComputeDigest(bytes);
  auto parsed = EvidenceId::Parse(digest.ShortString());
  return parsed.HasValue() ? parsed.Value() : EvidenceId{};
}

CapabilityRecordId MakeCapabilityRecordId(const EntityId& entity,
                                          EntityGeneration entity_generation,
                                          const CapabilityId& capability) {
  std::vector<std::byte> bytes;
  bytes.reserve(160);
  ByteWriter writer(bytes);
  writer.Text(entity.ToString(), limits::kMaxEntityIdLength);
  writer.U64(entity_generation.Value());
  writer.Text(capability.ToString(), limits::kMaxCapabilityIdLength);
  const Digest digest = ComputeDigest(bytes);
  auto parsed = CapabilityRecordId::Parse(digest.ShortString());
  return parsed.HasValue() ? parsed.Value() : CapabilityRecordId{};
}

}  // namespace fabric::capability
