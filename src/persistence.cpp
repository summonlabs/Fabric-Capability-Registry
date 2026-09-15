// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/capability/persistence.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "fabric/capability/encoding.hpp"
#include "fabric/capability/limits.hpp"
#include "fabric/capability/state.hpp"
#include "fabric/capability/version.hpp"
#include "src/internal/persistence_io.hpp"
#include "src/internal/store_format.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fabric::capability {
namespace {

constexpr std::string_view kStoreSuffix = ".fcrstore";
constexpr std::uint32_t kFlagsNone = 0;

constexpr std::uint8_t kRecordMeta = 1;
constexpr std::uint8_t kRecordAuthority = 2;
constexpr std::uint8_t kRecordFence = 3;
constexpr std::uint8_t kRecordEntity = 4;

bool IsAsciiAlphaNum(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
}

bool IsReservedDeviceName(const std::string& stem) {
  std::string upper;
  upper.reserve(stem.size());
  for (const char ch : stem) {
    upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
  }
  const std::size_t dot = upper.find('.');
  const std::string base = dot == std::string::npos ? upper : upper.substr(0, dot);
  static const char* kReserved[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4",
                                    "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2",
                                    "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
  for (const char* reserved : kReserved) {
    if (base == reserved) return true;
  }
  return false;
}

Outcome<void> ValidateStem(const std::string& stem) {
  if (stem.empty() || stem.size() > 64) {
    return Status::Failure(ErrorCode::PersistencePathInvalid,
                           "store file stem length is out of range",
                           std::to_string(stem.size()));
  }
  if (stem == "." || stem == "..") {
    return Status::Failure(ErrorCode::PersistencePathInvalid,
                           "store file stem must not be a traversal segment", stem);
  }
  if (stem.front() == '.' || stem.back() == '.' || stem.back() == ' ') {
    return Status::Failure(ErrorCode::PersistencePathInvalid,
                           "store file stem must not start or end with a dot or space", stem);
  }
  for (const char ch : stem) {
    if (IsAsciiAlphaNum(ch) || ch == '-' || ch == '_' || ch == '.') continue;
    return Status::Failure(ErrorCode::PersistencePathInvalid,
                           "store file stem may only contain letters, digits, '-', '_' and '.'",
                           stem);
  }
  if (IsReservedDeviceName(stem)) {
    return Status::Failure(ErrorCode::PersistencePathInvalid,
                           "store file stem is a reserved device name", stem);
  }
  return Status::Success();
}

std::string NarrowPath(const std::filesystem::path& path) {
#if defined(_WIN32)
  const std::wstring wide = path.wstring();
  if (wide.empty()) return std::string();
  const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                                       nullptr, 0, nullptr, nullptr);
  std::string narrow(static_cast<std::size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), narrow.data(), size,
                      nullptr, nullptr);
  return narrow;
#else
  return path.string();
#endif
}

#if defined(_WIN32)
std::wstring WidenPath(const std::string& text) {
  if (text.empty()) return std::wstring();
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                       nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), size);
  return wide;
}
#endif

/// Reads a whole file with a hard bound. Fails rather than growing without
/// limit on a hostile or corrupted file.
Outcome<std::vector<std::byte>> ReadFileBounded(const std::filesystem::path& path,
                                                std::size_t max_bytes) {
#if defined(_WIN32)
  const std::wstring wide = path.wstring();
  HANDLE handle = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Outcome<std::vector<std::byte>>::Failure(ErrorCode::PersistenceIoFailure,
                                                    "cannot open the store file for reading",
                                                    NarrowPath(path));
  }
  LARGE_INTEGER size{};
  if (GetFileSizeEx(handle, &size) == 0 || size.QuadPart < 0) {
    CloseHandle(handle);
    return Outcome<std::vector<std::byte>>::Failure(ErrorCode::PersistenceIoFailure,
                                                    "cannot determine the store file size",
                                                    NarrowPath(path));
  }
  if (static_cast<std::uint64_t>(size.QuadPart) > max_bytes) {
    CloseHandle(handle);
    return Outcome<std::vector<std::byte>>::Failure(
        ErrorCode::PersistenceLimitExceeded, "store file exceeds the configured bound",
        std::to_string(size.QuadPart));
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(size.QuadPart));
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const DWORD chunk = static_cast<DWORD>(
        std::min<std::size_t>(bytes.size() - offset, 1u << 20));
    DWORD read = 0;
    if (ReadFile(handle, bytes.data() + offset, chunk, &read, nullptr) == 0 || read == 0) {
      CloseHandle(handle);
      return Outcome<std::vector<std::byte>>::Failure(ErrorCode::PersistenceIoFailure,
                                                      "short read from the store file",
                                                      NarrowPath(path));
    }
    offset += read;
  }
  CloseHandle(handle);
  return bytes;
#else
  std::FILE* file = std::fopen(path.string().c_str(), "rb");
  if (file == nullptr) {
    return Outcome<std::vector<std::byte>>::Failure(ErrorCode::PersistenceIoFailure,
                                                    "cannot open the store file for reading",
                                                    path.string());
  }
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  if (size < 0 || static_cast<std::uint64_t>(size) > max_bytes) {
    std::fclose(file);
    return Outcome<std::vector<std::byte>>::Failure(ErrorCode::PersistenceLimitExceeded,
                                                    "store file size is out of range");
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  if (!bytes.empty() && std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) {
    std::fclose(file);
    return Outcome<std::vector<std::byte>>::Failure(ErrorCode::PersistenceIoFailure,
                                                    "short read from the store file");
  }
  std::fclose(file);
  return bytes;
#endif
}

/// Writes bytes durably to a fresh temporary file and atomically replaces the
/// target. A failure never leaves a partially written store behind.
Outcome<void> WriteFileAtomic(const std::filesystem::path& target,
                              const std::vector<std::byte>& bytes, bool durable_flush,
                              const std::filesystem::path& temporary) {
#if defined(_WIN32)
  const std::wstring wide = temporary.wstring();
  HANDLE handle = CreateFileW(wide.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Status::Failure(ErrorCode::PersistenceIoFailure,
                           "cannot create the temporary store file", NarrowPath(temporary));
  }
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const DWORD chunk =
        static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1u << 20));
    DWORD written = 0;
    if (WriteFile(handle, bytes.data() + offset, chunk, &written, nullptr) == 0 ||
        written == 0) {
      CloseHandle(handle);
      DeleteFileW(wide.c_str());
      return Status::Failure(ErrorCode::PersistenceIoFailure, "write to the store file failed",
                             NarrowPath(temporary));
    }
    offset += written;
  }
  if (durable_flush) {
    FlushFileBuffers(handle);
  }
  CloseHandle(handle);
  if (MoveFileExW(wide.c_str(), target.wstring().c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    DeleteFileW(wide.c_str());
    return Status::Failure(ErrorCode::PersistenceIoFailure,
                           "atomic replacement of the store file failed", NarrowPath(target));
  }
  return Status::Success();
#else
  const std::string temporary_path = temporary.string();
  const int fd = ::open(temporary_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return Status::Failure(ErrorCode::PersistenceIoFailure,
                           "cannot create the temporary store file", temporary_path);
  }
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t written = ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (written <= 0) {
      ::close(fd);
      ::unlink(temporary_path.c_str());
      return Status::Failure(ErrorCode::PersistenceIoFailure, "write to the store file failed",
                             temporary_path);
    }
    offset += static_cast<std::size_t>(written);
  }
  if (durable_flush) ::fsync(fd);
  ::close(fd);
  std::error_code error;
  std::filesystem::rename(temporary, target, error);
  if (error) {
    std::filesystem::remove(temporary, error);
    return Status::Failure(ErrorCode::PersistenceIoFailure,
                           "atomic replacement of the store file failed", error.message());
  }
  return Status::Success();
#endif
}

// --- payload codec --------------------------------------------------------

void WriteDigest(ByteWriter& writer, const Digest& digest) {
  for (const std::byte value : digest.Bytes()) writer.U8(static_cast<std::uint8_t>(value));
}

Outcome<Digest> ReadDigest(ByteReader& reader) {
  std::array<std::byte, kDigestBytes> bytes{};
  for (std::size_t index = 0; index < kDigestBytes; ++index) {
    auto value = reader.U8();
    if (!value) return value.GetError();
    bytes[index] = static_cast<std::byte>(value.Value());
  }
  return Digest(bytes);
}

Outcome<void> WriteClaim(ByteWriter& writer, const internal::StoredClaim& claim) {
  auto ok = writer.Text(claim.capability.ToString(), limits::kMaxCapabilityIdLength);
  if (!ok) return ok;
  ok = writer.Text(claim.id.Value(), limits::kDigestIdLength);
  if (!ok) return ok;
  ok = writer.Text(claim.source.Value(), limits::kMaxSourceIdLength);
  if (!ok) return ok;
  ok = writer.Text(claim.publisher.Value(), limits::kMaxPublisherIdLength);
  if (!ok) return ok;
  ok = writer.Text(claim.scope.Value(), limits::kMaxAuthorityScopeIdLength);
  if (!ok) return ok;
  ok = writer.Text(claim.worker_boot.Value(), 64);
  if (!ok) return ok;
  ok = writer.Text(claim.attempt.Value(), limits::kMaxAttemptIdLength);
  if (!ok) return ok;
  ok = writer.Text(claim.publication.Value(), limits::kMaxPublicationIdLength);
  if (!ok) return ok;
  writer.U64(claim.epoch.Value());
  writer.U64(claim.source_generation.Value());
  writer.U64(claim.evidence_generation.Value());
  writer.U8(ProvenanceRank(claim.provenance));
  writer.U8(static_cast<std::uint8_t>(claim.source_class));
  writer.U8(static_cast<std::uint8_t>(claim.durability));
  writer.U8(static_cast<std::uint8_t>(claim.coverage));
  writer.U8(static_cast<std::uint8_t>(claim.state));
  writer.U8(static_cast<std::uint8_t>(claim.currentness));
  claim.value.Encode(writer);
  ok = writer.Text(claim.firmware_version.Value(), limits::kMaxVersionTokenLength);
  if (!ok) return ok;
  ok = writer.Text(claim.driver_version.Value(), limits::kMaxVersionTokenLength);
  if (!ok) return ok;
  return writer.Text(claim.reason.Value(), limits::kMaxTextLength);
}

Outcome<internal::StoredClaim> ReadClaim(ByteReader& reader) {
  internal::StoredClaim claim;
  auto capability = reader.Text(limits::kMaxCapabilityIdLength);
  if (!capability) return capability.GetError();
  auto parsed_capability = CapabilityId::Parse(capability.Value());
  if (!parsed_capability) return parsed_capability.GetError();
  claim.capability = parsed_capability.Value();
  auto id = reader.Text(limits::kDigestIdLength);
  if (!id) return id.GetError();
  auto parsed_id = EvidenceId::Parse(id.Value());
  if (!parsed_id) return parsed_id.GetError();
  claim.id = parsed_id.Value();
  auto source = reader.Text(limits::kMaxSourceIdLength);
  if (!source) return source.GetError();
  auto parsed_source = SourceId::Parse(source.Value());
  if (!parsed_source) return parsed_source.GetError();
  claim.source = parsed_source.Value();
  auto publisher = reader.Text(limits::kMaxPublisherIdLength);
  if (!publisher) return publisher.GetError();
  auto parsed_publisher = PublisherId::Parse(publisher.Value());
  if (!parsed_publisher) return parsed_publisher.GetError();
  claim.publisher = parsed_publisher.Value();
  auto scope = reader.Text(limits::kMaxAuthorityScopeIdLength);
  if (!scope) return scope.GetError();
  auto parsed_scope = AuthorityScopeId::Parse(scope.Value());
  if (!parsed_scope) return parsed_scope.GetError();
  claim.scope = parsed_scope.Value();
  auto boot = reader.Text(64);
  if (!boot) return boot.GetError();
  auto parsed_boot = WorkerBootId::Parse(boot.Value());
  if (!parsed_boot) return parsed_boot.GetError();
  claim.worker_boot = parsed_boot.Value();
  auto attempt = reader.Text(limits::kMaxAttemptIdLength);
  if (!attempt) return attempt.GetError();
  auto parsed_attempt = MutationAttemptId::Parse(attempt.Value());
  if (!parsed_attempt) return parsed_attempt.GetError();
  claim.attempt = parsed_attempt.Value();
  auto publication = reader.Text(limits::kMaxPublicationIdLength);
  if (!publication) return publication.GetError();
  auto parsed_publication = PublicationId::Parse(publication.Value());
  if (!parsed_publication) return parsed_publication.GetError();
  claim.publication = parsed_publication.Value();
  auto epoch = reader.U64();
  if (!epoch) return epoch.GetError();
  claim.epoch = CoordinatorEpoch::FromValue(epoch.Value());
  auto source_generation = reader.U64();
  if (!source_generation) return source_generation.GetError();
  claim.source_generation = SourceGeneration::FromValue(source_generation.Value());
  auto evidence_generation = reader.U64();
  if (!evidence_generation) return evidence_generation.GetError();
  claim.evidence_generation = EvidenceGeneration::FromValue(evidence_generation.Value());
  auto provenance = reader.U8();
  if (!provenance) return provenance.GetError();
  if (provenance.Value() >= kProvenanceClassCount) {
    return Outcome<internal::StoredClaim>::Failure(ErrorCode::PersistenceCorrupt,
                                                   "stored provenance class is out of range");
  }
  claim.provenance = static_cast<ProvenanceClass>(provenance.Value());
  auto source_class = reader.U8();
  if (!source_class) return source_class.GetError();
  if (source_class.Value() >= kEvidenceSourceClassCount) {
    return Outcome<internal::StoredClaim>::Failure(ErrorCode::PersistenceCorrupt,
                                                   "stored evidence source class is out of range");
  }
  claim.source_class = static_cast<EvidenceSourceClass>(source_class.Value());
  auto durability = reader.U8();
  if (!durability) return durability.GetError();
  if (durability.Value() > static_cast<std::uint8_t>(DurabilityClass::Durable)) {
    return Outcome<internal::StoredClaim>::Failure(ErrorCode::PersistenceCorrupt,
                                                   "stored durability class is out of range");
  }
  claim.durability = static_cast<DurabilityClass>(durability.Value());
  auto coverage = reader.U8();
  if (!coverage) return coverage.GetError();
  if (coverage.Value() > static_cast<std::uint8_t>(Coverage::FullEnumeration)) {
    return Outcome<internal::StoredClaim>::Failure(ErrorCode::PersistenceCorrupt,
                                                   "stored coverage class is out of range");
  }
  claim.coverage = static_cast<Coverage>(coverage.Value());
  auto state = reader.U8();
  if (!state) return state.GetError();
  if (state.Value() > static_cast<std::uint8_t>(CapabilityState::Conflicted)) {
    return Outcome<internal::StoredClaim>::Failure(ErrorCode::PersistenceCorrupt,
                                                   "stored capability state is out of range");
  }
  claim.state = static_cast<CapabilityState>(state.Value());
  auto currentness = reader.U8();
  if (!currentness) return currentness.GetError();
  if (currentness.Value() > static_cast<std::uint8_t>(EvidenceCurrentness::EntityGenerationSuperseded)) {
    return Outcome<internal::StoredClaim>::Failure(ErrorCode::PersistenceCorrupt,
                                                   "stored currentness class is out of range");
  }
  claim.currentness = static_cast<EvidenceCurrentness>(currentness.Value());
  auto value = CapabilityValue::Decode(reader);
  if (!value) return value.GetError();
  claim.value = value.Value();
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

Outcome<void> WriteEntity(ByteWriter& writer, const internal::StoredEntity& entity) {
  auto ok = writer.Text(entity.id.ToString(), limits::kMaxEntityIdLength);
  if (!ok) return ok;
  writer.U64(entity.current_generation.Value());
  writer.U64(entity.bound_generation.Value());
  writer.Bool(entity.has_set);
  writer.U64(entity.set_generation.Value());
  writer.Bool(entity.invalidated);
  ok = writer.Text(entity.invalidation_reason.Value(), limits::kMaxTextLength);
  if (!ok) return ok;
  writer.U32(static_cast<std::uint32_t>(entity.capabilities.size()));
  for (const internal::StoredCapability& capability : entity.capabilities) {
    ok = writer.Text(capability.id.ToString(), limits::kMaxCapabilityIdLength);
    if (!ok) return ok;
    writer.U64(capability.generation.Value());
  }
  writer.U32(static_cast<std::uint32_t>(entity.claims.size()));
  for (const internal::StoredClaim& claim : entity.claims) {
    ok = WriteClaim(writer, claim);
    if (!ok) return ok;
  }
  writer.U32(static_cast<std::uint32_t>(entity.source_floors.size()));
  for (const internal::StoredSourceFloor& floor : entity.source_floors) {
    ok = writer.Text(floor.source.Value(), limits::kMaxSourceIdLength);
    if (!ok) return ok;
    ok = writer.Text(floor.worker_boot.Value(), 64);
    if (!ok) return ok;
    writer.U64(floor.generation.Value());
  }
  writer.U32(static_cast<std::uint32_t>(entity.history.size()));
  for (const internal::StoredGeneration& generation : entity.history) {
    writer.U64(generation.generation.Value());
    writer.U64(generation.set_generation.Value());
    WriteDigest(writer, generation.digest);
    writer.U64(generation.capability_count);
    writer.U64(generation.supported_count);
    writer.U64(generation.retired_at.Value());
    ok = writer.Text(generation.reason.Value(), limits::kMaxTextLength);
    if (!ok) return ok;
  }
  return Status::Success();
}

Outcome<internal::StoredEntity> ReadEntity(ByteReader& reader) {
  internal::StoredEntity entity;
  auto id = reader.Text(limits::kMaxEntityIdLength);
  if (!id) return id.GetError();
  auto parsed_id = EntityId::Parse(id.Value());
  if (!parsed_id) return parsed_id.GetError();
  entity.id = parsed_id.Value();
  auto current = reader.U64();
  if (!current) return current.GetError();
  if (current.Value() == 0) {
    return Outcome<internal::StoredEntity>::Failure(ErrorCode::PersistenceCorrupt,
                                                    "stored entity has no current generation");
  }
  entity.current_generation = EntityGeneration::FromValue(current.Value());
  auto bound = reader.U64();
  if (!bound) return bound.GetError();
  entity.bound_generation = EntityGeneration::FromValue(bound.Value());
  auto has_set = reader.Bool();
  if (!has_set) return has_set.GetError();
  entity.has_set = has_set.Value();
  auto set_generation = reader.U64();
  if (!set_generation) return set_generation.GetError();
  entity.set_generation = CapabilitySetGeneration::FromValue(set_generation.Value());
  auto invalidated = reader.Bool();
  if (!invalidated) return invalidated.GetError();
  entity.invalidated = invalidated.Value();
  auto reason = reader.Text(limits::kMaxTextLength);
  if (!reason) return reason.GetError();
  if (!reason.Value().empty()) {
    auto parsed = ReasonToken::Parse(reason.Value());
    if (!parsed) return parsed.GetError();
    entity.invalidation_reason = parsed.Value();
  }

  auto capability_count = reader.U32();
  if (!capability_count) return capability_count.GetError();
  if (capability_count.Value() > limits::kMaxCapabilitiesPerEntity) {
    return Outcome<internal::StoredEntity>::Failure(
        ErrorCode::PersistenceLimitExceeded, "stored entity exceeds the capability bound");
  }
  for (std::size_t index = 0; index < capability_count.Value(); ++index) {
    auto capability_id = reader.Text(limits::kMaxCapabilityIdLength);
    if (!capability_id) return capability_id.GetError();
    auto parsed_capability = CapabilityId::Parse(capability_id.Value());
    if (!parsed_capability) return parsed_capability.GetError();
    auto generation = reader.U64();
    if (!generation) return generation.GetError();
    internal::StoredCapability capability;
    capability.id = parsed_capability.Value();
    capability.generation = CapabilityGeneration::FromValue(generation.Value());
    entity.capabilities.push_back(std::move(capability));
  }
  std::sort(entity.capabilities.begin(), entity.capabilities.end(),
            [](const internal::StoredCapability& lhs, const internal::StoredCapability& rhs) {
              return lhs.id < rhs.id;
            });
  for (std::size_t index = 1; index < entity.capabilities.size(); ++index) {
    if (entity.capabilities[index - 1].id == entity.capabilities[index].id) {
      return Outcome<internal::StoredEntity>::Failure(ErrorCode::PersistenceDuplicateRecord,
                                                      "stored entity declares one capability twice",
                                                      entity.capabilities[index].id.ToString());
    }
  }

  auto claim_count = reader.U32();
  if (!claim_count) return claim_count.GetError();
  if (claim_count.Value() > limits::kMaxEvidencePerEntity) {
    return Outcome<internal::StoredEntity>::Failure(ErrorCode::PersistenceLimitExceeded,
                                                    "stored entity exceeds the evidence bound");
  }
  for (std::size_t index = 0; index < claim_count.Value(); ++index) {
    auto claim = ReadClaim(reader);
    if (!claim) return claim.GetError();
    entity.claims.push_back(std::move(claim.Value()));
  }

  auto floor_count = reader.U32();
  if (!floor_count) return floor_count.GetError();
  if (floor_count.Value() > limits::kMaxEvidencePerEntity) {
    return Outcome<internal::StoredEntity>::Failure(ErrorCode::PersistenceLimitExceeded,
                                                    "stored entity exceeds the source floor bound");
  }
  for (std::size_t index = 0; index < floor_count.Value(); ++index) {
    auto source = reader.Text(limits::kMaxSourceIdLength);
    if (!source) return source.GetError();
    auto parsed_source = SourceId::Parse(source.Value());
    if (!parsed_source) return parsed_source.GetError();
    auto boot = reader.Text(64);
    if (!boot) return boot.GetError();
    auto parsed_boot = WorkerBootId::Parse(boot.Value());
    if (!parsed_boot) return parsed_boot.GetError();
    auto generation = reader.U64();
    if (!generation) return generation.GetError();
    internal::StoredSourceFloor floor;
    floor.source = parsed_source.Value();
    floor.worker_boot = parsed_boot.Value();
    floor.generation = SourceGeneration::FromValue(generation.Value());
    entity.source_floors.push_back(std::move(floor));
  }

  auto history_count = reader.U32();
  if (!history_count) return history_count.GetError();
  if (history_count.Value() > limits::kMaxEntityGenerationsRetained) {
    return Outcome<internal::StoredEntity>::Failure(ErrorCode::PersistenceLimitExceeded,
                                                    "stored entity exceeds the history bound");
  }
  for (std::size_t index = 0; index < history_count.Value(); ++index) {
    auto generation = reader.U64();
    if (!generation) return generation.GetError();
    auto history_set_generation = reader.U64();
    if (!history_set_generation) return history_set_generation.GetError();
    auto digest = ReadDigest(reader);
    if (!digest) return digest.GetError();
    auto capability_count_value = reader.U64();
    if (!capability_count_value) return capability_count_value.GetError();
    auto supported_count = reader.U64();
    if (!supported_count) return supported_count.GetError();
    auto retired_at = reader.U64();
    if (!retired_at) return retired_at.GetError();
    auto history_reason = reader.Text(limits::kMaxTextLength);
    if (!history_reason) return history_reason.GetError();
    internal::StoredGeneration stored;
    stored.generation = EntityGeneration::FromValue(generation.Value());
    stored.set_generation = CapabilitySetGeneration::FromValue(history_set_generation.Value());
    stored.digest = digest.Value();
    stored.capability_count = capability_count_value.Value();
    stored.supported_count = supported_count.Value();
    stored.retired_at = RegistryGeneration::FromValue(retired_at.Value());
    if (!history_reason.Value().empty()) {
      auto parsed = ReasonToken::Parse(history_reason.Value());
      if (!parsed) return parsed.GetError();
      stored.reason = parsed.Value();
    }
    entity.history.push_back(std::move(stored));
  }
  return entity;
}

Outcome<void> WriteGrant(ByteWriter& writer, const AuthorityGrant& grant) {
  auto ok = writer.Text(grant.scope.Value(), limits::kMaxAuthorityScopeIdLength);
  if (!ok) return ok;
  writer.U32(static_cast<std::uint32_t>(grant.entity_kinds.size()));
  for (const FabricEntityKind kind : grant.entity_kinds) writer.U8(static_cast<std::uint8_t>(kind));
  writer.U32(static_cast<std::uint32_t>(grant.namespaces.size()));
  for (const CapabilityNamespaceId& ns : grant.namespaces) {
    ok = writer.Text(ns.Value(), limits::kMaxCapabilityIdLength);
    if (!ok) return ok;
  }
  writer.U8(grant.modes);
  writer.Bool(grant.exclusive);
  writer.Bool(grant.may_publish_durable);
  writer.U8(ProvenanceRank(grant.strongest_provenance));
  writer.U32(static_cast<std::uint32_t>(grant.max_claims_per_publication));
  ok = writer.Text(grant.description.Value(), limits::kMaxTextLength);
  if (!ok) return ok;
  writer.U32(static_cast<std::uint32_t>(grant.allowed_publishers.size()));
  for (const PublisherId& publisher : grant.allowed_publishers) {
    ok = writer.Text(publisher.Value(), limits::kMaxPublisherIdLength);
    if (!ok) return ok;
  }
  return Status::Success();
}

Outcome<AuthorityGrant> ReadGrant(ByteReader& reader) {
  AuthorityGrant grant;
  auto scope = reader.Text(limits::kMaxAuthorityScopeIdLength);
  if (!scope) return scope.GetError();
  auto parsed_scope = AuthorityScopeId::Parse(scope.Value());
  if (!parsed_scope) return parsed_scope.GetError();
  grant.scope = parsed_scope.Value();
  auto kind_count = reader.U32();
  if (!kind_count) return kind_count.GetError();
  if (kind_count.Value() > limits::kMaxAuthorityEntityKinds) {
    return Outcome<AuthorityGrant>::Failure(ErrorCode::PersistenceLimitExceeded,
                                            "stored authority declares too many entity classes");
  }
  for (std::size_t index = 0; index < kind_count.Value(); ++index) {
    auto kind = reader.U8();
    if (!kind) return kind.GetError();
    if (kind.Value() == 0 || kind.Value() > static_cast<std::uint8_t>(FabricEntityKind::Endpoint)) {
      return Outcome<AuthorityGrant>::Failure(ErrorCode::PersistenceCorrupt,
                                              "stored authority declares an unknown entity class");
    }
    grant.entity_kinds.push_back(static_cast<FabricEntityKind>(kind.Value()));
  }
  auto namespace_count = reader.U32();
  if (!namespace_count) return namespace_count.GetError();
  if (namespace_count.Value() > limits::kMaxAuthorityNamespaces) {
    return Outcome<AuthorityGrant>::Failure(ErrorCode::PersistenceLimitExceeded,
                                            "stored authority declares too many namespaces");
  }
  for (std::size_t index = 0; index < namespace_count.Value(); ++index) {
    auto ns = reader.Text(limits::kMaxCapabilityIdLength);
    if (!ns) return ns.GetError();
    auto parsed = CapabilityNamespaceId::Parse(ns.Value());
    if (!parsed) return parsed.GetError();
    grant.namespaces.push_back(parsed.Value());
  }
  auto modes = reader.U8();
  if (!modes) return modes.GetError();
  grant.modes = modes.Value();
  auto exclusive = reader.Bool();
  if (!exclusive) return exclusive.GetError();
  grant.exclusive = exclusive.Value();
  auto durable = reader.Bool();
  if (!durable) return durable.GetError();
  grant.may_publish_durable = durable.Value();
  auto provenance = reader.U8();
  if (!provenance) return provenance.GetError();
  if (provenance.Value() >= kProvenanceClassCount) {
    return Outcome<AuthorityGrant>::Failure(ErrorCode::PersistenceCorrupt,
                                            "stored authority declares an unknown provenance");
  }
  grant.strongest_provenance = static_cast<ProvenanceClass>(provenance.Value());
  auto max_claims = reader.U32();
  if (!max_claims) return max_claims.GetError();
  if (max_claims.Value() > limits::kMaxClaimsPerPublication) {
    return Outcome<AuthorityGrant>::Failure(ErrorCode::PersistenceCorrupt,
                                            "stored authority claim bound is out of range");
  }
  grant.max_claims_per_publication = max_claims.Value();
  auto description = reader.Text(limits::kMaxTextLength);
  if (!description) return description.GetError();
  if (!description.Value().empty()) {
    auto parsed = ReasonToken::Parse(description.Value());
    if (!parsed) return parsed.GetError();
    grant.description = parsed.Value();
  }
  auto publisher_count = reader.U32();
  if (!publisher_count) return publisher_count.GetError();
  if (publisher_count.Value() > 4096) {
    return Outcome<AuthorityGrant>::Failure(ErrorCode::PersistenceLimitExceeded,
                                            "stored authority allowlist is too large");
  }
  for (std::size_t index = 0; index < publisher_count.Value(); ++index) {
    auto publisher = reader.Text(limits::kMaxPublisherIdLength);
    if (!publisher) return publisher.GetError();
    auto parsed = PublisherId::Parse(publisher.Value());
    if (!parsed) return parsed.GetError();
    grant.allowed_publishers.push_back(parsed.Value());
  }
  return grant;
}

Outcome<std::vector<std::byte>> EncodePayload(const internal::StorePayload& payload) {
  std::vector<std::byte> bytes;
  bytes.reserve(256 + payload.entities.size() * 256);

  const auto begin_record = [&bytes](std::uint8_t type) {
    ByteWriter writer(bytes);
    writer.U8(type);
    return bytes.size();
  };
  const auto finish_record = [&bytes](std::size_t length_offset) {
    const std::size_t end = bytes.size();
    const std::size_t body = end - length_offset - 4;
    bytes[length_offset] = static_cast<std::byte>(body & 0xFFu);
    bytes[length_offset + 1] = static_cast<std::byte>((body >> 8) & 0xFFu);
    bytes[length_offset + 2] = static_cast<std::byte>((body >> 16) & 0xFFu);
    bytes[length_offset + 3] = static_cast<std::byte>((body >> 24) & 0xFFu);
    return Status::Success();
  };

  {
    const std::size_t length_offset = begin_record(kRecordMeta) + 0;  // reserve below
    // Reserve the four length bytes explicitly.
    bytes.resize(bytes.size() + 4);
    ByteWriter writer(bytes);
    writer.U64(payload.epoch.Value());
    writer.U64(payload.registry_generation.Value());
    finish_record(length_offset);
  }
  for (const AuthorityGrant& grant : payload.authorities) {
    const std::size_t length_offset = begin_record(kRecordAuthority);
    bytes.resize(bytes.size() + 4);
    ByteWriter writer(bytes);
    auto ok = WriteGrant(writer, grant);
    if (!ok) return ok.GetError();
    finish_record(length_offset);
  }
  for (const internal::StoredFence& fence : payload.fences) {
    const std::size_t length_offset = begin_record(kRecordFence);
    bytes.resize(bytes.size() + 4);
    ByteWriter writer(bytes);
    auto boot = writer.Text(fence.worker_boot.Value(), 64);
    if (!boot) return boot.GetError();
    auto publisher = writer.Text(fence.publisher.Value(), limits::kMaxPublisherIdLength);
    if (!publisher) return publisher.GetError();
    auto reason = writer.Text(fence.reason.Value(), limits::kMaxTextLength);
    if (!reason) return reason.GetError();
    finish_record(length_offset);
  }
  for (const internal::StoredEntity& entity : payload.entities) {
    const std::size_t length_offset = begin_record(kRecordEntity);
    bytes.resize(bytes.size() + 4);
    ByteWriter writer(bytes);
    auto ok = WriteEntity(writer, entity);
    if (!ok) return ok.GetError();
    finish_record(length_offset);
  }
  return bytes;
}

Outcome<internal::StorePayload> DecodePayload(std::span<const std::byte> bytes) {
  internal::StorePayload payload;
  ByteReader reader(bytes);
  bool saw_meta = false;
  while (!reader.Empty()) {
    auto type = reader.U8();
    if (!type) return type.GetError();
    auto length = reader.U32();
    if (!length) return length.GetError();
    if (length.Value() > reader.Remaining()) {
      return Outcome<internal::StorePayload>::Failure(
          ErrorCode::PersistenceCorrupt, "store record length exceeds the remaining payload",
          std::to_string(length.Value()));
    }
    const std::span<const std::byte> body = bytes.subspan(reader.offset(), length.Value());
    ByteReader record(body);
    switch (type.Value()) {
      case kRecordMeta: {
        if (saw_meta) {
          return Outcome<internal::StorePayload>::Failure(ErrorCode::PersistenceDuplicateRecord,
                                                          "store declares more than one meta record");
        }
        saw_meta = true;
        auto epoch = record.U64();
        if (!epoch) return epoch.GetError();
        auto generation = record.U64();
        if (!generation) return generation.GetError();
        payload.epoch = CoordinatorEpoch::FromValue(epoch.Value());
        payload.registry_generation = RegistryGeneration::FromValue(generation.Value());
        break;
      }
      case kRecordAuthority: {
        if (payload.authorities.size() >= 4096) {
          return Outcome<internal::StorePayload>::Failure(ErrorCode::PersistenceLimitExceeded,
                                                          "store exceeds the authority bound");
        }
        auto grant = ReadGrant(record);
        if (!grant) return grant.GetError();
        payload.authorities.push_back(std::move(grant.Value()));
        break;
      }
      case kRecordFence: {
        if (payload.fences.size() >= limits::kMaxFencedWorkerBoots) {
          return Outcome<internal::StorePayload>::Failure(ErrorCode::PersistenceLimitExceeded,
                                                          "store exceeds the fence bound");
        }
        auto boot = record.Text(64);
        if (!boot) return boot.GetError();
        auto parsed_boot = WorkerBootId::Parse(boot.Value());
        if (!parsed_boot) return parsed_boot.GetError();
        auto publisher = record.Text(limits::kMaxPublisherIdLength);
        if (!publisher) return publisher.GetError();
        internal::StoredFence fence;
        fence.worker_boot = parsed_boot.Value();
        if (!publisher.Value().empty()) {
          auto parsed_publisher = PublisherId::Parse(publisher.Value());
          if (!parsed_publisher) return parsed_publisher.GetError();
          fence.publisher = parsed_publisher.Value();
        }
        auto reason = record.Text(limits::kMaxTextLength);
        if (!reason) return reason.GetError();
        if (!reason.Value().empty()) {
          auto parsed_reason = ReasonToken::Parse(reason.Value());
          if (!parsed_reason) return parsed_reason.GetError();
          fence.reason = parsed_reason.Value();
        }
        payload.fences.push_back(std::move(fence));
        break;
      }
      case kRecordEntity: {
        if (payload.entities.size() >= limits::kMaxEntities) {
          return Outcome<internal::StorePayload>::Failure(ErrorCode::PersistenceLimitExceeded,
                                                          "store exceeds the entity bound");
        }
        auto entity = ReadEntity(record);
        if (!entity) return entity.GetError();
        payload.entities.push_back(std::move(entity.Value()));
        break;
      }
      default:
        return Outcome<internal::StorePayload>::Failure(ErrorCode::PersistenceCorrupt,
                                                        "store contains an unknown record type",
                                                        std::to_string(type.Value()));
    }
    auto end = record.ExpectEnd();
    if (!end) return end.GetError();
    reader = ByteReader(bytes.subspan(reader.offset() + length.Value()));
  }
  if (!saw_meta) {
    return Outcome<internal::StorePayload>::Failure(ErrorCode::PersistenceCorrupt,
                                                    "store payload has no meta record");
  }
  return payload;
}

struct StoreHeader {
  std::uint32_t format_version = 0;
  std::uint32_t flags = 0;
  std::uint64_t payload_bytes = 0;
  std::uint64_t record_count = 0;
  Digest payload_digest;
  std::size_t header_bytes = kStoreHeaderBytes;
};

Outcome<StoreHeader> ParseHeader(std::span<const std::byte> file) {
  if (file.size() < kStoreHeaderBytes) {
    return Outcome<StoreHeader>::Failure(ErrorCode::PersistenceCorrupt,
                                         "store file is shorter than its header",
                                         std::to_string(file.size()));
  }
  const auto* raw = reinterpret_cast<const unsigned char*>(file.data());
  if (std::memcmp(raw, kStoreMagic.data(), kStoreMagic.size()) != 0) {
    return Outcome<StoreHeader>::Failure(ErrorCode::PersistenceFormatInvalid,
                                         "store file magic does not match");
  }
  const auto read_u32 = [raw](std::size_t offset) {
    std::uint32_t value = 0;
    for (int index = 0; index < 4; ++index) {
      value |= static_cast<std::uint32_t>(raw[offset + static_cast<std::size_t>(index)])
               << (8 * index);
    }
    return value;
  };
  const auto read_u64 = [raw](std::size_t offset) {
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index) {
      value |= static_cast<std::uint64_t>(raw[offset + static_cast<std::size_t>(index)])
               << (8 * index);
    }
    return value;
  };
  const std::uint32_t stored_crc = read_u32(64);
  const std::uint32_t computed_crc = Crc32(file.first(64));
  if (stored_crc != computed_crc) {
    return Outcome<StoreHeader>::Failure(ErrorCode::PersistenceIntegrityFailure,
                                         "store header CRC does not match");
  }
  StoreHeader header;
  header.format_version = read_u32(8);
  header.flags = read_u32(12);
  header.payload_bytes = read_u64(16);
  header.record_count = read_u64(24);
  std::array<std::byte, kDigestBytes> digest_bytes{};
  for (std::size_t index = 0; index < kDigestBytes; ++index) {
    digest_bytes[index] = file[32 + index];
  }
  header.payload_digest = Digest(digest_bytes);
  if (header.format_version != kFormatVersion) {
    return Outcome<StoreHeader>::Failure(ErrorCode::PersistenceVersionUnsupported,
                                         "store format version is not supported",
                                         std::to_string(header.format_version));
  }
  return header;
}

std::filesystem::path TemporaryPathFor(const std::filesystem::path& target,
                                       const PersistenceConfig& config) {
  const std::string base = NarrowPath(target);
  return std::filesystem::path(base + ".tmp-" + config.file_stem);
}

}  // namespace

Outcome<void> ValidatePersistenceConfig(const PersistenceConfig& config) {
  if (config.directory.empty()) {
    return Status::Failure(ErrorCode::PersistencePathInvalid,
                           "persistence directory must be configured");
  }
  auto stem = ValidateStem(config.file_stem);
  if (!stem) return stem.GetError();
  std::error_code error;
  std::filesystem::path directory = std::filesystem::absolute(config.directory, error);
  if (error) {
    return Status::Failure(ErrorCode::PersistencePathInvalid,
                           "persistence directory cannot be resolved", error.message());
  }
  directory = directory.lexically_normal();
  const std::string text = NarrowPath(directory);
  if (text.size() > limits::kPersistencePathLength) {
    return Status::Failure(ErrorCode::PersistencePathInvalid,
                           "persistence directory path is too long",
                           std::to_string(text.size()));
  }
  if (!std::filesystem::exists(directory, error)) {
    if (!config.create_if_missing) {
      return Status::Failure(ErrorCode::PersistenceIoFailure,
                             "persistence directory does not exist and creation is disabled",
                             text);
    }
    std::filesystem::create_directories(directory, error);
    if (error) {
      return Status::Failure(ErrorCode::PersistenceIoFailure,
                             "persistence directory cannot be created", error.message());
    }
  } else if (!std::filesystem::is_directory(directory, error)) {
    return Status::Failure(ErrorCode::PersistencePathInvalid,
                           "persistence path is not a directory", text);
  }
  return Status::Success();
}

Outcome<std::filesystem::path> StorePath(const PersistenceConfig& config) {
  auto valid = ValidatePersistenceConfig(config);
  if (!valid) return valid.GetError();
  std::error_code error;
  std::filesystem::path directory = std::filesystem::absolute(config.directory, error);
  if (error) {
    return Outcome<std::filesystem::path>::Failure(ErrorCode::PersistencePathInvalid,
                                                   "persistence directory cannot be resolved");
  }
  return directory.lexically_normal() / (config.file_stem + std::string(kStoreSuffix));
}

namespace internal {

Outcome<SaveReport> WriteStore(const PersistenceConfig& config, const StorePayload& payload) {
  auto path = StorePath(config);
  if (!path.HasValue()) return path.GetError();
  auto encoded = EncodePayload(payload);
  if (!encoded.HasValue()) return encoded.GetError();
  const std::vector<std::byte>& body = encoded.Value();
  if (body.size() > limits::kMaxPersistenceBytes) {
    return Outcome<SaveReport>::Failure(ErrorCode::PersistenceLimitExceeded,
                                        "encoded store exceeds the persistence bound",
                                        std::to_string(body.size()));
  }
  const Digest digest = ComputeDigest(body);

  std::vector<std::byte> file;
  file.reserve(kStoreHeaderBytes + body.size());
  for (const char ch : kStoreMagic) file.push_back(static_cast<std::byte>(ch));
  ByteWriter writer(file);
  writer.U32(kFormatVersion);
  writer.U32(kFlagsNone);
  writer.U64(body.size());
  writer.U64(payload.entities.size());
  for (const std::byte value : digest.Bytes()) writer.U8(static_cast<std::uint8_t>(value));
  writer.U32(Crc32(std::span<const std::byte>(file.data(), 64)));
  file.insert(file.end(), body.begin(), body.end());

  auto written = WriteFileAtomic(path.Value(), file, config.durable_flush,
                                 TemporaryPathFor(path.Value(), config));
  if (!written) return written.GetError();

  SaveReport report;
  report.path = path.Value();
  report.bytes = file.size();
  report.entity_records = payload.entities.size();
  for (const StoredEntity& entity : payload.entities) {
    report.evidence_records += entity.claims.size();
    report.retired_generations += entity.history.size();
  }
  report.fence_records = payload.fences.size();
  report.authority_grants = payload.authorities.size();
  report.payload_digest = digest;
  report.file_digest = ComputeDigest(file);
  return report;
}

Outcome<LoadReport> ReadStore(const PersistenceConfig& config, StorePayload& payload) {
  auto path = StorePath(config);
  if (!path.HasValue()) return path.GetError();
  std::error_code error;
  if (!std::filesystem::exists(path.Value(), error)) {
    if (!config.create_if_missing) {
      return Outcome<LoadReport>::Failure(ErrorCode::PersistenceIoFailure,
                                          "store file does not exist", NarrowPath(path.Value()));
    }
    LoadReport empty;
    empty.created_empty = true;
    empty.epoch = CoordinatorEpoch::FromValue(1);
    return empty;
  }
  auto bytes = ReadFileBounded(path.Value(), limits::kMaxPersistenceBytes);
  if (!bytes.HasValue()) return bytes.GetError();
  const std::vector<std::byte>& file = bytes.Value();
  auto header = ParseHeader(file);
  if (!header.HasValue()) return header.GetError();
  const StoreHeader& parsed = header.Value();
  if (parsed.payload_bytes > file.size() ||
      file.size() - kStoreHeaderBytes != parsed.payload_bytes) {
    return Outcome<LoadReport>::Failure(ErrorCode::PersistenceCorrupt,
                                        "store payload length does not match the file size",
                                        std::to_string(parsed.payload_bytes));
  }
  const std::span<const std::byte> body(file.data() + kStoreHeaderBytes,
                                        static_cast<std::size_t>(parsed.payload_bytes));
  const Digest digest = ComputeDigest(body);
  if (!(digest == parsed.payload_digest)) {
    return Outcome<LoadReport>::Failure(ErrorCode::PersistenceIntegrityFailure,
                                        "store payload digest does not match the header");
  }
  auto decoded = DecodePayload(body);
  if (!decoded.HasValue()) return decoded.GetError();
  payload = std::move(decoded.Value());

  LoadReport report;
  report.epoch = payload.epoch;
  report.registry_generation = payload.registry_generation;
  report.store_digest = digest;
  report.authority_grants = payload.authorities.size();
  report.fenced_worker_boots = payload.fences.size();
  return report;
}

}  // namespace internal

Outcome<StoreInspection> InspectStore(const PersistenceConfig& config) {
  auto path = StorePath(config);
  if (!path.HasValue()) return path.GetError();
  StoreInspection inspection;
  inspection.path = path.Value();
  auto bytes = ReadFileBounded(path.Value(), limits::kMaxPersistenceBytes);
  if (!bytes.HasValue()) return bytes.GetError();
  const std::vector<std::byte>& file = bytes.Value();
  auto header = ParseHeader(file);
  if (!header.HasValue()) {
    if (header.Code() == ErrorCode::PersistenceVersionUnsupported) {
      inspection.integrity_ok = true;
      return inspection;
    }
    return header.GetError();
  }
  inspection.format_version = header.Value().format_version;
  inspection.flags = header.Value().flags;
  inspection.payload_bytes = header.Value().payload_bytes;
  inspection.payload_digest = header.Value().payload_digest;
  if (header.Value().payload_bytes != file.size() - kStoreHeaderBytes) {
    return Outcome<StoreInspection>::Failure(
        ErrorCode::PersistenceCorrupt, "store payload length does not match the file size");
  }
  const std::span<const std::byte> body(file.data() + kStoreHeaderBytes,
                                        static_cast<std::size_t>(header.Value().payload_bytes));
  if (!(ComputeDigest(body) == inspection.payload_digest)) {
    return Outcome<StoreInspection>::Failure(ErrorCode::PersistenceIntegrityFailure,
                                             "store payload digest does not match the header");
  }
  auto decoded = DecodePayload(body);
  if (!decoded.HasValue()) return decoded.GetError();
  inspection.integrity_ok = true;
  inspection.entity_records = decoded.Value().entities.size();
  for (const internal::StoredEntity& entity : decoded.Value().entities) {
    inspection.evidence_records += entity.claims.size();
    inspection.retired_generations += entity.history.size();
  }
  inspection.fence_records = decoded.Value().fences.size();
  inspection.authority_grants = decoded.Value().authorities.size();
  inspection.registry_generation = decoded.Value().registry_generation;
  inspection.epoch = decoded.Value().epoch;
  return inspection;
}

}  // namespace fabric::capability
