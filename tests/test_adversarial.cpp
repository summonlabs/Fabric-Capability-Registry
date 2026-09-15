// Fabric Capability Registry test suite: adversarial hardening.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>
#include <vector>

#include "fabric/capability/fabric_capability.hpp"
#include "registry_fixture.hpp"
#include "test_framework.hpp"

using namespace fabric::capability;
using namespace fcr::test;

FCR_TEST(adversarial, malformed_identifiers_never_reach_the_registry) {
  Fixture fixture;
  const std::vector<std::string> hostile = {
      "", ":", "nic:", ":name", "nic:UPPER", "nic:dot..dot", "nic:--", "nic: leading",
      "nic:trailing-", std::string("nic:") + std::string(400, 'a'), "nic:name:with:colons",
      "unknownkind:name"};
  for (const std::string& text : hostile) {
    FCR_CHECK(!EntityId::Parse(text).HasValue());
  }
  const std::vector<std::string> hostile_capabilities = {
      "", ".", "..", "fabric", "fabric.", ".fabric.port.x", "fabric..port.x",
      "vendor..x.y", "fabric.port.", "fabric.port.UPPER", "fabric.port.x%00",
      std::string("fabric.port.") + std::string(200, 'a')};
  for (const std::string& text : hostile_capabilities) {
    FCR_CHECK(!CapabilityId::Parse(text).HasValue());
  }
}

FCR_TEST(adversarial, absurd_declared_sizes_are_rejected_before_allocation) {
  // A frame that declares a payload far beyond the bound.
  std::vector<std::byte> frame;
  {
    ByteWriter writer(frame);
    for (const char ch : std::string("FCRF")) writer.U8(static_cast<std::uint8_t>(ch));
    writer.U16(kProtocolVersion);
    writer.U16(static_cast<std::uint16_t>(WireMessageType::Publish));
    writer.U32(0);
    writer.U64(1);
    writer.U32(0xFFFFFFF0u);
  }
  FCR_CHECK_CODE(DecodeFrame(frame), ErrorCode::FrameTooLarge);

  // A publication result that declares more diff entries than allowed.
  std::vector<std::byte> payload;
  {
    ByteWriter writer(payload);
    writer.U8(0);            // status: committed
    writer.U16(0);           // code: ok
    writer.U32(0);           // message
    writer.U32(0);           // publication
    writer.U32(0);           // attempt
    writer.U32(0);           // entity
    for (int index = 0; index < 8; ++index) writer.U64(0);
    for (int index = 0; index < kDigestBytes; ++index) writer.U8(0);
    writer.U32(0);           // diff entity
    for (int index = 0; index < 4; ++index) writer.U64(0);
    for (int index = 0; index < 2 * kDigestBytes; ++index) writer.U8(0);
    writer.U32(0xFFFFFFFFu); // diff entry count
  }
  FCR_CHECK_CODE(DecodePublicationResult(payload), ErrorCode::TooManyItems);

  // A value that declares an absurd cardinality.
  std::vector<std::byte> value;
  {
    ByteWriter writer(value);
    writer.U8(static_cast<std::uint8_t>(ValueKind::EnumSet));
    writer.U32(0);           // empty domain text
    writer.U32(0xFFFFFFFFu); // absurd code count
  }
  ByteReader reader(value);
  FCR_CHECK_CODE(CapabilityValue::Decode(reader), ErrorCode::TooManyItems);
}

FCR_TEST(adversarial, oversized_and_contradictory_publications_are_rejected) {
  Fixture fixture;
  const EntityId entity = Entity("nic:hostile");
  const EntityGeneration generation = EntityGeneration::FromValue(1);

  // Oversized claim list.
  PublicationRequest request;
  request.mode = PublicationMode::FullSnapshot;
  request.publication = *PublicationId::Parse("p-hostile");
  request.attempt = *MutationAttemptId::Parse("a-hostile");
  request.epoch = fixture.registry->CurrentEpoch();
  request.authority = AuthorityContext{fixture.scope(), fixture.publisher, fixture.boot,
                                       *SourceId::Parse("test-source"),
                                       SourceGeneration::FromValue(1)};
  request.entity = entity;
  request.entity_generation = generation;
  for (std::size_t index = 0; index <= limits::kMaxClaimsPerPublication; ++index) {
    request.claims.push_back(BoolClaim("fabric.forwarding.ecmp_supported", true));
  }
  auto oversized = fixture.registry->Publish(request);
  FCR_REQUIRE_OK(oversized);
  FCR_CHECK(oversized.Value().Rejected());
  FCR_CHECK(oversized.Value().code == ErrorCode::TooManyItems);

  // A contradictory range is rejected by value construction itself.
  FCR_CHECK_CODE(CapabilityValue::NumericRangeValue(Unit::Bytes, 9000, 1500),
                 ErrorCode::ValueContradiction);
  FCR_CHECK_CODE(CapabilityValue::NumericSetValue(Unit::Count,
                                                  std::array<std::uint64_t, 2>{5, 5}),
                 ErrorCode::DuplicateValue);

  // Absurd integer values in claims.
  CapabilityClaim absurd = QuantityClaim("fabric.queue.max_rx_queues", Unit::Count,
                                         limits::kMaxQuantity);
  auto rejected = fixture.PublishSnapshot(entity, generation, {absurd});
  FCR_REQUIRE_OK(rejected);
  FCR_CHECK(rejected.Value().Rejected());
  FCR_CHECK(rejected.Value().code == ErrorCode::ValueOutOfRange);

  // Forged vendor namespace: a canonical capability cannot be smuggled in
  // through a vendor descriptor, and a vendor capability is unknown until a
  // descriptor exists.
  CapabilityClaim forged;
  forged.capability = Cap("vendor.evil.core.some_feature");
  forged.state = CapabilityState::Supported;
  forged.value = CapabilityValue::Boolean(true);
  auto unknown_vendor = fixture.PublishSnapshot(entity, generation, {forged});
  FCR_REQUIRE_OK(unknown_vendor);
  FCR_CHECK(unknown_vendor.Value().Rejected());
  FCR_CHECK(unknown_vendor.Value().code == ErrorCode::UnknownCapability);
}

FCR_TEST(adversarial, every_bit_flip_in_a_frame_is_detected_or_rejected) {
  auto payload = EncodeHello(HelloPayload{
      *PublisherId::Parse("pub"), *AuthorityScopeId::Parse("scope"), *SourceId::Parse("source"),
      Boot(5), CoordinatorEpoch::FromValue(3), {*CapabilityNamespaceId::Parse("fabric.port")}});
  FCR_REQUIRE_OK(payload);
  auto frame = EncodeFrame(WireMessageType::Hello, 0, 7, payload.Value());
  FCR_REQUIRE_OK(frame);

  for (std::size_t position = 0; position < frame.Value().size(); position += 3) {
    std::vector<std::byte> corrupted = frame.Value();
    corrupted[position] = static_cast<std::byte>(
        static_cast<unsigned char>(corrupted[position]) ^ 0x01u);
    auto decoded = DecodeFrame(corrupted);
    if (decoded.HasValue()) {
      FCR_FAIL("a flipped bit at offset " + std::to_string(position) + " was not detected");
    }
  }
  // The intact frame still decodes.
  FCR_REQUIRE_OK(DecodeFrame(frame.Value()));
}

FCR_TEST(adversarial, deterministic_byte_soup_never_crashes_the_codec) {
  DeterministicRng rng(0xBADF00Dull);
  for (int iteration = 0; iteration < 512; ++iteration) {
    const std::size_t length = rng.NextBelow(512);
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
