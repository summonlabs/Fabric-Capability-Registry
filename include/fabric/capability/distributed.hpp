// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "fabric/capability/persistence.hpp"
#include "fabric/capability/publication.hpp"
#include "fabric/capability/registry.hpp"

namespace fabric::capability {

// ---------------------------------------------------------------------------
// Distributed capability publication over framed loopback TCP.
//
// The transport carries explicit, versioned, bounded frames. Raw C++
// structures are never written to a socket; every message is encoded field by
// field with bounds checked readers on the receiving side.
// ---------------------------------------------------------------------------

/// Frame layout documentation (also rendered by the CLI):
///
///   offset  size  field
///   0       4     magic "FCRF"
///   4       2     protocol version
///   6       2     message type
///   8       4     flags
///   12      8     request id
///   20      4     payload length
///   24      ...   payload
///   24+n    4     CRC-32 of the header prefix (magic through payload length) and the payload
///
/// A frame with trailing bytes after the declared payload and CRC is
/// rejected. The payload length is bounded by limits::kMaxFrameBytes.
inline constexpr std::uint16_t kProtocolVersion = 1;
inline constexpr std::size_t kFrameHeaderBytes = 24;

enum class WireMessageType : std::uint16_t {
  Hello = 1,
  HelloAck = 2,
  Publish = 3,
  PublishAck = 4,
  Fence = 5,
  FenceAck = 6,
  Status = 7,
  StatusAck = 8,
  Goodbye = 9,
  ErrorResponse = 10,
};

std::string_view WireMessageTypeName(WireMessageType type) noexcept;

/// A decoded frame: header fields plus the validated payload range.
struct WireFrame {
  std::uint16_t version = 0;
  WireMessageType type = WireMessageType::Hello;
  std::uint32_t flags = 0;
  std::uint64_t request_id = 0;
  std::vector<std::byte> payload;
};

Outcome<std::vector<std::byte>> EncodeFrame(WireMessageType type, std::uint32_t flags,
                                            std::uint64_t request_id,
                                            std::span<const std::byte> payload);
/// Decodes exactly one frame. Rejects short frames, unknown versions, unknown
/// message types, oversized payloads, CRC mismatches and trailing bytes.
Outcome<WireFrame> DecodeFrame(std::span<const std::byte> bytes);

/// Deterministic payload codecs used by the transport and by adversarial
/// codec tests.
Outcome<std::vector<std::byte>> EncodePublicationRequest(const PublicationRequest& request);
Outcome<PublicationRequest> DecodePublicationRequest(std::span<const std::byte> payload);
Outcome<std::vector<std::byte>> EncodePublicationResult(const PublicationResult& result);
Outcome<PublicationResult> DecodePublicationResult(std::span<const std::byte> payload);

/// Payload of a Hello message: the publisher identity a connection claims.
struct HelloPayload {
  PublisherId publisher;
  AuthorityScopeId scope;
  SourceId source;
  WorkerBootId worker_boot;
  CoordinatorEpoch epoch;
  std::vector<CapabilityNamespaceId> namespaces;
};

Outcome<std::vector<std::byte>> EncodeHello(const HelloPayload& hello);
Outcome<HelloPayload> DecodeHello(std::span<const std::byte> payload);

/// Hello response: the authoritative epoch plus acceptance decision.
struct HelloAckPayload {
  bool accepted = false;
  CoordinatorEpoch epoch;
  RegistryGeneration registry_generation;
  std::string message;
};

Outcome<std::vector<std::byte>> EncodeHelloAck(const HelloAckPayload& ack);
Outcome<HelloAckPayload> DecodeHelloAck(std::span<const std::byte> payload);

/// Status request/response: lets a client learn the exact entity and
/// capability set generation it must expect before publishing.
struct StatusPayload {
  EntityId entity;
  EntityGeneration entity_generation;
  CapabilitySetGeneration set_generation;
  RegistryGeneration registry_generation;
  CoordinatorEpoch epoch;
};

Outcome<std::vector<std::byte>> EncodeStatus(const StatusPayload& status);
Outcome<StatusPayload> DecodeStatus(std::span<const std::byte> payload);

struct FencePayload {
  WorkerBootId worker_boot;
  ReasonToken reason;
};

Outcome<std::vector<std::byte>> EncodeFence(const FencePayload& fence);
Outcome<FencePayload> DecodeFence(std::span<const std::byte> payload);

struct ErrorPayload {
  ErrorCode code = ErrorCode::Ok;
  std::string message;
};

Outcome<std::vector<std::byte>> EncodeErrorPayload(const ErrorPayload& error);
Outcome<ErrorPayload> DecodeErrorPayload(std::span<const std::byte> payload);

struct CoordinatorOptions {
  std::string bind_address = "127.0.0.1";
  std::uint16_t port = 0;
  std::size_t max_connections = 16;
  std::size_t max_connects_per_second = 256;
  bool persist_on_start = true;
  bool persist_on_stop = true;
  bool persist_on_mutation = false;
  PersistenceConfig persistence;
  CapabilityRegistry::Options registry_options;
  /// Advance the coordinator epoch on start. Required for a real coordinator
  /// restart proof; disabled only for tests that explicitly want to observe
  /// stale epoch rejection on the same epoch.
  bool advance_epoch_on_start = true;
};

struct CoordinatorStats {
  std::uint64_t connections_accepted = 0;
  std::uint64_t connections_rejected = 0;
  std::uint64_t frames_received = 0;
  std::uint64_t frames_rejected = 0;
  std::uint64_t publications_committed = 0;
  std::uint64_t publications_idempotent = 0;
  std::uint64_t publications_rejected = 0;
  std::uint64_t publishers_fenced = 0;
  std::uint64_t duplicate_requests_replayed = 0;
};

/// Multi-threaded capability coordinator. One thread accepts connections; one
/// worker thread serves each connection. Publication authority is bound to the
/// connection lifetime: when a connection dies the publisher's worker boot is
/// fenced and its process bound evidence becomes non-current.
class CapabilityCoordinator {
 public:
  explicit CapabilityCoordinator(CoordinatorOptions options);
  ~CapabilityCoordinator();
  CapabilityCoordinator(const CapabilityCoordinator&) = delete;
  CapabilityCoordinator& operator=(const CapabilityCoordinator&) = delete;

  /// Loads durable state, advances the coordinator epoch, binds the listener.
  Outcome<void> Start();
  /// Stops serving, closes connections, optionally persists. Idempotent and
  /// safe to call from a signal handler thread; never self-joins.
  void Stop();
  bool Running() const;

  std::uint16_t BoundPort() const;
  std::string BoundAddress() const;
  CoordinatorEpoch Epoch() const;
  CoordinatorStats Stats() const;

  /// Read-only access for in-process tests and the CLI.
  CapabilityRegistry& Registry();
  const CapabilityRegistry& Registry() const;

  /// Persists the current registry state using the configured store.
  Outcome<SaveReport> Persist();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

struct PublisherClientOptions {
  std::string host = "127.0.0.1";
  std::uint16_t port = 0;
  PublisherId publisher;
  AuthorityScopeId scope;
  SourceId source;
  WorkerBootId worker_boot;
  std::vector<CapabilityNamespaceId> namespaces;
  std::size_t connect_attempts = 40;
  std::uint32_t connect_backoff_ms = 25;
  /// Epoch the client believes is current. A stale epoch is rejected by the
  /// coordinator; the client can read the authoritative epoch from Status.
  CoordinatorEpoch epoch;
};

/// Capability publisher used by worker processes and by tests.
class PublisherClient {
 public:
  explicit PublisherClient(PublisherClientOptions options);
  ~PublisherClient();
  PublisherClient(const PublisherClient&) = delete;
  PublisherClient& operator=(const PublisherClient&) = delete;

  /// Connects, performs the Hello handshake and learns the current epoch.
  Outcome<void> Connect();
  bool Connected() const;
  CoordinatorEpoch Epoch() const;

  Outcome<PublicationResult> Publish(const PublicationRequest& request);
  Outcome<void> Fence(const WorkerBootId& worker_boot, ReasonToken reason);
  /// Closes the connection. Idempotent; never blocks forever.
  void Close();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace fabric::capability
