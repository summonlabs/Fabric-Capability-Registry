// Fabric Capability Registry test suite: in-process distributed transport.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <memory>
#include <thread>
#include <vector>

#include "fabric/capability/fabric_capability.hpp"
#include "registry_fixture.hpp"
#include "test_framework.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace fabric::capability;
using namespace fcr::test;

namespace {

struct CoordinatorSetup {
  CoordinatorOptions options;
  AuthorityGrant grant;
  PublisherId publisher = *PublisherId::Parse("wire-publisher");
  PublisherId other_publisher = *PublisherId::Parse("other-publisher");
  WorkerBootId boot = Boot(21);
};

CoordinatorSetup Setup() {
  CoordinatorSetup setup;
  setup.options.port = 0;
  setup.options.advance_epoch_on_start = true;
  setup.grant.scope = *AuthorityScopeId::Parse("wire-scope");
  setup.grant.entity_kinds = {FabricEntityKind::Nic};
  setup.grant.namespaces = {*CapabilityNamespaceId::Parse("fabric.port"),
                            *CapabilityNamespaceId::Parse("fabric.forwarding")};
  setup.grant.modes = static_cast<std::uint8_t>(PublicationMode::FullSnapshot) |
                      static_cast<std::uint8_t>(PublicationMode::Incremental) |
                      static_cast<std::uint8_t>(PublicationMode::PartialObservation);
  setup.grant.strongest_provenance = ProvenanceClass::DirectHardwareEnumeration;
  setup.grant.allowed_publishers = {setup.publisher};
  return setup;
}

PublisherClientOptions ClientOptions(const CoordinatorSetup& setup, std::uint16_t port,
                                     const WorkerBootId& boot) {
  PublisherClientOptions options;
  options.port = port;
  options.publisher = setup.publisher;
  options.scope = setup.grant.scope;
  options.source = *SourceId::Parse("wire-source");
  options.worker_boot = boot;
  options.connect_attempts = 40;
  return options;
}

PublicationRequest Request(const CoordinatorSetup& setup, const WorkerBootId& boot,
                           const char* entity, const char* publication, const char* attempt,
                           std::vector<CapabilityClaim> claims,
                           CapabilitySetGeneration expected = {}) {
  PublicationRequest request;
  request.mode = PublicationMode::FullSnapshot;
  request.publication = *PublicationId::Parse(publication);
  request.attempt = *MutationAttemptId::Parse(attempt);
  request.authority = AuthorityContext{setup.grant.scope, setup.publisher, boot,
                                       *SourceId::Parse("wire-source"),
                                       SourceGeneration::FromValue(1)};
  request.entity = Entity(entity);
  request.entity_generation = EntityGeneration::FromValue(1);
  request.expected_set_generation = expected;
  request.coverage = Coverage::FullEnumeration;
  request.claims = std::move(claims);
  return request;
}

/// Raw socket helper used by the hostile connection cases.
class RawClient {
 public:
  explicit RawClient(std::uint16_t port) {
#if defined(_WIN32)
    WSADATA data{};
    WSAStartup(MAKEWORD(2, 2), &data);
    socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
    socket_ = socket(AF_INET, SOCK_STREAM, 0);
#endif
    if (socket_ == kInvalid) return;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (connect(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
      Close();
    }
  }

  ~RawClient() { Close(); }

  bool Connected() const { return socket_ != kInvalid; }

  void Send(std::span<const std::byte> bytes) {
    if (!Connected()) return;
#if defined(_WIN32)
    send(socket_, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0);
#else
    ::send(socket_, bytes.data(), bytes.size(), 0);
#endif
  }

  void Close() {
    if (socket_ == kInvalid) return;
#if defined(_WIN32)
    shutdown(socket_, SD_BOTH);
    closesocket(socket_);
#else
    ::shutdown(socket_, SHUT_RDWR);
    ::close(socket_);
#endif
    socket_ = kInvalid;
  }

 private:
#if defined(_WIN32)
  using Handle = SOCKET;
  static constexpr Handle kInvalid = INVALID_SOCKET;
#else
  using Handle = int;
  static constexpr Handle kInvalid = -1;
#endif
  Handle socket_ = kInvalid;
};

}  // namespace

FCR_TEST(distributed, handshake_authority_and_epoch) {
  CoordinatorSetup setup = Setup();
  CapabilityCoordinator coordinator(setup.options);
  FCR_REQUIRE_OK(coordinator.Start());
  FCR_CHECK(coordinator.Running());
  FCR_REQUIRE_OK(coordinator.Registry().DeclareAuthority(setup.grant));
  const std::uint16_t port = coordinator.BoundPort();
  FCR_CHECK(port != 0);

  // A publisher that is not on the allowlist is refused.
  PublisherClientOptions denied = ClientOptions(setup, port, Boot(22));
  denied.publisher = setup.other_publisher;
  PublisherClient denied_client(denied);
  auto denied_result = denied_client.Connect();
  FCR_CHECK(!denied_result.HasValue());
  FCR_CHECK(denied_result.Code() == ErrorCode::UnauthorizedPublisher);
  FCR_CHECK(!denied_client.Connected());

  PublisherClient client(ClientOptions(setup, port, setup.boot));
  FCR_REQUIRE_OK(client.Connect());
  FCR_CHECK(client.Connected());
  FCR_CHECK(client.Epoch() == coordinator.Epoch());

  auto committed = client.Publish(Request(setup, setup.boot, "nic:wire-0", "p-1", "a-1",
                                         {SpeedSetClaim({100'000'000'000ull})}));
  FCR_REQUIRE_OK(committed);
  FCR_CHECK(committed.Value().Committed());
  FCR_CHECK_EQ(committed.Value().new_set_generation.Value(), 1ull);

  auto query = coordinator.Registry().Query(Entity("nic:wire-0"), Cap("fabric.port.supported_speeds"));
  FCR_REQUIRE_OK(query);
  FCR_CHECK(query.Value().actionable);

  // Exact replay is idempotent and does not advance the set generation.
  auto replay = client.Publish(Request(setup, setup.boot, "nic:wire-0", "p-1", "a-1",
                                       {SpeedSetClaim({100'000'000'000ull})},
                                       CapabilitySetGeneration::FromValue(1)));
  FCR_REQUIRE_OK(replay);
  FCR_CHECK(replay.Value().Idempotent());
  FCR_CHECK_EQ(replay.Value().new_set_generation.Value(), 1ull);

  // A second publication: the client fills the current generation it learned, so the
  // caller does not have to track it.
  auto second = client.Publish(Request(setup, setup.boot, "nic:wire-0", "p-2", "a-2",
                                       {SpeedSetClaim({200'000'000'000ull})},
                                       CapabilitySetGeneration::FromValue(0)));
  FCR_REQUIRE_OK(second);
  FCR_CHECK(second.Value().Committed());
  FCR_CHECK_EQ(second.Value().new_set_generation.Value(), 2ull);

  // An explicitly stale expectation is never silently rewritten and is rejected.
  auto stale = client.Publish(Request(setup, setup.boot, "nic:wire-0", "p-3", "a-3",
                                      {SpeedSetClaim({400'000'000'000ull})},
                                      CapabilitySetGeneration::FromValue(1)));
  FCR_REQUIRE_OK(stale);
  FCR_CHECK(stale.Value().Rejected());
  FCR_CHECK(stale.Value().code == ErrorCode::CapabilitySetGenerationStale);
  client.Close();
  coordinator.Stop();
}

FCR_TEST(distributed, stale_epoch_clients_are_refused) {
  CoordinatorSetup setup = Setup();
  CapabilityCoordinator coordinator(setup.options);
  FCR_REQUIRE_OK(coordinator.Start());
  FCR_REQUIRE_OK(coordinator.Registry().DeclareAuthority(setup.grant));
  const std::uint16_t port = coordinator.BoundPort();

  PublisherClient client(ClientOptions(setup, port, setup.boot));
  FCR_REQUIRE_OK(client.Connect());
  const CoordinatorEpoch epoch = client.Epoch();
  client.Close();

  auto advanced = coordinator.Registry().AdvanceCoordinatorEpoch(Reason("restart"));
  FCR_REQUIRE_OK(advanced);

  PublisherClientOptions stale_options = ClientOptions(setup, port, setup.boot);
  stale_options.epoch = epoch;
  PublisherClient stale(stale_options);
  auto result = stale.Connect();
  FCR_CHECK(!result.HasValue());
  FCR_CHECK(result.Code() == ErrorCode::CoordinatorEpochStale ||
            result.Code() == ErrorCode::WorkerBootFenced);

  // A fresh boot with no epoch expectation is accepted under the new epoch: live authority
  // does not survive a restart, but a new instance can establish itself.
  PublisherClientOptions fresh_options = ClientOptions(setup, port, Boot(23));
  PublisherClient fresh_client(fresh_options);
  auto fresh_result = fresh_client.Connect();
  FCR_REQUIRE_OK(fresh_result);
  FCR_CHECK(fresh_client.Epoch() > epoch);
  coordinator.Stop();
}

FCR_TEST(distributed, hostile_connections_do_not_disturb_service) {
  CoordinatorSetup setup = Setup();
  CapabilityCoordinator coordinator(setup.options);
  FCR_REQUIRE_OK(coordinator.Start());
  FCR_REQUIRE_OK(coordinator.Registry().DeclareAuthority(setup.grant));
  const std::uint16_t port = coordinator.BoundPort();

  // Garbage bytes.
  {
    RawClient raw(port);
    FCR_REQUIRE(raw.Connected());
    const std::string garbage = "not a frame at all, just text";
    raw.Send(std::span<const std::byte>(reinterpret_cast<const std::byte*>(garbage.data()),
                                        garbage.size()));
    raw.Close();
  }
  // A well formed frame with a corrupted CRC.
  {
    RawClient raw(port);
    FCR_REQUIRE(raw.Connected());
    auto payload = EncodeHello(HelloPayload{setup.publisher, setup.grant.scope,
                                            *SourceId::Parse("wire-source"), Boot(31),
                                            CoordinatorEpoch{}, {}});
    FCR_REQUIRE_OK(payload);
    auto frame = EncodeFrame(WireMessageType::Hello, 0, 1, payload.Value());
    FCR_REQUIRE_OK(frame);
    std::vector<std::byte> corrupted = frame.Value();
    corrupted.back() = static_cast<std::byte>(static_cast<unsigned char>(corrupted.back()) ^ 0xFFu);
    raw.Send(corrupted);
    raw.Close();
  }
  // A frame that declares an oversized payload.
  {
    RawClient raw(port);
    FCR_REQUIRE(raw.Connected());
    std::vector<std::byte> header;
    ByteWriter writer(header);
    for (const char ch : std::string("FCRF")) writer.U8(static_cast<std::uint8_t>(ch));
    writer.U16(kProtocolVersion);
    writer.U16(static_cast<std::uint16_t>(WireMessageType::Publish));
    writer.U32(0);
    writer.U64(1);
    writer.U32(0xFFFFFF00u);
    raw.Send(header);
    raw.Close();
  }

  // The coordinator keeps serving legitimate clients.
  PublisherClient client(ClientOptions(setup, port, setup.boot));
  FCR_REQUIRE_OK(client.Connect());
  auto result = client.Publish(Request(setup, setup.boot, "nic:after-hostile", "p-1", "a-1",
                                      {SpeedSetClaim({400'000'000'000ull})}));
  FCR_REQUIRE_OK(result);
  FCR_CHECK(result.Value().Committed());
  FCR_CHECK_EQ(coordinator.Stats().frames_rejected >= 1, true);
  client.Close();
  coordinator.Stop();
}

FCR_TEST(distributed, concurrent_publishers_and_repeated_lifecycle) {
  CoordinatorSetup setup = Setup();
  CapabilityCoordinator coordinator(setup.options);
  FCR_REQUIRE_OK(coordinator.Start());
  FCR_REQUIRE_OK(coordinator.Registry().DeclareAuthority(setup.grant));
  const std::uint16_t port = coordinator.BoundPort();

  std::vector<std::unique_ptr<PublisherClient>> clients;
  for (int index = 0; index < 2; ++index) {
    // The publication identity must match the connection identity, so both clients use the
    // source the request helper stamps.
    PublisherClientOptions options =
        ClientOptions(setup, port, Boot(40 + static_cast<std::uint64_t>(index)));
    clients.push_back(std::make_unique<PublisherClient>(options));
  }
  FCR_REQUIRE_OK(clients[0]->Connect());
  FCR_REQUIRE_OK(clients[1]->Connect());

  std::vector<std::thread> workers;
  for (int index = 0; index < 2; ++index) {
    workers.emplace_back([&clients, &setup, index]() {
      const std::string entity = "nic:concurrent-" + std::to_string(index);
      const WorkerBootId boot = Boot(40 + static_cast<std::uint64_t>(index));
      auto result = clients[static_cast<std::size_t>(index)]->Publish(
          Request(setup, boot, entity.c_str(), "p-1", "a-1", {SpeedSetClaim({100'000'000'000ull})}));
      FCR_CHECK(result.HasValue());
      FCR_CHECK(result.Value().Committed());
    });
  }
  for (std::thread& worker : workers) worker.join();
  FCR_CHECK_EQ(coordinator.Registry().Entities().size(), std::size_t(2));

  // Killing a connection fences the boot and makes process evidence non-current.
  const WorkerBootId killed = Boot(40);
  clients[0]->Close();
  // The coordinator notices the closed connection and fences it.
  for (int attempt = 0; attempt < 200 && !coordinator.Registry().IsWorkerBootFenced(killed);
       ++attempt) {
    std::this_thread::yield();
  }
  FCR_CHECK(coordinator.Registry().IsWorkerBootFenced(killed));
  auto after = coordinator.Registry().Query(Entity("nic:concurrent-0"),
                                            Cap("fabric.port.supported_speeds"));
  FCR_REQUIRE_OK(after);
  FCR_CHECK(after.Value().state == CapabilityState::RevalidationRequired);

  clients[1]->Close();
  coordinator.Stop();

  // Repeated start and stop cycles must be safe.
  FCR_REQUIRE_OK(coordinator.Start());
  FCR_CHECK(coordinator.Running());
  coordinator.Stop();
  FCR_CHECK(!coordinator.Running());
  coordinator.Stop();
}
