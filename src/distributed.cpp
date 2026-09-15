// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/capability/distributed.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "fabric/capability/encoding.hpp"
#include "fabric/capability/ids.hpp"
#include "fabric/capability/limits.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace fabric::capability {
namespace {

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;

void CloseSocket(SocketHandle socket) {
  if (socket == kInvalidSocket) return;
  shutdown(socket, SD_BOTH);
  closesocket(socket);
}

std::string LastSocketError() { return std::to_string(WSAGetLastError()); }

bool EnsureNetworkSubsystem() {
  static const bool ready = []() {
    WSADATA data{};
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return ready;
}

#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;

void CloseSocket(SocketHandle socket) {
  if (socket == kInvalidSocket) return;
  ::shutdown(socket, SHUT_RDWR);
  ::close(socket);
}

std::string LastSocketError() { return std::to_string(errno); }

bool EnsureNetworkSubsystem() { return true; }
#endif

ReasonToken MakeReason(std::string_view text) {
  auto parsed = ReasonToken::Parse(text);
  return parsed.HasValue() ? parsed.Value() : ReasonToken{};
}

Outcome<void> SendAll(SocketHandle socket, std::span<const std::byte> bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const std::size_t remaining = bytes.size() - offset;
    const int chunk = static_cast<int>(std::min<std::size_t>(remaining, 1u << 20));
#if defined(_WIN32)
    const int sent = send(socket, reinterpret_cast<const char*>(bytes.data() + offset), chunk, 0);
#else
    const int sent = static_cast<int>(
        ::send(socket, bytes.data() + offset, static_cast<std::size_t>(chunk), 0));
#endif
    if (sent <= 0) {
      return Status::Failure(ErrorCode::TransportFailure, "socket send failed", LastSocketError());
    }
    offset += static_cast<std::size_t>(sent);
  }
  return Status::Success();
}

Outcome<void> ReceiveAll(SocketHandle socket, std::span<std::byte> buffer) {
  std::size_t offset = 0;
  while (offset < buffer.size()) {
    const std::size_t remaining = buffer.size() - offset;
    const int chunk = static_cast<int>(std::min<std::size_t>(remaining, 1u << 20));
#if defined(_WIN32)
    const int received =
        recv(socket, reinterpret_cast<char*>(buffer.data() + offset), chunk, 0);
#else
    const int received =
        static_cast<int>(::recv(socket, buffer.data() + offset, static_cast<std::size_t>(chunk), 0));
#endif
    if (received == 0) {
      return Status::Failure(ErrorCode::TransportClosed, "peer closed the connection");
    }
    if (received < 0) {
      return Status::Failure(ErrorCode::TransportFailure, "socket receive failed",
                             LastSocketError());
    }
    offset += static_cast<std::size_t>(received);
  }
  return Status::Success();
}

/// Reads exactly one frame: header, payload and CRC are read in full before
/// any decode is attempted.
Outcome<WireFrame> ReceiveFrame(SocketHandle socket) {
  std::array<std::byte, kFrameHeaderBytes> header{};
  auto header_ok = ReceiveAll(socket, header);
  if (!header_ok) return header_ok.GetError();
  static const std::array<std::byte, 4> kMagic = {std::byte{'F'}, std::byte{'C'}, std::byte{'R'},
                                                  std::byte{'F'}};
  if (std::memcmp(header.data(), kMagic.data(), kMagic.size()) != 0) {
    return Outcome<WireFrame>::Failure(ErrorCode::FrameMalformed, "frame magic does not match");
  }
  ByteReader reader(std::span<const std::byte>(header.data() + 4, kFrameHeaderBytes - 4));
  auto version = reader.U16();
  if (!version) return version.GetError();
  auto type = reader.U16();
  if (!type) return type.GetError();
  auto flags = reader.U32();
  if (!flags) return flags.GetError();
  auto request_id = reader.U64();
  if (!request_id) return request_id.GetError();
  auto length = reader.U32();
  if (!length) return length.GetError();
  if (length.Value() > limits::kMaxFrameBytes) {
    return Outcome<WireFrame>::Failure(ErrorCode::FrameTooLarge,
                                       "declared frame payload exceeds the bound",
                                       std::to_string(length.Value()));
  }
  std::vector<std::byte> frame(kFrameHeaderBytes + length.Value() + 4);
  std::memcpy(frame.data(), header.data(), kFrameHeaderBytes);
  std::span<std::byte> rest(frame.data() + kFrameHeaderBytes, length.Value() + 4);
  auto body_ok = ReceiveAll(socket, rest);
  if (!body_ok) return body_ok.GetError();
  return DecodeFrame(frame);
}

Outcome<void> SendFrame(SocketHandle socket, WireMessageType type, std::uint64_t request_id,
                        std::span<const std::byte> payload) {
  auto frame = EncodeFrame(type, 0, request_id, payload);
  if (!frame.HasValue()) return frame.GetError();
  return SendAll(socket, frame.Value());
}

/// Deterministic worker boot identity derived from a caller supplied token and
/// process local entropy so that a restarted worker never reuses a boot.
WorkerBootId DeriveBoot(const WorkerBootId& requested) {
  if (requested.IsSet()) return requested;
  const std::uint64_t ticks = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  std::vector<std::byte> bytes;
  ByteWriter writer(bytes);
  writer.U64(ticks);
  writer.U64(static_cast<std::uint64_t>(
#if defined(_WIN32)
      GetCurrentProcessId()
#else
      static_cast<unsigned long>(::getpid())
#endif
      ));
  writer.U64(std::hash<std::thread::id>{}(std::this_thread::get_id()));
  const Digest digest = ComputeDigest(bytes);
  auto parsed = WorkerBootId::Parse(digest.ShortString());
  return parsed.HasValue() ? parsed.Value() : WorkerBootId{};
}

}  // namespace

// ---------------------------------------------------------------------------
// Coordinator
// ---------------------------------------------------------------------------

class CapabilityCoordinator::Impl {
 public:
  explicit Impl(CoordinatorOptions options)
      : options_(std::move(options)), registry_(options_.registry_options) {}

  Outcome<void> Start() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    if (running_) {
      return Status::Failure(ErrorCode::InvalidArgument, "the coordinator is already running");
    }
    if (!EnsureNetworkSubsystem()) {
      return Status::Failure(ErrorCode::TransportUnavailable,
                             "the network subsystem could not be initialised");
    }
    if (options_.max_connections == 0 || options_.max_connections > 4096) {
      return Status::Failure(ErrorCode::InvalidArgument,
                             "coordinator connection bound is out of range");
    }
    if (options_.bind_address.empty()) {
      return Status::Failure(ErrorCode::InvalidArgument, "coordinator bind address is required");
    }

    if (options_.persist_on_start && !options_.persistence.directory.empty()) {
      auto loaded = registry_.Load(options_.persistence);
      if (!loaded.HasValue()) return loaded.GetError();
      loaded_ = loaded.Value();
    }
    if (options_.advance_epoch_on_start) {
      auto advanced = registry_.AdvanceCoordinatorEpoch(MakeReason("coordinator-start"));
      if (!advanced.HasValue()) return advanced.GetError();
      if (options_.persist_on_start && !options_.persistence.directory.empty()) {
        // The epoch and the fences it created must be durable before the listener serves,
        // otherwise a crash would lose the fact that the previous epoch's boots are stale.
        auto saved = registry_.Save(options_.persistence);
        if (!saved.HasValue()) return saved.GetError();
      }
    }

    listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener_ == kInvalidSocket) {
      return Status::Failure(ErrorCode::TransportUnavailable, "listener socket could not be created",
                             LastSocketError());
    }
    const int reuse = 1;
    setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
               sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(options_.port);
    if (inet_pton(AF_INET, options_.bind_address.c_str(), &address.sin_addr) != 1) {
      CloseSocket(listener_);
      listener_ = kInvalidSocket;
      return Status::Failure(ErrorCode::InvalidArgument,
                             "coordinator bind address is not an IPv4 literal",
                             options_.bind_address);
    }
    if (bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
      CloseSocket(listener_);
      listener_ = kInvalidSocket;
      return Status::Failure(ErrorCode::TransportUnavailable, "listener bind failed",
                             LastSocketError());
    }
    if (listen(listener_, static_cast<int>(options_.max_connections)) != 0) {
      CloseSocket(listener_);
      listener_ = kInvalidSocket;
      return Status::Failure(ErrorCode::TransportUnavailable, "listener listen failed",
                             LastSocketError());
    }
    int length = sizeof(address);
    if (getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) == 0) {
      bound_port_ = ntohs(address.sin_port);
    }

    stopping_.store(false);
    running_ = true;
    accept_thread_ = std::thread([this]() { AcceptLoop(); });
    return Status::Success();
  }

  void Stop() {
    {
      std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
      if (!running_ && !stopping_.load()) {
        if (options_.persist_on_stop && !options_.persistence.directory.empty()) {
          registry_.Save(options_.persistence);
        }
        return;
      }
      if (stopping_.exchange(true)) return;
    }

    if (listener_ != kInvalidSocket) {
      CloseSocket(listener_);
      listener_ = kInvalidSocket;
    }
    std::vector<SocketHandle> sockets;
    {
      std::lock_guard<std::mutex> guard(connections_mutex_);
      for (const auto& entry : connections_) sockets.push_back(entry.first);
    }
    for (const SocketHandle socket : sockets) CloseSocket(socket);

    JoinThreads();

    running_ = false;
    if (options_.persist_on_stop && !options_.persistence.directory.empty()) {
      registry_.Save(options_.persistence);
    }
  }

  bool Running() const { return running_; }

  std::uint16_t BoundPort() const { return bound_port_; }

  /// Joins the accept thread and every worker thread. Never self-joins: a
  /// thread that stops the coordinator detaches itself instead.
  void JoinThreads() {
    const std::thread::id self = std::this_thread::get_id();
    std::vector<std::thread> accept;
    std::vector<std::thread> workers;
    {
      std::lock_guard<std::mutex> guard(threads_mutex_);
      if (accept_thread_.joinable()) accept.push_back(std::move(accept_thread_));
      for (std::thread& worker : worker_threads_) {
        if (worker.joinable()) workers.push_back(std::move(worker));
      }
      worker_threads_.clear();
    }
    for (std::thread& thread : accept) {
      if (thread.get_id() == self) {
        thread.detach();
      } else {
        thread.join();
      }
    }
    for (std::thread& thread : workers) {
      if (thread.get_id() == self) {
        thread.detach();
      } else {
        thread.join();
      }
    }
  }

  void AcceptLoop() {
    for (;;) {
      if (stopping_.load()) return;
      const SocketHandle listener = listener_;
      if (listener == kInvalidSocket) return;
      sockaddr_in peer{};
      int peer_length = sizeof(peer);
      const SocketHandle connection =
          accept(listener, reinterpret_cast<sockaddr*>(&peer), &peer_length);
      if (connection == kInvalidSocket) {
        if (stopping_.load()) return;
        continue;
      }
      {
        std::lock_guard<std::mutex> guard(connections_mutex_);
        if (connections_.size() >= options_.max_connections) {
          CloseSocket(connection);
          Bump(&CoordinatorStats::connections_rejected);
          continue;
        }
        connections_[connection] = ConnectionState{};
      }
      Bump(&CoordinatorStats::connections_accepted);
      std::lock_guard<std::mutex> guard(threads_mutex_);
      worker_threads_.emplace_back([this, connection]() { ServeConnection(connection); });
    }
  }

  struct ReplayCacheEntry {
    std::vector<std::byte> response;
  };

  struct ConnectionState {
    bool handshake_done = false;
    PublisherId publisher;
    AuthorityScopeId scope;
    SourceId source;
    WorkerBootId worker_boot;
    std::deque<std::uint64_t> replay_order;
    std::unordered_map<std::uint64_t, ReplayCacheEntry> replay;
  };

  void ServeConnection(SocketHandle connection) {
    WorkerBootId boot;
    PublisherId publisher;
    bool handshake_done = false;

    for (;;) {
      auto frame = ReceiveFrame(connection);
      if (!frame.HasValue()) {
        Bump(&CoordinatorStats::frames_rejected);
        break;
      }
      Bump(&CoordinatorStats::frames_received);
      const WireFrame& request = frame.Value();
      if (request.type == WireMessageType::Goodbye) break;

      if (request.type == WireMessageType::Hello) {
        auto payload = DecodeHello(request.payload);
        HelloAckPayload ack;
        if (!payload.HasValue()) {
          ack.accepted = false;
          ack.message = payload.GetError().ToString();
        } else if (payload.Value().epoch.IsSet() &&
                   !(payload.Value().epoch == registry_.CurrentEpoch())) {
          ack.accepted = false;
          ack.epoch = registry_.CurrentEpoch();
          ack.message = "coordinator epoch is stale";
        } else {
          auto registered = registry_.RegisterPublisher(payload.Value().publisher,
                                                        payload.Value().scope,
                                                        payload.Value().worker_boot);
          if (!registered.HasValue()) {
            ack.accepted = false;
            ack.epoch = registry_.CurrentEpoch();
            ack.message = registered.GetError().ToString();
          } else {
            ack.accepted = true;
            ack.epoch = registered.Value().epoch;
            ack.registry_generation = registered.Value().accepted_generation;
            ack.message = "registered";
          }
        }
        auto encoded = EncodeHelloAck(ack);
        if (!encoded.HasValue()) break;
        if (!SendFrame(connection, WireMessageType::HelloAck, request.request_id, encoded.Value())
                 .HasValue()) {
          break;
        }
        if (!ack.accepted) {
          ErrorPayload error;
          error.code = ErrorCode::CoordinatorEpochStale;
          if (payload.HasValue()) {
            if (registry_.IsWorkerBootFenced(payload.Value().worker_boot)) {
              error.code = ErrorCode::WorkerBootFenced;
            } else if (!registry_.Authority(payload.Value().scope).HasValue()) {
              error.code = ErrorCode::UnknownAuthorityScope;
            } else {
              error.code = ErrorCode::UnauthorizedPublisher;
            }
          } else {
            error.code = ErrorCode::FrameMalformed;
          }
          error.message = ack.message;
          auto encoded_error = EncodeErrorPayload(error);
          if (encoded_error.HasValue()) {
            SendFrame(connection, WireMessageType::ErrorResponse, request.request_id,
                      encoded_error.Value());
          }
          break;
        }
        if (payload.HasValue()) {
          handshake_done = true;
          publisher = payload.Value().publisher;
          boot = payload.Value().worker_boot;
          {
            std::lock_guard<std::mutex> guard(connections_mutex_);
            const auto state = connections_.find(connection);
            if (state != connections_.end()) {
              state->second.handshake_done = true;
              state->second.publisher = publisher;
              state->second.scope = payload.Value().scope;
              state->second.source = payload.Value().source;
              state->second.worker_boot = boot;
            }
          }
          std::lock_guard<std::mutex> guard(boots_mutex_);
          boot_scopes_[boot] = payload.Value().scope;
        }
        continue;
      }

      ConnectionState* state = nullptr;
      {
        std::lock_guard<std::mutex> guard(connections_mutex_);
        const auto found = connections_.find(connection);
        if (found == connections_.end()) break;
        state = &found->second;
      }
      if (!handshake_done || !state->handshake_done) {
        Bump(&CoordinatorStats::frames_rejected);
        break;
      }

      if (request.type == WireMessageType::Publish) {
        const auto cached = state->replay.find(request.request_id);
        if (cached != state->replay.end()) {
          Bump(&CoordinatorStats::duplicate_requests_replayed);
          if (!SendFrame(connection, WireMessageType::PublishAck, request.request_id,
                         cached->second.response)
                   .HasValue()) {
            break;
          }
          continue;
        }
        PublicationResult result;
        auto decoded = DecodePublicationRequest(request.payload);
        if (!decoded.HasValue()) {
          result.status = PublicationStatus::Rejected;
          result.code = decoded.GetError().code;
          result.message = decoded.GetError().ToString();
        } else {
          PublicationRequest publication = decoded.Value();
          if (!(publication.authority.publisher == state->publisher) ||
              !(publication.authority.worker_boot == state->worker_boot) ||
              !(publication.authority.scope == state->scope) ||
              !(publication.authority.source == state->source)) {
            result.status = PublicationStatus::Rejected;
            result.code = ErrorCode::UnauthorizedPublisher;
            result.message =
                "the publication identity does not match the registered connection identity";
          } else {
            publication.epoch = registry_.CurrentEpoch();
            auto outcome = registry_.Publish(publication);
            if (outcome.HasValue()) {
              result = outcome.Value();
            } else {
              result.status = PublicationStatus::Rejected;
              result.code = outcome.Code();
              result.message = outcome.GetError().ToString();
              result.publication = publication.publication;
              result.attempt = publication.attempt;
              result.entity = publication.entity;
              result.entity_generation = publication.entity_generation;
            }
          }
        }
        switch (result.status) {
          case PublicationStatus::Committed:
            Bump(&CoordinatorStats::publications_committed);
            break;
          case PublicationStatus::IdempotentReplay:
            Bump(&CoordinatorStats::publications_idempotent);
            break;
          case PublicationStatus::Rejected:
            Bump(&CoordinatorStats::publications_rejected);
            break;
        }
        auto encoded = EncodePublicationResult(result);
        if (!encoded.HasValue()) break;
        {
          std::lock_guard<std::mutex> guard(connections_mutex_);
          if (state->replay.size() >= limits::kMaxFrameRequestsInFlight) {
            const std::uint64_t oldest = state->replay_order.front();
            state->replay_order.pop_front();
            state->replay.erase(oldest);
          }
          state->replay_order.push_back(request.request_id);
          state->replay[request.request_id] = ReplayCacheEntry{encoded.Value()};
        }
        if (options_.persist_on_mutation && !options_.persistence.directory.empty()) {
          // Durability before acknowledgement: a client that is told "committed" must be able
          // to rely on the state surviving a coordinator crash, and a kill that lands between
          // the commit and the save must not be able to lose a fencing record.
          auto saved = registry_.Save(options_.persistence);
          if (!saved.HasValue()) {
            Bump(&CoordinatorStats::frames_rejected);
          }
        }
        if (!SendFrame(connection, WireMessageType::PublishAck, request.request_id,
                       encoded.Value())
                 .HasValue()) {
          break;
        }
        continue;
      }

      if (request.type == WireMessageType::Status) {
        StatusPayload status;
        status.epoch = registry_.CurrentEpoch();
        status.registry_generation = registry_.Generation();
        auto decoded = DecodeStatus(request.payload);
        if (decoded.HasValue() && decoded.Value().entity.IsSet()) {
          status.entity = decoded.Value().entity;
          auto record = registry_.EntityRecord(status.entity);
          if (record.HasValue()) {
            status.entity_generation = record.Value().current_generation;
            if (record.Value().has_current_set) {
              status.set_generation = record.Value().current_set.set_generation;
            }
          }
        }
        auto encoded = EncodeStatus(status);
        if (!encoded.HasValue()) break;
        if (!SendFrame(connection, WireMessageType::StatusAck, request.request_id, encoded.Value())
                 .HasValue()) {
          break;
        }
        continue;
      }

      if (request.type == WireMessageType::Fence) {
        auto decoded = DecodeFence(request.payload);
        ErrorPayload error;
        if (!decoded.HasValue()) {
          error.code = decoded.GetError().code;
          error.message = decoded.GetError().ToString();
        } else {
          bool allowed = false;
          {
            std::lock_guard<std::mutex> guard(boots_mutex_);
            const auto found = boot_scopes_.find(decoded.Value().worker_boot);
            allowed = found != boot_scopes_.end() && found->second == state->scope;
          }
          if (!allowed) {
            error.code = ErrorCode::UnauthorizedPublisher;
            error.message = "the fence target is not registered inside this authority scope";
          } else {
            auto fenced = registry_.FenceWorkerBoot(decoded.Value().worker_boot,
                                                    decoded.Value().reason);
            if (!fenced.HasValue()) {
              error.code = fenced.Code();
              error.message = fenced.GetError().ToString();
            } else {
              Bump(&CoordinatorStats::publishers_fenced);
              {
                std::lock_guard<std::mutex> guard(boots_mutex_);
                boot_scopes_.erase(decoded.Value().worker_boot);
              }
              if (!SendFrame(connection, WireMessageType::FenceAck, request.request_id, {})
                       .HasValue()) {
                break;
              }
              continue;
            }
          }
        }
        auto encoded = EncodeErrorPayload(error);
        if (!encoded.HasValue()) break;
        if (!SendFrame(connection, WireMessageType::ErrorResponse, request.request_id,
                       encoded.Value())
                 .HasValue()) {
          break;
        }
        continue;
      }

      Bump(&CoordinatorStats::frames_rejected);
      ErrorPayload error;
      error.code = ErrorCode::FrameTypeUnknown;
      error.message = "message type is not accepted in this connection state";
      auto encoded = EncodeErrorPayload(error);
      if (!encoded.HasValue()) break;
      if (!SendFrame(connection, WireMessageType::ErrorResponse, request.request_id, encoded.Value())
               .HasValue()) {
        break;
      }
    }

    if (handshake_done && boot.IsSet()) {
      registry_.FenceWorkerBoot(boot, MakeReason("connection-lost"));
      Bump(&CoordinatorStats::publishers_fenced);
      std::lock_guard<std::mutex> guard(boots_mutex_);
      boot_scopes_.erase(boot);
    }
    {
      std::lock_guard<std::mutex> guard(connections_mutex_);
      connections_.erase(connection);
    }
    CloseSocket(connection);
  }

  CoordinatorOptions options_;
  CapabilityRegistry registry_;
  LoadReport loaded_;

  std::mutex lifecycle_mutex_;
  std::atomic<bool> stopping_{false};
  bool running_ = false;
  std::uint16_t bound_port_ = 0;
  SocketHandle listener_ = kInvalidSocket;

  std::mutex connections_mutex_;
  std::unordered_map<SocketHandle, ConnectionState> connections_;

  std::mutex threads_mutex_;
  std::thread accept_thread_;
  std::vector<std::thread> worker_threads_;

  std::mutex boots_mutex_;
  std::unordered_map<WorkerBootId, AuthorityScopeId> boot_scopes_;

  std::mutex stats_mutex_;
  CoordinatorStats stats_;

  /// Coordinator statistics are read through Stats() under stats_mutex_, so every
  /// increment goes through the same mutex.
  void Bump(std::uint64_t CoordinatorStats::*field, std::uint64_t delta = 1) {
    std::lock_guard<std::mutex> guard(stats_mutex_);
    stats_.*field += delta;
  }
};

CapabilityCoordinator::CapabilityCoordinator(CoordinatorOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

CapabilityCoordinator::~CapabilityCoordinator() {
  if (impl_ != nullptr) impl_->Stop();
}

Outcome<void> CapabilityCoordinator::Start() { return impl_->Start(); }

void CapabilityCoordinator::Stop() { impl_->Stop(); }

bool CapabilityCoordinator::Running() const { return impl_->Running(); }

std::uint16_t CapabilityCoordinator::BoundPort() const { return impl_->BoundPort(); }

std::string CapabilityCoordinator::BoundAddress() const { return impl_->options_.bind_address; }

CoordinatorEpoch CapabilityCoordinator::Epoch() const { return impl_->registry_.CurrentEpoch(); }

CoordinatorStats CapabilityCoordinator::Stats() const {
  std::lock_guard<std::mutex> guard(impl_->stats_mutex_);
  return impl_->stats_;
}

CapabilityRegistry& CapabilityCoordinator::Registry() { return impl_->registry_; }

const CapabilityRegistry& CapabilityCoordinator::Registry() const { return impl_->registry_; }

Outcome<SaveReport> CapabilityCoordinator::Persist() {
  return impl_->registry_.Save(impl_->options_.persistence);
}

// ---------------------------------------------------------------------------
// Publisher client
// ---------------------------------------------------------------------------

class PublisherClient::Impl {
 public:
  explicit Impl(PublisherClientOptions options) : options_(std::move(options)) {
    if (!options_.worker_boot.IsSet()) options_.worker_boot = DeriveBoot(options_.worker_boot);
  }

  ~Impl() { Close(); }

  Outcome<void> Connect() {
    std::lock_guard<std::mutex> guard(mutex_);
    if (socket_ != kInvalidSocket) return Status::Success();
    if (!EnsureNetworkSubsystem()) {
      return Status::Failure(ErrorCode::TransportUnavailable,
                             "the network subsystem could not be initialised");
    }
    if (options_.port == 0 || options_.publisher.IsSet() == false || options_.scope.IsSet() == false ||
        options_.source.IsSet() == false) {
      return Status::Failure(ErrorCode::InvalidArgument,
                             "publisher, scope, source and port are required");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(options_.port);
    if (inet_pton(AF_INET, options_.host.c_str(), &address.sin_addr) != 1) {
      return Status::Failure(ErrorCode::InvalidArgument, "publisher host is not an IPv4 literal",
                             options_.host);
    }

    for (std::size_t attempt = 0; attempt < options_.connect_attempts; ++attempt) {
      SocketHandle candidate = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
      if (candidate == kInvalidSocket) {
        return Status::Failure(ErrorCode::TransportUnavailable, "client socket could not be created",
                               LastSocketError());
      }
      if (connect(candidate, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
        socket_ = candidate;
        break;
      }
      CloseSocket(candidate);
      std::this_thread::sleep_for(std::chrono::milliseconds(options_.connect_backoff_ms));
    }
    if (socket_ == kInvalidSocket) {
      return Status::Failure(ErrorCode::TransportUnavailable,
                             "the coordinator did not accept a connection",
                             options_.host + ":" + std::to_string(options_.port));
    }

    HelloPayload hello;
    hello.publisher = options_.publisher;
    hello.scope = options_.scope;
    hello.source = options_.source;
    hello.worker_boot = options_.worker_boot;
    hello.epoch = options_.epoch;
    hello.namespaces = options_.namespaces;
    auto encoded = EncodeHello(hello);
    if (!encoded.HasValue()) {
      CloseLocked();
      return encoded.GetError();
    }
    request_id_ = 1;
    auto sent = SendFrame(socket_, WireMessageType::Hello, request_id_, encoded.Value());
    if (!sent.HasValue()) {
      CloseLocked();
      return sent.GetError();
    }
    auto response = ReceiveFrame(socket_);
    if (!response.HasValue()) {
      CloseLocked();
      return response.GetError();
    }
    if (response.Value().type != WireMessageType::HelloAck) {
      CloseLocked();
      return Status::Failure(ErrorCode::FrameStateViolation,
                             "the coordinator did not answer the handshake");
    }
    auto ack = DecodeHelloAck(response.Value().payload);
    if (!ack.HasValue()) {
      CloseLocked();
      return ack.GetError();
    }
    if (!ack.Value().accepted) {
      ErrorCode code = ErrorCode::UnauthorizedPublisher;
      std::string message = ack.Value().message;
      auto error_frame = ReceiveFrame(socket_);
      if (error_frame.HasValue() && error_frame.Value().type == WireMessageType::ErrorResponse) {
        auto payload = DecodeErrorPayload(error_frame.Value().payload);
        if (payload.HasValue()) {
          code = payload.Value().code;
          message = payload.Value().message.empty() ? message : payload.Value().message;
        }
      }
      CloseLocked();
      return Status::Failure(code, message, options_.publisher.Value());
    }
    epoch_ = ack.Value().epoch;
    connected_ = true;
    return Status::Success();
  }

  void Close() {
    std::lock_guard<std::mutex> guard(mutex_);
    CloseLocked();
  }

  void CloseLocked() {
    if (socket_ != kInvalidSocket) {
      SendFrame(socket_, WireMessageType::Goodbye, request_id_, {});
      CloseSocket(socket_);
      socket_ = kInvalidSocket;
    }
    connected_ = false;
    generation_cache_.clear();
  }

  Outcome<PublicationResult> Publish(const PublicationRequest& request) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!connected_) {
      return Outcome<PublicationResult>::Failure(ErrorCode::TransportClosed,
                                                 "the publisher is not connected");
    }
    PublicationRequest outgoing = request;
    outgoing.epoch = epoch_;
    auto cached = generation_cache_.find(outgoing.entity);
    if (cached == generation_cache_.end() || cached->second.unknown) {
      auto refreshed = RefreshStatusLocked(outgoing.entity);
      if (!refreshed.HasValue()) return refreshed.GetError();
      cached = generation_cache_.find(outgoing.entity);
    }
    if (outgoing.expected_set_generation.Value() == 0 && cached != generation_cache_.end()) {
      outgoing.expected_set_generation = cached->second.set_generation;
    }
    auto encoded = EncodePublicationRequest(outgoing);
    if (!encoded.HasValue()) return encoded.GetError();
    ++request_id_;
    auto sent = SendFrame(socket_, WireMessageType::Publish, request_id_, encoded.Value());
    if (!sent.HasValue()) {
      CloseLocked();
      return sent.GetError();
    }
    auto response = ReceiveFrame(socket_);
    if (!response.HasValue()) {
      CloseLocked();
      return response.GetError();
    }
    if (response.Value().type != WireMessageType::PublishAck) {
      return Outcome<PublicationResult>::Failure(ErrorCode::FrameStateViolation,
                                                 "the coordinator did not answer the publication");
    }
    auto result = DecodePublicationResult(response.Value().payload);
    if (!result.HasValue()) return result.GetError();
    if (result.Value().code == ErrorCode::CapabilitySetGenerationStale ||
        result.Value().code == ErrorCode::CapabilitySetGenerationMismatch) {
      generation_cache_.erase(outgoing.entity);
    } else if (result.Value().status != PublicationStatus::Rejected) {
      GenerationCache entry;
      entry.set_generation = result.Value().new_set_generation;
      entry.unknown = false;
      generation_cache_[outgoing.entity] = entry;
    }
    return result.Value();
  }

  Outcome<void> Fence(const WorkerBootId& worker_boot, ReasonToken reason) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!connected_) {
      return Status::Failure(ErrorCode::TransportClosed, "the publisher is not connected");
    }
    FencePayload payload;
    payload.worker_boot = worker_boot;
    payload.reason = reason;
    auto encoded = EncodeFence(payload);
    if (!encoded.HasValue()) return encoded.GetError();
    ++request_id_;
    auto sent = SendFrame(socket_, WireMessageType::Fence, request_id_, encoded.Value());
    if (!sent.HasValue()) {
      CloseLocked();
      return sent.GetError();
    }
    auto response = ReceiveFrame(socket_);
    if (!response.HasValue()) {
      CloseLocked();
      return response.GetError();
    }
    if (response.Value().type == WireMessageType::FenceAck) return Status::Success();
    if (response.Value().type == WireMessageType::ErrorResponse) {
      auto error = DecodeErrorPayload(response.Value().payload);
      if (error.HasValue()) {
        return Status::Failure(error.Value().code, error.Value().message);
      }
    }
    return Status::Failure(ErrorCode::FrameStateViolation,
                           "the coordinator did not answer the fence request");
  }

  struct GenerationCache {
    CapabilitySetGeneration set_generation;
    bool unknown = false;
  };

  Outcome<void> RefreshStatusLocked(const EntityId& entity) {
    StatusPayload request;
    request.entity = entity;
    auto encoded = EncodeStatus(request);
    if (!encoded.HasValue()) return encoded.GetError();
    ++request_id_;
    auto sent = SendFrame(socket_, WireMessageType::Status, request_id_, encoded.Value());
    if (!sent.HasValue()) {
      CloseLocked();
      return sent.GetError();
    }
    auto response = ReceiveFrame(socket_);
    if (!response.HasValue()) {
      CloseLocked();
      return response.GetError();
    }
    if (response.Value().type != WireMessageType::StatusAck) {
      return Status::Failure(ErrorCode::FrameStateViolation,
                             "the coordinator did not answer the status request");
    }
    auto status = DecodeStatus(response.Value().payload);
    if (!status.HasValue()) return status.GetError();
    GenerationCache entry;
    entry.set_generation = status.Value().set_generation;
    entry.unknown = false;
    generation_cache_[entity] = entry;
    epoch_ = status.Value().epoch;
    return Status::Success();
  }

  PublisherClientOptions options_;
  mutable std::mutex mutex_;
  SocketHandle socket_ = kInvalidSocket;
  bool connected_ = false;
  std::uint64_t request_id_ = 0;
  CoordinatorEpoch epoch_;
  std::unordered_map<EntityId, GenerationCache> generation_cache_;
};

PublisherClient::PublisherClient(PublisherClientOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

PublisherClient::~PublisherClient() {
  if (impl_ != nullptr) impl_->Close();
}

Outcome<void> PublisherClient::Connect() { return impl_->Connect(); }

bool PublisherClient::Connected() const {
  std::lock_guard<std::mutex> guard(impl_->mutex_);
  return impl_->connected_;
}

CoordinatorEpoch PublisherClient::Epoch() const {
  std::lock_guard<std::mutex> guard(impl_->mutex_);
  return impl_->epoch_;
}

Outcome<PublicationResult> PublisherClient::Publish(const PublicationRequest& request) {
  return impl_->Publish(request);
}

Outcome<void> PublisherClient::Fence(const WorkerBootId& worker_boot, ReasonToken reason) {
  return impl_->Fence(worker_boot, reason);
}

void PublisherClient::Close() { impl_->Close(); }

}  // namespace fabric::capability
