// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Bounded, versioned, integrity checked frame codec. Raw C++ structures are
// never written to a socket: every field is encoded and decoded explicitly and
// every enum has a stable wire mapping that is independent of its C++ ordinal.

#include <array>
#include <cstring>
#include <string>
#include <vector>

#include "fabric/capability/digest.hpp"
#include "fabric/capability/distributed.hpp"
#include "fabric/capability/encoding.hpp"
#include "fabric/capability/limits.hpp"

namespace fabric::capability {
namespace {

constexpr std::array<std::byte, 4> kFrameMagic = {std::byte{'F'}, std::byte{'C'}, std::byte{'R'},
                                                  std::byte{'F'}};
constexpr std::size_t kMaxWireDiffEntries = 4096;
constexpr std::size_t kMaxWireExplanationSteps = 256;

// --- stable enum tables ---------------------------------------------------
// The wire code of a value is its index in these tables. Reordering a C++
// enum therefore cannot silently change the protocol.

constexpr std::array<CapabilityState, 5> kStateWire = {
    CapabilityState::Unknown, CapabilityState::Supported, CapabilityState::Unsupported,
    CapabilityState::RevalidationRequired, CapabilityState::Conflicted};

constexpr std::array<PublicationMode, 3> kModeWire = {
    PublicationMode::FullSnapshot, PublicationMode::Incremental,
    PublicationMode::PartialObservation};

constexpr std::array<PublicationStatus, 3> kStatusWire = {
    PublicationStatus::Committed, PublicationStatus::IdempotentReplay,
    PublicationStatus::Rejected};

constexpr std::array<ProvenanceClass, kProvenanceClassCount> kProvenanceWire = {
    ProvenanceClass::DirectHardwareEnumeration, ProvenanceClass::DirectDeviceOrOsApi,
    ProvenanceClass::AuthoritativeAdministrativeDeclaration,
    ProvenanceClass::VendorFirmwareOrSdkManifest, ProvenanceClass::ImportedStaticProfile,
    ProvenanceClass::Inferred, ProvenanceClass::SyntheticTestBackend};

constexpr std::array<EvidenceSourceClass, kEvidenceSourceClassCount> kSourceClassWire = {
    EvidenceSourceClass::HardwareEnumeration, EvidenceSourceClass::OperatingSystemApi,
    EvidenceSourceClass::DriverApi, EvidenceSourceClass::FirmwareManifest,
    EvidenceSourceClass::VendorSdk, EvidenceSourceClass::DeviceAgent,
    EvidenceSourceClass::SwitchAgent, EvidenceSourceClass::AdministrativeDeclaration,
    EvidenceSourceClass::ImportedManifest, EvidenceSourceClass::SyntheticBackend};

constexpr std::array<DurabilityClass, 2> kDurabilityWire = {DurabilityClass::ProcessBound,
                                                            DurabilityClass::Durable};

constexpr std::array<Coverage, 2> kCoverageWire = {Coverage::Partial, Coverage::FullEnumeration};

constexpr std::array<EvidenceCurrentness, 5> kCurrentnessWire = {
    EvidenceCurrentness::Current, EvidenceCurrentness::RevalidationRequired,
    EvidenceCurrentness::Fenced, EvidenceCurrentness::Superseded,
    EvidenceCurrentness::EntityGenerationSuperseded};

constexpr std::array<IncrementalOperation, 6> kOperationWire = {
    IncrementalOperation::UpsertClaim, IncrementalOperation::WithdrawClaim,
    IncrementalOperation::MarkUnsupported, IncrementalOperation::MarkUnknown,
    IncrementalOperation::MarkRevalidationRequired, IncrementalOperation::SupersedeSourceEvidence};

constexpr std::array<CapabilityDiffKind, 10> kDiffKindWire = {
    CapabilityDiffKind::CapabilityAdded, CapabilityDiffKind::CapabilityRemoved,
    CapabilityDiffKind::StateChanged, CapabilityDiffKind::ValueChanged,
    CapabilityDiffKind::ProvenanceChanged, CapabilityDiffKind::EvidenceSuperseded,
    CapabilityDiffKind::CurrentnessChanged, CapabilityDiffKind::GenerationAdvanced,
    CapabilityDiffKind::EntityGenerationChanged, CapabilityDiffKind::EntityInvalidated};

constexpr std::array<WireMessageType, 10> kMessageTypeWire = {
    WireMessageType::Hello, WireMessageType::HelloAck, WireMessageType::Publish,
    WireMessageType::PublishAck, WireMessageType::Fence, WireMessageType::FenceAck,
    WireMessageType::Status, WireMessageType::StatusAck, WireMessageType::Goodbye,
    WireMessageType::ErrorResponse};

template <class Enum, std::size_t N>
std::uint32_t WireCodeOf(const std::array<Enum, N>& table, Enum value) noexcept {
  for (std::size_t index = 0; index < N; ++index) {
    if (table[index] == value) return static_cast<std::uint32_t>(index);
  }
  return 0;
}

template <class Enum, std::size_t N>
bool WireValueOf(const std::array<Enum, N>& table, std::uint32_t raw, Enum* out) noexcept {
  if (raw >= N) return false;
  *out = table[raw];
  return true;
}

Outcome<void> Malformed(std::string_view message, std::string detail = {}) {
  return Status::Failure(ErrorCode::MalformedEncoding, std::string(message), std::move(detail));
}

template <class Enum, std::size_t N>
Outcome<void> WriteEnum(ByteWriter& writer, const std::array<Enum, N>& table, Enum value) {
  writer.U8(static_cast<std::uint8_t>(WireCodeOf(table, value)));
  return Status::Success();
}

template <class Enum, std::size_t N>
Outcome<Enum> ReadEnum(ByteReader& reader, const std::array<Enum, N>& table,
                       std::string_view what) {
  auto raw = reader.U8();
  if (!raw) return raw.GetError();
  Enum value{};
  if (!WireValueOf(table, raw.Value(), &value)) {
    return Outcome<Enum>::Failure(ErrorCode::MalformedEncoding,
                                  "unknown " + std::string(what) + " code",
                                  std::to_string(raw.Value()));
  }
  return value;
}

Outcome<void> WriteErrorCode(ByteWriter& writer, ErrorCode code) {
  if (static_cast<std::uint16_t>(code) > static_cast<std::uint16_t>(ErrorCode::InternalFailure)) {
    return Malformed("error code is outside the encoded range");
  }
  writer.U16(static_cast<std::uint16_t>(code));
  return Status::Success();
}

Outcome<ErrorCode> ReadErrorCode(ByteReader& reader) {
  auto raw = reader.U16();
  if (!raw) return raw.GetError();
  if (raw.Value() > static_cast<std::uint16_t>(ErrorCode::InternalFailure)) {
    return Outcome<ErrorCode>::Failure(ErrorCode::MalformedEncoding,
                                       "unknown error code", std::to_string(raw.Value()));
  }
  return static_cast<ErrorCode>(raw.Value());
}

Outcome<void> WriteText(ByteWriter& writer, const std::string& text, std::size_t limit) {
  return writer.Text(text, limit);
}

Outcome<void> WriteIdentifier(ByteWriter& writer, const std::string& text, std::size_t limit) {
  return writer.Text(text, limit);
}

Outcome<void> WriteValue(ByteWriter& writer, const CapabilityValue& value) {
  value.Encode(writer);
  return Status::Success();
}

Outcome<CapabilityValue> ReadValue(ByteReader& reader) { return CapabilityValue::Decode(reader); }

Outcome<void> WriteAuthority(ByteWriter& writer, const AuthorityContext& authority) {
  auto ok = WriteIdentifier(writer, authority.scope.Value(), limits::kMaxAuthorityScopeIdLength);
  if (!ok) return ok;
  ok = WriteIdentifier(writer, authority.publisher.Value(), limits::kMaxPublisherIdLength);
  if (!ok) return ok;
  ok = WriteIdentifier(writer, authority.worker_boot.Value(), 64);
  if (!ok) return ok;
  ok = WriteIdentifier(writer, authority.source.Value(), limits::kMaxSourceIdLength);
  if (!ok) return ok;
  writer.U64(authority.source_generation.Value());
  return Status::Success();
}

Outcome<AuthorityContext> ReadAuthority(ByteReader& reader) {
  AuthorityContext authority;
  auto scope = reader.Text(limits::kMaxAuthorityScopeIdLength);
  if (!scope) return scope.GetError();
  auto parsed_scope = AuthorityScopeId::Parse(scope.Value());
  if (!parsed_scope) return parsed_scope.GetError();
  authority.scope = parsed_scope.Value();
  auto publisher = reader.Text(limits::kMaxPublisherIdLength);
  if (!publisher) return publisher.GetError();
  auto parsed_publisher = PublisherId::Parse(publisher.Value());
  if (!parsed_publisher) return parsed_publisher.GetError();
  authority.publisher = parsed_publisher.Value();
  auto boot = reader.Text(64);
  if (!boot) return boot.GetError();
  auto parsed_boot = WorkerBootId::Parse(boot.Value());
  if (!parsed_boot) return parsed_boot.GetError();
  authority.worker_boot = parsed_boot.Value();
  auto source = reader.Text(limits::kMaxSourceIdLength);
  if (!source) return source.GetError();
  auto parsed_source = SourceId::Parse(source.Value());
  if (!parsed_source) return parsed_source.GetError();
  authority.source = parsed_source.Value();
  auto generation = reader.U64();
  if (!generation) return generation.GetError();
  authority.source_generation = SourceGeneration::FromValue(generation.Value());
  return authority;
}

Outcome<void> WriteClaim(ByteWriter& writer, const CapabilityClaim& claim) {
  auto ok = WriteIdentifier(writer, claim.capability.ToString(), limits::kMaxCapabilityIdLength);
  if (!ok) return ok;
  ok = WriteEnum(writer, kStateWire, claim.state);
  if (!ok) return ok;
  writer.Bool(!claim.value.IsAbsent());
  if (!claim.value.IsAbsent()) {
    ok = WriteValue(writer, claim.value);
    if (!ok) return ok;
  }
  ok = WriteEnum(writer, kProvenanceWire, claim.provenance);
  if (!ok) return ok;
  ok = WriteEnum(writer, kSourceClassWire, claim.source_class);
  if (!ok) return ok;
  ok = WriteEnum(writer, kDurabilityWire, claim.durability);
  if (!ok) return ok;
  ok = WriteEnum(writer, kCoverageWire, claim.coverage);
  if (!ok) return ok;
  ok = WriteText(writer, claim.firmware_version.Value(), limits::kMaxVersionTokenLength);
  if (!ok) return ok;
  ok = WriteText(writer, claim.driver_version.Value(), limits::kMaxVersionTokenLength);
  if (!ok) return ok;
  return WriteText(writer, claim.reason.Value(), limits::kMaxTextLength);
}

Outcome<CapabilityClaim> ReadClaim(ByteReader& reader) {
  CapabilityClaim claim;
  auto capability = reader.Text(limits::kMaxCapabilityIdLength);
  if (!capability) return capability.GetError();
  auto parsed_capability = CapabilityId::Parse(capability.Value());
  if (!parsed_capability) return parsed_capability.GetError();
  claim.capability = parsed_capability.Value();
  auto state = ReadEnum(reader, kStateWire, "capability state");
  if (!state) return state.GetError();
  claim.state = state.Value();
  auto has_value = reader.Bool();
  if (!has_value) return has_value.GetError();
  if (has_value.Value()) {
    auto value = ReadValue(reader);
    if (!value) return value.GetError();
    claim.value = value.Value();
  }
  auto provenance = ReadEnum(reader, kProvenanceWire, "provenance class");
  if (!provenance) return provenance.GetError();
  claim.provenance = provenance.Value();
  auto source_class = ReadEnum(reader, kSourceClassWire, "evidence source class");
  if (!source_class) return source_class.GetError();
  claim.source_class = source_class.Value();
  auto durability = ReadEnum(reader, kDurabilityWire, "durability class");
  if (!durability) return durability.GetError();
  claim.durability = durability.Value();
  auto coverage = ReadEnum(reader, kCoverageWire, "coverage class");
  if (!coverage) return coverage.GetError();
  claim.coverage = coverage.Value();
  auto firmware = reader.Text(limits::kMaxVersionTokenLength);
  if (!firmware) return firmware.GetError();
  if (!firmware.Value().empty()) {
    auto parsed = VersionToken::Parse(firmware.Value());
    if (!parsed) return parsed.GetError();
    claim.firmware_version = parsed.Value();
  }
  auto driver = reader.Text(limits::kMaxVersionTokenLength);
  if (!driver) return driver.GetError();
  if (!driver.Value().empty()) {
    auto parsed = VersionToken::Parse(driver.Value());
    if (!parsed) return parsed.GetError();
    claim.driver_version = parsed.Value();
  }
  auto reason = reader.Text(limits::kMaxTextLength);
  if (!reason) return reason.GetError();
  if (!reason.Value().empty()) {
    auto parsed = ReasonToken::Parse(reason.Value());
    if (!parsed) return parsed.GetError();
    claim.reason = parsed.Value();
  }
  return claim;
}

Outcome<void> WriteDiffEntry(ByteWriter& writer, const CapabilityDiffEntry& entry) {
  auto ok = WriteIdentifier(writer, entry.capability.ToString(), limits::kMaxCapabilityIdLength);
  if (!ok) return ok;
  ok = WriteEnum(writer, kDiffKindWire, entry.kind);
  if (!ok) return ok;
  ok = WriteEnum(writer, kStateWire, entry.before_state);
  if (!ok) return ok;
  ok = WriteEnum(writer, kStateWire, entry.after_state);
  if (!ok) return ok;
  writer.Bool(entry.has_before_value);
  if (entry.has_before_value) {
    ok = WriteValue(writer, entry.before_value);
    if (!ok) return ok;
  }
  writer.Bool(entry.has_after_value);
  if (entry.has_after_value) {
    ok = WriteValue(writer, entry.after_value);
    if (!ok) return ok;
  }
  ok = WriteEnum(writer, kProvenanceWire, entry.before_provenance);
  if (!ok) return ok;
  ok = WriteEnum(writer, kProvenanceWire, entry.after_provenance);
  if (!ok) return ok;
  writer.U64(entry.before_generation.Value());
  writer.U64(entry.after_generation.Value());
  return WriteText(writer, entry.detail, limits::kMaxTextLength * 4);
}

Outcome<CapabilityDiffEntry> ReadDiffEntry(ByteReader& reader) {
  CapabilityDiffEntry entry;
  auto capability = reader.Text(limits::kMaxCapabilityIdLength);
  if (!capability) return capability.GetError();
  if (!capability.Value().empty()) {
    auto parsed = CapabilityId::Parse(capability.Value());
    if (!parsed) return parsed.GetError();
    entry.capability = parsed.Value();
  }
  auto kind = ReadEnum(reader, kDiffKindWire, "diff kind");
  if (!kind) return kind.GetError();
  entry.kind = kind.Value();
  auto before_state = ReadEnum(reader, kStateWire, "capability state");
  if (!before_state) return before_state.GetError();
  entry.before_state = before_state.Value();
  auto after_state = ReadEnum(reader, kStateWire, "capability state");
  if (!after_state) return after_state.GetError();
  entry.after_state = after_state.Value();
  auto has_before = reader.Bool();
  if (!has_before) return has_before.GetError();
  if (has_before.Value()) {
    auto value = ReadValue(reader);
    if (!value) return value.GetError();
    entry.before_value = value.Value();
    entry.has_before_value = true;
  }
  auto has_after = reader.Bool();
  if (!has_after) return has_after.GetError();
  if (has_after.Value()) {
    auto value = ReadValue(reader);
    if (!value) return value.GetError();
    entry.after_value = value.Value();
    entry.has_after_value = true;
  }
  auto before_provenance = ReadEnum(reader, kProvenanceWire, "provenance class");
  if (!before_provenance) return before_provenance.GetError();
  entry.before_provenance = before_provenance.Value();
  auto after_provenance = ReadEnum(reader, kProvenanceWire, "provenance class");
  if (!after_provenance) return after_provenance.GetError();
  entry.after_provenance = after_provenance.Value();
  auto before_generation = reader.U64();
  if (!before_generation) return before_generation.GetError();
  entry.before_generation = CapabilityGeneration::FromValue(before_generation.Value());
  auto after_generation = reader.U64();
  if (!after_generation) return after_generation.GetError();
  entry.after_generation = CapabilityGeneration::FromValue(after_generation.Value());
  auto detail = reader.Text(limits::kMaxTextLength * 4);
  if (!detail) return detail.GetError();
  entry.detail = detail.Value();
  return entry;
}

Outcome<void> WriteExplanation(ByteWriter& writer, const Explanation& explanation) {
  auto ok = WriteText(writer, explanation.code, limits::kMaxTextLength * 2);
  if (!ok) return ok;
  ok = WriteText(writer, explanation.summary, limits::kMaxTextLength * 4);
  if (!ok) return ok;
  if (explanation.steps.size() > kMaxWireExplanationSteps) {
    return Status::Failure(ErrorCode::TooManyItems, "explanation has too many steps");
  }
  writer.U32(static_cast<std::uint32_t>(explanation.steps.size()));
  for (const ExplanationStep& step : explanation.steps) {
    ok = WriteText(writer, step.code, limits::kMaxTextLength * 2);
    if (!ok) return ok;
    ok = WriteText(writer, step.summary, limits::kMaxTextLength * 4);
    if (!ok) return ok;
    if (step.fields.size() > limits::kMaxTextFieldsPerExplanation) {
      return Status::Failure(ErrorCode::TooManyItems, "explanation step has too many fields");
    }
    writer.U32(static_cast<std::uint32_t>(step.fields.size()));
    for (const auto& field : step.fields) {
      ok = WriteText(writer, field.first, limits::kMaxTextLength);
      if (!ok) return ok;
      ok = WriteText(writer, field.second, limits::kMaxTextLength * 4);
      if (!ok) return ok;
    }
  }
  return Status::Success();
}

Outcome<Explanation> ReadExplanation(ByteReader& reader) {
  Explanation explanation;
  auto code = reader.Text(limits::kMaxTextLength * 2);
  if (!code) return code.GetError();
  explanation.code = code.Value();
  auto summary = reader.Text(limits::kMaxTextLength * 4);
  if (!summary) return summary.GetError();
  explanation.summary = summary.Value();
  auto step_count = reader.U32();
  if (!step_count) return step_count.GetError();
  if (step_count.Value() > kMaxWireExplanationSteps) {
    return Outcome<Explanation>::Failure(ErrorCode::TooManyItems,
                                         "encoded explanation has too many steps");
  }
  for (std::size_t index = 0; index < step_count.Value(); ++index) {
    ExplanationStep step;
    auto step_code = reader.Text(limits::kMaxTextLength * 2);
    if (!step_code) return step_code.GetError();
    step.code = step_code.Value();
    auto step_summary = reader.Text(limits::kMaxTextLength * 4);
    if (!step_summary) return step_summary.GetError();
    step.summary = step_summary.Value();
    auto field_count = reader.U32();
    if (!field_count) return field_count.GetError();
    if (field_count.Value() > limits::kMaxTextFieldsPerExplanation) {
      return Outcome<Explanation>::Failure(ErrorCode::TooManyItems,
                                           "encoded explanation step has too many fields");
    }
    for (std::size_t field = 0; field < field_count.Value(); ++field) {
      auto name = reader.Text(limits::kMaxTextLength);
      if (!name) return name.GetError();
      auto value = reader.Text(limits::kMaxTextLength * 4);
      if (!value) return value.GetError();
      step.fields.emplace_back(name.Value(), value.Value());
    }
    explanation.steps.push_back(std::move(step));
  }
  return explanation;
}

Outcome<void> WriteDigestBytes(ByteWriter& writer, const Digest& digest) {
  for (const std::byte value : digest.Bytes()) writer.U8(static_cast<std::uint8_t>(value));
  return Status::Success();
}

Outcome<Digest> ReadDigestBytes(ByteReader& reader) {
  std::array<std::byte, kDigestBytes> bytes{};
  for (std::size_t index = 0; index < kDigestBytes; ++index) {
    auto value = reader.U8();
    if (!value) return value.GetError();
    bytes[index] = static_cast<std::byte>(value.Value());
  }
  return Digest(bytes);
}

}  // namespace

std::string_view WireMessageTypeName(WireMessageType type) noexcept {
  switch (type) {
    case WireMessageType::Hello:
      return "hello";
    case WireMessageType::HelloAck:
      return "hello-ack";
    case WireMessageType::Publish:
      return "publish";
    case WireMessageType::PublishAck:
      return "publish-ack";
    case WireMessageType::Fence:
      return "fence";
    case WireMessageType::FenceAck:
      return "fence-ack";
    case WireMessageType::Status:
      return "status";
    case WireMessageType::StatusAck:
      return "status-ack";
    case WireMessageType::Goodbye:
      return "goodbye";
    case WireMessageType::ErrorResponse:
      return "error";
  }
  return "unknown";
}

Outcome<std::vector<std::byte>> EncodeFrame(WireMessageType type, std::uint32_t flags,
                                            std::uint64_t request_id,
                                            std::span<const std::byte> payload) {
  if (payload.size() > limits::kMaxFrameBytes) {
    return Outcome<std::vector<std::byte>>::Failure(ErrorCode::FrameTooLarge,
                                                    "frame payload exceeds the bound",
                                                    std::to_string(payload.size()));
  }
  if (WireCodeOf(kMessageTypeWire, type) >= kMessageTypeWire.size()) {
    return Outcome<std::vector<std::byte>>::Failure(ErrorCode::FrameTypeUnknown,
                                                    "unknown message type");
  }
  std::vector<std::byte> frame;
  frame.reserve(kFrameHeaderBytes + payload.size() + 4);
  frame.insert(frame.end(), kFrameMagic.begin(), kFrameMagic.end());
  ByteWriter writer(frame);
  writer.U16(kProtocolVersion);
  writer.U16(static_cast<std::uint16_t>(type));
  writer.U32(flags);
  writer.U64(request_id);
  const auto length = CheckedU32(payload.size());
  if (!length.HasValue()) {
    return Outcome<std::vector<std::byte>>::Failure(ErrorCode::FrameTooLarge,
                                                    "frame payload length cannot be encoded");
  }
  writer.U32(length.Value());
  frame.insert(frame.end(), payload.begin(), payload.end());
  writer.U32(Crc32(payload));
  return frame;
}

Outcome<WireFrame> DecodeFrame(std::span<const std::byte> bytes) {
  if (bytes.size() < kFrameHeaderBytes + 4) {
    return Outcome<WireFrame>::Failure(ErrorCode::FrameMalformed, "frame is shorter than its header",
                                       std::to_string(bytes.size()));
  }
  if (std::memcmp(bytes.data(), kFrameMagic.data(), kFrameMagic.size()) != 0) {
    return Outcome<WireFrame>::Failure(ErrorCode::FrameMalformed, "frame magic does not match");
  }
  ByteReader header(bytes.subspan(4, kFrameHeaderBytes - 4));
  auto version = header.U16();
  if (!version) return version.GetError();
  auto type = header.U16();
  if (!type) return type.GetError();
  auto flags = header.U32();
  if (!flags) return flags.GetError();
  auto request_id = header.U64();
  if (!request_id) return request_id.GetError();
  auto payload_length = header.U32();
  if (!payload_length) return payload_length.GetError();

  if (version.Value() != kProtocolVersion) {
    return Outcome<WireFrame>::Failure(ErrorCode::FrameVersionUnsupported,
                                       "frame protocol version is not supported",
                                       std::to_string(version.Value()));
  }
  WireMessageType message_type{};
  if (!WireValueOf(kMessageTypeWire, type.Value(), &message_type)) {
    return Outcome<WireFrame>::Failure(ErrorCode::FrameTypeUnknown,
                                       "frame message type is unknown",
                                       std::to_string(type.Value()));
  }
  if (payload_length.Value() > limits::kMaxFrameBytes) {
    return Outcome<WireFrame>::Failure(ErrorCode::FrameTooLarge,
                                       "declared payload length exceeds the bound",
                                       std::to_string(payload_length.Value()));
  }
  const std::size_t expected = kFrameHeaderBytes + payload_length.Value() + 4;
  if (bytes.size() != expected) {
    return Outcome<WireFrame>::Failure(
        ErrorCode::FrameTrailingBytes,
        "frame size does not match the declared payload length",
        "actual=" + std::to_string(bytes.size()) + " expected=" + std::to_string(expected));
  }
  const std::span<const std::byte> payload = bytes.subspan(kFrameHeaderBytes,
                                                           payload_length.Value());
  ByteReader trailer(bytes.subspan(kFrameHeaderBytes + payload_length.Value(), 4));
  auto crc = trailer.U32();
  if (!crc) return crc.GetError();
  if (crc.Value() != Crc32(payload)) {
    return Outcome<WireFrame>::Failure(ErrorCode::FrameIntegrityFailure,
                                       "frame payload CRC does not match");
  }
  WireFrame frame;
  frame.version = version.Value();
  frame.type = message_type;
  frame.flags = flags.Value();
  frame.request_id = request_id.Value();
  frame.payload.assign(payload.begin(), payload.end());
  return frame;
}

Outcome<std::vector<std::byte>> EncodePublicationRequest(const PublicationRequest& request) {
  std::vector<std::byte> payload;
  payload.reserve(128 + request.claims.size() * 64);
  ByteWriter writer(payload);
  auto ok = WriteEnum(writer, kModeWire, request.mode);
  if (!ok) return ok.GetError();
  ok = WriteIdentifier(writer, request.attempt.Value(), limits::kMaxAttemptIdLength);
  if (!ok) return ok.GetError();
  ok = WriteIdentifier(writer, request.publication.Value(), limits::kMaxPublicationIdLength);
  if (!ok) return ok.GetError();
  writer.U64(request.epoch.Value());
  ok = WriteAuthority(writer, request.authority);
  if (!ok) return ok.GetError();
  ok = WriteIdentifier(writer, request.entity.ToString(), limits::kMaxEntityIdLength);
  if (!ok) return ok.GetError();
  writer.U64(request.entity_generation.Value());
  writer.U64(request.expected_set_generation.Value());
  ok = WriteEnum(writer, kCoverageWire, request.coverage);
  if (!ok) return ok.GetError();
  ok = WriteText(writer, request.reason.Value(), limits::kMaxTextLength);
  if (!ok) return ok.GetError();
  if (request.claims.size() > limits::kMaxClaimsPerPublication ||
      request.edits.size() > limits::kMaxClaimsPerPublication) {
    return Outcome<std::vector<std::byte>>::Failure(ErrorCode::TooManyItems,
                                                    "publication exceeds the claim bound");
  }
  writer.U32(static_cast<std::uint32_t>(request.claims.size()));
  for (const CapabilityClaim& claim : request.claims) {
    ok = WriteClaim(writer, claim);
    if (!ok) return ok.GetError();
  }
  writer.U32(static_cast<std::uint32_t>(request.edits.size()));
  for (const IncrementalEdit& edit : request.edits) {
    ok = WriteEnum(writer, kOperationWire, edit.operation);
    if (!ok) return ok.GetError();
    CapabilityClaim claim = edit.claim;
    if (claim.capability.IsSet()) {
      return Outcome<std::vector<std::byte>>::Failure(
          ErrorCode::MalformedEncoding,
          "an incremental edit carries the capability in its own field, not in the claim");
    }
    claim.capability = edit.capability;
    ok = WriteClaim(writer, claim);
    if (!ok) return ok.GetError();
  }
  return payload;
}

Outcome<PublicationRequest> DecodePublicationRequest(std::span<const std::byte> payload) {
  ByteReader reader(payload);
  PublicationRequest request;
  auto mode = ReadEnum(reader, kModeWire, "publication mode");
  if (!mode) return mode.GetError();
  request.mode = mode.Value();
  auto attempt = reader.Text(limits::kMaxAttemptIdLength);
  if (!attempt) return attempt.GetError();
  auto parsed_attempt = MutationAttemptId::Parse(attempt.Value());
  if (!parsed_attempt) return parsed_attempt.GetError();
  request.attempt = parsed_attempt.Value();
  auto publication = reader.Text(limits::kMaxPublicationIdLength);
  if (!publication) return publication.GetError();
  auto parsed_publication = PublicationId::Parse(publication.Value());
  if (!parsed_publication) return parsed_publication.GetError();
  request.publication = parsed_publication.Value();
  auto epoch = reader.U64();
  if (!epoch) return epoch.GetError();
  request.epoch = CoordinatorEpoch::FromValue(epoch.Value());
  auto authority = ReadAuthority(reader);
  if (!authority) return authority.GetError();
  request.authority = authority.Value();
  auto entity = reader.Text(limits::kMaxEntityIdLength);
  if (!entity) return entity.GetError();
  auto parsed_entity = EntityId::Parse(entity.Value());
  if (!parsed_entity) return parsed_entity.GetError();
  request.entity = parsed_entity.Value();
  auto entity_generation = reader.U64();
  if (!entity_generation) return entity_generation.GetError();
  request.entity_generation = EntityGeneration::FromValue(entity_generation.Value());
  auto set_generation = reader.U64();
  if (!set_generation) return set_generation.GetError();
  request.expected_set_generation = CapabilitySetGeneration::FromValue(set_generation.Value());
  auto coverage = ReadEnum(reader, kCoverageWire, "coverage class");
  if (!coverage) return coverage.GetError();
  request.coverage = coverage.Value();
  auto reason = reader.Text(limits::kMaxTextLength);
  if (!reason) return reason.GetError();
  if (!reason.Value().empty()) {
    auto parsed = ReasonToken::Parse(reason.Value());
    if (!parsed) return parsed.GetError();
    request.reason = parsed.Value();
  }
  auto claim_count = reader.U32();
  if (!claim_count) return claim_count.GetError();
  if (claim_count.Value() > limits::kMaxClaimsPerPublication) {
    return Outcome<PublicationRequest>::Failure(ErrorCode::TooManyItems,
                                                "encoded publication exceeds the claim bound");
  }
  for (std::size_t index = 0; index < claim_count.Value(); ++index) {
    auto claim = ReadClaim(reader);
    if (!claim) return claim.GetError();
    request.claims.push_back(std::move(claim.Value()));
  }
  auto edit_count = reader.U32();
  if (!edit_count) return edit_count.GetError();
  if (edit_count.Value() > limits::kMaxClaimsPerPublication) {
    return Outcome<PublicationRequest>::Failure(ErrorCode::TooManyItems,
                                                "encoded publication exceeds the edit bound");
  }
  for (std::size_t index = 0; index < edit_count.Value(); ++index) {
    auto operation = ReadEnum(reader, kOperationWire, "incremental operation");
    if (!operation) return operation.GetError();
    auto capability = reader.Text(limits::kMaxCapabilityIdLength);
    if (!capability) return capability.GetError();
    auto parsed_capability = CapabilityId::Parse(capability.Value());
    if (!parsed_capability) return parsed_capability.GetError();
    auto claim = ReadClaim(reader);
    if (!claim) return claim.GetError();
    IncrementalEdit edit;
    edit.operation = operation.Value();
    edit.capability = parsed_capability.Value();
    edit.claim = claim.Value();
    edit.claim.capability = CapabilityId{};
    request.edits.push_back(std::move(edit));
  }
  auto end = reader.ExpectEnd();
  if (!end) return end.GetError();
  return request;
}

Outcome<std::vector<std::byte>> EncodePublicationResult(const PublicationResult& result) {
  std::vector<std::byte> payload;
  payload.reserve(256 + result.diff.entries.size() * 96);
  ByteWriter writer(payload);
  auto ok = WriteEnum(writer, kStatusWire, result.status);
  if (!ok) return ok.GetError();
  ok = WriteErrorCode(writer, result.code);
  if (!ok) return ok.GetError();
  ok = WriteText(writer, result.message, limits::kMaxTextLength * 4);
  if (!ok) return ok.GetError();
  ok = WriteIdentifier(writer, result.publication.Value(), limits::kMaxPublicationIdLength);
  if (!ok) return ok.GetError();
  ok = WriteIdentifier(writer, result.attempt.Value(), limits::kMaxAttemptIdLength);
  if (!ok) return ok.GetError();
  ok = WriteIdentifier(writer, result.entity.ToString(), limits::kMaxEntityIdLength);
  if (!ok) return ok.GetError();
  writer.U64(result.entity_generation.Value());
  writer.U64(result.previous_set_generation.Value());
  writer.U64(result.new_set_generation.Value());
  writer.U64(result.registry_generation.Value());
  writer.U64(result.claims_applied);
  writer.U64(result.claims_withdrawn);
  writer.U64(result.capabilities_added);
  writer.U64(result.capabilities_removed);
  writer.U64(result.capabilities_changed);
  ok = WriteDigestBytes(writer, result.set_digest);
  if (!ok) return ok.GetError();

  ok = WriteIdentifier(writer, result.diff.entity.ToString(), limits::kMaxEntityIdLength);
  if (!ok) return ok.GetError();
  writer.U64(result.diff.before_entity_generation.Value());
  writer.U64(result.diff.after_entity_generation.Value());
  writer.U64(result.diff.before_set_generation.Value());
  writer.U64(result.diff.after_set_generation.Value());
  ok = WriteDigestBytes(writer, result.diff.before_digest);
  if (!ok) return ok.GetError();
  ok = WriteDigestBytes(writer, result.diff.after_digest);
  if (!ok) return ok.GetError();
  if (result.diff.entries.size() > kMaxWireDiffEntries) {
    return Outcome<std::vector<std::byte>>::Failure(ErrorCode::TooManyItems,
                                                    "publication diff has too many entries");
  }
  writer.U32(static_cast<std::uint32_t>(result.diff.entries.size()));
  for (const CapabilityDiffEntry& entry : result.diff.entries) {
    ok = WriteDiffEntry(writer, entry);
    if (!ok) return ok.GetError();
  }
  ok = WriteExplanation(writer, result.explanation);
  if (!ok) return ok.GetError();
  return payload;
}

Outcome<PublicationResult> DecodePublicationResult(std::span<const std::byte> payload) {
  ByteReader reader(payload);
  PublicationResult result;
  auto status = ReadEnum(reader, kStatusWire, "publication status");
  if (!status) return status.GetError();
  result.status = status.Value();
  auto code = ReadErrorCode(reader);
  if (!code) return code.GetError();
  result.code = code.Value();
  auto message = reader.Text(limits::kMaxTextLength * 4);
  if (!message) return message.GetError();
  result.message = message.Value();
  auto publication = reader.Text(limits::kMaxPublicationIdLength);
  if (!publication) return publication.GetError();
  if (!publication.Value().empty()) {
    auto parsed = PublicationId::Parse(publication.Value());
    if (!parsed) return parsed.GetError();
    result.publication = parsed.Value();
  }
  auto attempt = reader.Text(limits::kMaxAttemptIdLength);
  if (!attempt) return attempt.GetError();
  if (!attempt.Value().empty()) {
    auto parsed = MutationAttemptId::Parse(attempt.Value());
    if (!parsed) return parsed.GetError();
    result.attempt = parsed.Value();
  }
  auto entity = reader.Text(limits::kMaxEntityIdLength);
  if (!entity) return entity.GetError();
  if (!entity.Value().empty()) {
    auto parsed = EntityId::Parse(entity.Value());
    if (!parsed) return parsed.GetError();
    result.entity = parsed.Value();
  }
  auto entity_generation = reader.U64();
  if (!entity_generation) return entity_generation.GetError();
  result.entity_generation = EntityGeneration::FromValue(entity_generation.Value());
  auto previous_generation = reader.U64();
  if (!previous_generation) return previous_generation.GetError();
  result.previous_set_generation = CapabilitySetGeneration::FromValue(previous_generation.Value());
  auto new_generation = reader.U64();
  if (!new_generation) return new_generation.GetError();
  result.new_set_generation = CapabilitySetGeneration::FromValue(new_generation.Value());
  auto registry_generation = reader.U64();
  if (!registry_generation) return registry_generation.GetError();
  result.registry_generation = RegistryGeneration::FromValue(registry_generation.Value());
  auto claims_applied = reader.U64();
  if (!claims_applied) return claims_applied.GetError();
  result.claims_applied = static_cast<std::size_t>(claims_applied.Value());
  auto claims_withdrawn = reader.U64();
  if (!claims_withdrawn) return claims_withdrawn.GetError();
  result.claims_withdrawn = static_cast<std::size_t>(claims_withdrawn.Value());
  auto added = reader.U64();
  if (!added) return added.GetError();
  result.capabilities_added = static_cast<std::size_t>(added.Value());
  auto removed = reader.U64();
  if (!removed) return removed.GetError();
  result.capabilities_removed = static_cast<std::size_t>(removed.Value());
  auto changed = reader.U64();
  if (!changed) return changed.GetError();
  result.capabilities_changed = static_cast<std::size_t>(changed.Value());
  auto set_digest = ReadDigestBytes(reader);
  if (!set_digest) return set_digest.GetError();
  result.set_digest = set_digest.Value();

  auto diff_entity = reader.Text(limits::kMaxEntityIdLength);
  if (!diff_entity) return diff_entity.GetError();
  if (!diff_entity.Value().empty()) {
    auto parsed = EntityId::Parse(diff_entity.Value());
    if (!parsed) return parsed.GetError();
    result.diff.entity = parsed.Value();
  }
  auto before_entity_generation = reader.U64();
  if (!before_entity_generation) return before_entity_generation.GetError();
  result.diff.before_entity_generation = EntityGeneration::FromValue(before_entity_generation.Value());
  auto after_entity_generation = reader.U64();
  if (!after_entity_generation) return after_entity_generation.GetError();
  result.diff.after_entity_generation = EntityGeneration::FromValue(after_entity_generation.Value());
  auto before_set_generation = reader.U64();
  if (!before_set_generation) return before_set_generation.GetError();
  result.diff.before_set_generation = CapabilitySetGeneration::FromValue(before_set_generation.Value());
  auto after_set_generation = reader.U64();
  if (!after_set_generation) return after_set_generation.GetError();
  result.diff.after_set_generation = CapabilitySetGeneration::FromValue(after_set_generation.Value());
  auto before_digest = ReadDigestBytes(reader);
  if (!before_digest) return before_digest.GetError();
  result.diff.before_digest = before_digest.Value();
  auto after_digest = ReadDigestBytes(reader);
  if (!after_digest) return after_digest.GetError();
  result.diff.after_digest = after_digest.Value();
  auto entry_count = reader.U32();
  if (!entry_count) return entry_count.GetError();
  if (entry_count.Value() > kMaxWireDiffEntries) {
    return Outcome<PublicationResult>::Failure(ErrorCode::TooManyItems,
                                               "encoded diff has too many entries");
  }
  for (std::size_t index = 0; index < entry_count.Value(); ++index) {
    auto entry = ReadDiffEntry(reader);
    if (!entry) return entry.GetError();
    result.diff.entries.push_back(std::move(entry.Value()));
  }
  auto explanation = ReadExplanation(reader);
  if (!explanation) return explanation.GetError();
  result.explanation = explanation.Value();
  auto end = reader.ExpectEnd();
  if (!end) return end.GetError();
  return result;
}

Outcome<std::vector<std::byte>> EncodeHello(const HelloPayload& hello) {
  std::vector<std::byte> payload;
  payload.reserve(64 + hello.namespaces.size() * 24);
  ByteWriter writer(payload);
  auto ok = WriteIdentifier(writer, hello.publisher.Value(), limits::kMaxPublisherIdLength);
  if (!ok) return ok.GetError();
  ok = WriteIdentifier(writer, hello.scope.Value(), limits::kMaxAuthorityScopeIdLength);
  if (!ok) return ok.GetError();
  ok = WriteIdentifier(writer, hello.source.Value(), limits::kMaxSourceIdLength);
  if (!ok) return ok.GetError();
  ok = WriteIdentifier(writer, hello.worker_boot.Value(), 64);
  if (!ok) return ok.GetError();
  writer.U64(hello.epoch.Value());
  if (hello.namespaces.size() > limits::kMaxAuthorityNamespaces) {
    return Outcome<std::vector<std::byte>>::Failure(ErrorCode::TooManyItems,
                                                    "hello declares too many namespaces");
  }
  writer.U32(static_cast<std::uint32_t>(hello.namespaces.size()));
  for (const CapabilityNamespaceId& ns : hello.namespaces) {
    ok = WriteIdentifier(writer, ns.Value(), limits::kMaxCapabilityIdLength);
    if (!ok) return ok.GetError();
  }
  return payload;
}

Outcome<HelloPayload> DecodeHello(std::span<const std::byte> payload) {
  ByteReader reader(payload);
  HelloPayload hello;
  auto publisher = reader.Text(limits::kMaxPublisherIdLength);
  if (!publisher) return publisher.GetError();
  auto parsed_publisher = PublisherId::Parse(publisher.Value());
  if (!parsed_publisher) return parsed_publisher.GetError();
  hello.publisher = parsed_publisher.Value();
  auto scope = reader.Text(limits::kMaxAuthorityScopeIdLength);
  if (!scope) return scope.GetError();
  auto parsed_scope = AuthorityScopeId::Parse(scope.Value());
  if (!parsed_scope) return parsed_scope.GetError();
  hello.scope = parsed_scope.Value();
  auto source = reader.Text(limits::kMaxSourceIdLength);
  if (!source) return source.GetError();
  auto parsed_source = SourceId::Parse(source.Value());
  if (!parsed_source) return parsed_source.GetError();
  hello.source = parsed_source.Value();
  auto boot = reader.Text(64);
  if (!boot) return boot.GetError();
  auto parsed_boot = WorkerBootId::Parse(boot.Value());
  if (!parsed_boot) return parsed_boot.GetError();
  hello.worker_boot = parsed_boot.Value();
  auto epoch = reader.U64();
  if (!epoch) return epoch.GetError();
  hello.epoch = CoordinatorEpoch::FromValue(epoch.Value());
  auto namespace_count = reader.U32();
  if (!namespace_count) return namespace_count.GetError();
  if (namespace_count.Value() > limits::kMaxAuthorityNamespaces) {
    return Outcome<HelloPayload>::Failure(ErrorCode::TooManyItems,
                                          "encoded hello declares too many namespaces");
  }
  for (std::size_t index = 0; index < namespace_count.Value(); ++index) {
    auto ns = reader.Text(limits::kMaxCapabilityIdLength);
    if (!ns) return ns.GetError();
    auto parsed = CapabilityNamespaceId::Parse(ns.Value());
    if (!parsed) return parsed.GetError();
    hello.namespaces.push_back(parsed.Value());
  }
  auto end = reader.ExpectEnd();
  if (!end) return end.GetError();
  return hello;
}

Outcome<std::vector<std::byte>> EncodeHelloAck(const HelloAckPayload& ack) {
  std::vector<std::byte> payload;
  payload.reserve(32);
  ByteWriter writer(payload);
  writer.Bool(ack.accepted);
  writer.U64(ack.epoch.Value());
  writer.U64(ack.registry_generation.Value());
  auto ok = WriteText(writer, ack.message, limits::kMaxTextLength * 4);
  if (!ok) return ok.GetError();
  return payload;
}

Outcome<HelloAckPayload> DecodeHelloAck(std::span<const std::byte> payload) {
  ByteReader reader(payload);
  HelloAckPayload ack;
  auto accepted = reader.Bool();
  if (!accepted) return accepted.GetError();
  ack.accepted = accepted.Value();
  auto epoch = reader.U64();
  if (!epoch) return epoch.GetError();
  ack.epoch = CoordinatorEpoch::FromValue(epoch.Value());
  auto generation = reader.U64();
  if (!generation) return generation.GetError();
  ack.registry_generation = RegistryGeneration::FromValue(generation.Value());
  auto message = reader.Text(limits::kMaxTextLength * 4);
  if (!message) return message.GetError();
  ack.message = message.Value();
  auto end = reader.ExpectEnd();
  if (!end) return end.GetError();
  return ack;
}

Outcome<std::vector<std::byte>> EncodeStatus(const StatusPayload& status) {
  std::vector<std::byte> payload;
  payload.reserve(64);
  ByteWriter writer(payload);
  auto ok = WriteIdentifier(writer, status.entity.ToString(), limits::kMaxEntityIdLength);
  if (!ok) return ok.GetError();
  writer.U64(status.entity_generation.Value());
  writer.U64(status.set_generation.Value());
  writer.U64(status.registry_generation.Value());
  writer.U64(status.epoch.Value());
  return payload;
}

Outcome<StatusPayload> DecodeStatus(std::span<const std::byte> payload) {
  ByteReader reader(payload);
  StatusPayload status;
  auto entity = reader.Text(limits::kMaxEntityIdLength);
  if (!entity) return entity.GetError();
  if (!entity.Value().empty()) {
    auto parsed = EntityId::Parse(entity.Value());
    if (!parsed) return parsed.GetError();
    status.entity = parsed.Value();
  }
  auto entity_generation = reader.U64();
  if (!entity_generation) return entity_generation.GetError();
  status.entity_generation = EntityGeneration::FromValue(entity_generation.Value());
  auto set_generation = reader.U64();
  if (!set_generation) return set_generation.GetError();
  status.set_generation = CapabilitySetGeneration::FromValue(set_generation.Value());
  auto registry_generation = reader.U64();
  if (!registry_generation) return registry_generation.GetError();
  status.registry_generation = RegistryGeneration::FromValue(registry_generation.Value());
  auto epoch = reader.U64();
  if (!epoch) return epoch.GetError();
  status.epoch = CoordinatorEpoch::FromValue(epoch.Value());
  auto end = reader.ExpectEnd();
  if (!end) return end.GetError();
  return status;
}

Outcome<std::vector<std::byte>> EncodeFence(const FencePayload& fence) {
  std::vector<std::byte> payload;
  payload.reserve(32);
  ByteWriter writer(payload);
  auto ok = WriteIdentifier(writer, fence.worker_boot.Value(), 64);
  if (!ok) return ok.GetError();
  ok = WriteText(writer, fence.reason.Value(), limits::kMaxTextLength);
  if (!ok) return ok.GetError();
  return payload;
}

Outcome<FencePayload> DecodeFence(std::span<const std::byte> payload) {
  ByteReader reader(payload);
  FencePayload fence;
  auto boot = reader.Text(64);
  if (!boot) return boot.GetError();
  auto parsed_boot = WorkerBootId::Parse(boot.Value());
  if (!parsed_boot) return parsed_boot.GetError();
  fence.worker_boot = parsed_boot.Value();
  auto reason = reader.Text(limits::kMaxTextLength);
  if (!reason) return reason.GetError();
  if (!reason.Value().empty()) {
    auto parsed = ReasonToken::Parse(reason.Value());
    if (!parsed) return parsed.GetError();
    fence.reason = parsed.Value();
  }
  auto end = reader.ExpectEnd();
  if (!end) return end.GetError();
  return fence;
}

Outcome<std::vector<std::byte>> EncodeErrorPayload(const ErrorPayload& error) {
  std::vector<std::byte> payload;
  payload.reserve(32);
  ByteWriter writer(payload);
  auto ok = WriteErrorCode(writer, error.code);
  if (!ok) return ok.GetError();
  ok = WriteText(writer, error.message, limits::kMaxTextLength * 4);
  if (!ok) return ok.GetError();
  return payload;
}

Outcome<ErrorPayload> DecodeErrorPayload(std::span<const std::byte> payload) {
  ByteReader reader(payload);
  ErrorPayload error;
  auto code = ReadErrorCode(reader);
  if (!code) return code.GetError();
  error.code = code.Value();
  auto message = reader.Text(limits::kMaxTextLength * 4);
  if (!message) return message.GetError();
  error.message = message.Value();
  auto end = reader.ExpectEnd();
  if (!end) return end.GetError();
  return error;
}

}  // namespace fabric::capability
