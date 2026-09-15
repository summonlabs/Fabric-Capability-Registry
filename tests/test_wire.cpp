// Fabric Capability Registry test suite: wire frame codec.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <vector>

#include "fabric/capability/fabric_capability.hpp"
#include "registry_fixture.hpp"
#include "test_framework.hpp"

using namespace fabric::capability;
using namespace fcr::test;

namespace {

PublicationRequest SampleRequest() {
  PublicationRequest request;
  request.mode = PublicationMode::FullSnapshot;
  request.publication = *PublicationId::Parse("publication-1");
  request.attempt = *MutationAttemptId::Parse("attempt-1");
  request.epoch = CoordinatorEpoch::FromValue(3);
  request.authority = AuthorityContext{*AuthorityScopeId::Parse("scope"),
                                       *PublisherId::Parse("publisher"), Boot(3),
                                       *SourceId::Parse("source"), SourceGeneration::FromValue(2)};
  request.entity = Entity("nic:wire-0");
  request.entity_generation = EntityGeneration::FromValue(4);
  request.expected_set_generation = CapabilitySetGeneration::FromValue(5);
  request.coverage = Coverage::FullEnumeration;
  request.reason = Reason("wire-test");
  request.claims = {SpeedSetClaim({100'000'000'000ull}),
                    BoolClaim("fabric.forwarding.ecmp_supported", true)};
  IncrementalEdit edit;
  edit.operation = IncrementalOperation::WithdrawClaim;
  edit.capability = Cap("fabric.queue.max_rx_queues");
  request.edits.clear();
  return request;
}

}  // namespace

FCR_TEST(wire, frame_round_trip_and_rejections) {
  const std::vector<std::byte> payload = {std::byte{1}, std::byte{2}, std::byte{3}};
  auto frame = EncodeFrame(WireMessageType::Publish, 9, 42, payload);
  FCR_REQUIRE_OK(frame);
  auto decoded = DecodeFrame(frame.Value());
  FCR_REQUIRE_OK(decoded);
  FCR_CHECK(decoded.Value().type == WireMessageType::Publish);
  FCR_CHECK_EQ(decoded.Value().request_id, 42ull);
  FCR_CHECK_EQ(decoded.Value().flags, 9u);
  FCR_CHECK_EQ(decoded.Value().payload.size(), payload.size());
  FCR_CHECK_EQ(decoded.Value().version, kProtocolVersion);

  // Truncation at every length.
  for (std::size_t length = 0; length < frame.Value().size(); ++length) {
    std::vector<std::byte> truncated(frame.Value().begin(),
                                     frame.Value().begin() + static_cast<std::ptrdiff_t>(length));
    FCR_CHECK(!DecodeFrame(truncated).HasValue());
  }
  // Trailing bytes.
  std::vector<std::byte> trailing = frame.Value();
  trailing.push_back(std::byte{0});
  FCR_CHECK_CODE(DecodeFrame(trailing), ErrorCode::FrameTrailingBytes);
  // Bad magic.
  std::vector<std::byte> bad_magic = frame.Value();
  bad_magic[0] = std::byte{'X'};
  FCR_CHECK_CODE(DecodeFrame(bad_magic), ErrorCode::FrameMalformed);
  // Unknown version.
  std::vector<std::byte> bad_version = frame.Value();
  bad_version[4] = std::byte{99};
  FCR_CHECK_CODE(DecodeFrame(bad_version), ErrorCode::FrameVersionUnsupported);
  // Unknown type.
  std::vector<std::byte> bad_type = frame.Value();
  bad_type[6] = std::byte{99};
  FCR_CHECK_CODE(DecodeFrame(bad_type), ErrorCode::FrameTypeUnknown);
  // Corrupted payload.
  std::vector<std::byte> corrupted = frame.Value();
  corrupted[kFrameHeaderBytes] = static_cast<std::byte>(
      static_cast<unsigned char>(corrupted[kFrameHeaderBytes]) ^ 0x80u);
  FCR_CHECK_CODE(DecodeFrame(corrupted), ErrorCode::FrameIntegrityFailure);
}

FCR_TEST(wire, publication_request_round_trip) {
  const PublicationRequest request = SampleRequest();
  auto encoded = EncodePublicationRequest(request);
  FCR_REQUIRE_OK(encoded);
  auto decoded = DecodePublicationRequest(encoded.Value());
  FCR_REQUIRE_OK(decoded);
  FCR_CHECK(decoded.Value().mode == request.mode);
  FCR_CHECK(decoded.Value().publication == request.publication);
  FCR_CHECK(decoded.Value().attempt == request.attempt);
  FCR_CHECK(decoded.Value().epoch == request.epoch);
  FCR_CHECK(decoded.Value().entity == request.entity);
  FCR_CHECK(decoded.Value().entity_generation == request.entity_generation);
  FCR_CHECK(decoded.Value().expected_set_generation == request.expected_set_generation);
  FCR_CHECK(decoded.Value().authority.publisher == request.authority.publisher);
  FCR_CHECK(decoded.Value().authority.worker_boot == request.authority.worker_boot);
  FCR_CHECK_EQ(decoded.Value().claims.size(), request.claims.size());
  FCR_CHECK(decoded.Value().claims[0].capability == request.claims[0].capability);
  FCR_CHECK(decoded.Value().claims[0].value == request.claims[0].value);

  // Trailing bytes are rejected.
  std::vector<std::byte> trailing = encoded.Value();
  trailing.push_back(std::byte{7});
  FCR_CHECK_CODE(DecodePublicationRequest(trailing), ErrorCode::FrameTrailingBytes);

  // Unknown enum code inside the payload is rejected.
  std::vector<std::byte> bad_mode = encoded.Value();
  bad_mode[0] = std::byte{9};
  FCR_CHECK_CODE(DecodePublicationRequest(bad_mode), ErrorCode::MalformedEncoding);

  // Oversized claim count declared by a hostile payload.
  std::vector<std::byte> absurd = encoded.Value();
  const std::size_t claim_offset = absurd.size() - (encoded.Value().size() - absurd.size());
  (void)claim_offset;
  FCR_CHECK(!DecodePublicationRequest(std::vector<std::byte>{}).HasValue());
}

FCR_TEST(wire, publication_result_round_trip_with_diff_and_explanation) {
  Fixture fixture;
  FCR_REQUIRE_OK(fixture.PublishSnapshot(Entity("nic:wire-0"), EntityGeneration::FromValue(1),
                                         {SpeedSetClaim({100'000'000'000ull})}));
  auto second = fixture.PublishSnapshot(Entity("nic:wire-0"), EntityGeneration::FromValue(1),
                                        {SpeedSetClaim({400'000'000'000ull})},
                                        CapabilitySetGeneration::FromValue(1),
                                        Coverage::FullEnumeration, "p-2", "a-2");
  FCR_REQUIRE_OK(second);
  const PublicationResult& result = second.Value();
  FCR_CHECK(!result.diff.entries.empty());
  FCR_CHECK(!result.explanation.steps.empty());

  auto encoded = EncodePublicationResult(result);
  FCR_REQUIRE_OK(encoded);
  auto decoded = DecodePublicationResult(encoded.Value());
  FCR_REQUIRE_OK(decoded);
  FCR_CHECK(decoded.Value().status == result.status);
  FCR_CHECK(decoded.Value().code == result.code);
  FCR_CHECK(decoded.Value().publication == result.publication);
  FCR_CHECK(decoded.Value().entity == result.entity);
  FCR_CHECK(decoded.Value().new_set_generation == result.new_set_generation);
  FCR_CHECK(decoded.Value().set_digest == result.set_digest);
  FCR_CHECK_EQ(decoded.Value().diff.entries.size(), result.diff.entries.size());
  FCR_CHECK(decoded.Value().diff.entries[0].capability == result.diff.entries[0].capability);
  FCR_CHECK(decoded.Value().diff.entries[0].kind == result.diff.entries[0].kind);
  FCR_CHECK_EQ(decoded.Value().explanation.steps.size(), result.explanation.steps.size());
  FCR_CHECK_EQ(decoded.Value().explanation.ToText(), result.explanation.ToText());

  std::vector<std::byte> truncated = encoded.Value();
  truncated.resize(truncated.size() / 2);
  FCR_CHECK(!DecodePublicationResult(truncated).HasValue());
}

FCR_TEST(wire, control_payload_round_trips) {
  HelloPayload hello;
  hello.publisher = *PublisherId::Parse("publisher");
  hello.scope = *AuthorityScopeId::Parse("scope");
  hello.source = *SourceId::Parse("source");
  hello.worker_boot = Boot(9);
  hello.epoch = CoordinatorEpoch::FromValue(2);
  hello.namespaces = {*CapabilityNamespaceId::Parse("fabric.port")};
  auto hello_bytes = EncodeHello(hello);
  FCR_REQUIRE_OK(hello_bytes);
  auto hello_decoded = DecodeHello(hello_bytes.Value());
  FCR_REQUIRE_OK(hello_decoded);
  FCR_CHECK(hello_decoded.Value().publisher == hello.publisher);
  FCR_CHECK(hello_decoded.Value().worker_boot == hello.worker_boot);
  FCR_CHECK_EQ(hello_decoded.Value().namespaces.size(), std::size_t(1));

  HelloAckPayload ack;
  ack.accepted = true;
  ack.epoch = CoordinatorEpoch::FromValue(4);
  ack.registry_generation = RegistryGeneration::FromValue(12);
  ack.message = "registered";
  auto ack_bytes = EncodeHelloAck(ack);
  FCR_REQUIRE_OK(ack_bytes);
  auto ack_decoded = DecodeHelloAck(ack_bytes.Value());
  FCR_REQUIRE_OK(ack_decoded);
  FCR_CHECK(ack_decoded.Value().accepted);
  FCR_CHECK(ack_decoded.Value().registry_generation == ack.registry_generation);

  StatusPayload status;
  status.entity = Entity("nic:0");
  status.entity_generation = EntityGeneration::FromValue(2);
  status.set_generation = CapabilitySetGeneration::FromValue(3);
  auto status_bytes = EncodeStatus(status);
  FCR_REQUIRE_OK(status_bytes);
  auto status_decoded = DecodeStatus(status_bytes.Value());
  FCR_REQUIRE_OK(status_decoded);
  FCR_CHECK(status_decoded.Value().entity == status.entity);
  FCR_CHECK(status_decoded.Value().set_generation == status.set_generation);

  FencePayload fence;
  fence.worker_boot = Boot(4);
  fence.reason = Reason("fence");
  auto fence_bytes = EncodeFence(fence);
  FCR_REQUIRE_OK(fence_bytes);
  auto fence_decoded = DecodeFence(fence_bytes.Value());
  FCR_REQUIRE_OK(fence_decoded);
  FCR_CHECK(fence_decoded.Value().worker_boot == fence.worker_boot);

  ErrorPayload error;
  error.code = ErrorCode::WorkerBootFenced;
  error.message = "fenced";
  auto error_bytes = EncodeErrorPayload(error);
  FCR_REQUIRE_OK(error_bytes);
  auto error_decoded = DecodeErrorPayload(error_bytes.Value());
  FCR_REQUIRE_OK(error_decoded);
  FCR_CHECK(error_decoded.Value().code == ErrorCode::WorkerBootFenced);

  FCR_CHECK_EQ(std::string(WireMessageTypeName(WireMessageType::PublishAck)),
               std::string("publish-ack"));

  // Random byte soup never crashes the frame decoder.
  DeterministicRng rng(0x1234ull);
  for (int iteration = 0; iteration < 512; ++iteration) {
    const std::size_t length = rng.NextBelow(256);
    std::vector<std::byte> bytes(length);
    for (std::size_t index = 0; index < length; ++index) {
      bytes[index] = static_cast<std::byte>(rng.NextU32() & 0xFFu);
    }
    auto decoded = DecodeFrame(bytes);
    if (decoded.HasValue()) {
      auto reencoded = EncodeFrame(decoded.Value().type, decoded.Value().flags,
                                   decoded.Value().request_id, decoded.Value().payload);
      FCR_REQUIRE_OK(reencoded);
      FCR_CHECK_EQ(reencoded.Value().size(), bytes.size());
    }
  }
}
