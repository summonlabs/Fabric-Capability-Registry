// Fabric Capability Registry test suite: typed identities and diagnostics.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/capability/fabric_capability.hpp"
#include "registry_fixture.hpp"
#include "test_framework.hpp"

using namespace fabric::capability;
using namespace fcr::test;

FCR_TEST(ids, entity_identifier_rules) {
  auto switch_id = EntityId::Parse("switch:leaf-01");
  FCR_REQUIRE_OK(switch_id);
  FCR_CHECK(switch_id.Value().Kind() == FabricEntityKind::Switch);
  FCR_CHECK_EQ(switch_id.Value().CanonicalName(), std::string("leaf-01"));
  FCR_CHECK_EQ(switch_id.Value().ToString(), std::string("switch:leaf-01"));

  FCR_REQUIRE_OK(EntityId::Create(FabricEntityKind::Port, "port-1/1"));
  FCR_CHECK(EntityId::Create(FabricEntityKind::Port, "port-1/1").HasValue() == false);
  FCR_CHECK_CODE(EntityId::Create(FabricEntityKind::Port, "Port-1"), ErrorCode::MalformedIdentifier);
  FCR_CHECK_CODE(EntityId::Create(FabricEntityKind::Port, "-leading"), ErrorCode::MalformedIdentifier);
  FCR_CHECK_CODE(EntityId::Create(FabricEntityKind::Port, "double--separator"),
                 ErrorCode::MalformedIdentifier);
  FCR_CHECK_CODE(EntityId::Create(FabricEntityKind::Port, ""), ErrorCode::MalformedIdentifier);
  FCR_CHECK_CODE(EntityId::Create(FabricEntityKind::Unknown, "x"), ErrorCode::MalformedIdentifier);
  FCR_CHECK_CODE(EntityId::Parse("switch"), ErrorCode::MalformedIdentifier);
  FCR_CHECK_CODE(EntityId::Parse("spine:leaf-01"), ErrorCode::MalformedIdentifier);
  FCR_CHECK_CODE(EntityId::Create(FabricEntityKind::Port, std::string(200, 'a')),
                 ErrorCode::MalformedIdentifier);

  const std::vector<std::string> kinds = {"fabric:1", "site:1",  "device:1",   "switch:1",
                                          "router:1", "nic:1",   "smartnic:1", "dpu:1",
                                          "port:1",   "link:1",  "endpoint:1"};
  for (const std::string& text : kinds) {
    FCR_REQUIRE_OK(EntityId::Parse(text));
  }
}

FCR_TEST(ids, capability_identifier_rules) {
  auto canonical = CapabilityId::Parse("fabric.port.supported_speeds");
  FCR_REQUIRE_OK(canonical);
  FCR_CHECK(canonical.Value().IsVendorExtension() == false);
  FCR_CHECK_EQ(canonical.Value().Namespace().Value(), std::string("fabric.port"));
  FCR_CHECK_EQ(canonical.Value().LocalName(), std::string("supported_speeds"));

  auto vendor = CapabilityId::Parse("vendor.nvidia.spectrum.some_feature");
  FCR_REQUIRE_OK(vendor);
  FCR_CHECK(vendor.Value().IsVendorExtension());
  FCR_CHECK_EQ(vendor.Value().Namespace().Value(), std::string("vendor.nvidia.spectrum"));
  FCR_CHECK_EQ(vendor.Value().LocalName(), std::string("some_feature"));

  FCR_REQUIRE_OK(CapabilityId::Parse("vendor.nvidia.raw_counter"));

  FCR_CHECK_CODE(CapabilityId::Parse("port.speeds"), ErrorCode::UnknownNamespace);
  FCR_CHECK_CODE(CapabilityId::Parse("fabric.port"), ErrorCode::MalformedIdentifier);
  FCR_CHECK_CODE(CapabilityId::Parse("fabric.port.supported_speeds.extra"),
                 ErrorCode::MalformedIdentifier);
  FCR_CHECK_CODE(CapabilityId::Parse("fabric.port.Supported"), ErrorCode::MalformedIdentifier);
  FCR_CHECK_CODE(CapabilityId::Parse(""), ErrorCode::MalformedIdentifier);
  FCR_CHECK_CODE(CapabilityId::Parse("vendor.nvidia.spectrum"), ErrorCode::MalformedIdentifier);
  FCR_CHECK_CODE(CapabilityNamespaceId::Parse("fabric"), ErrorCode::MalformedIdentifier);
  FCR_CHECK_CODE(CapabilityNamespaceId::Parse("vendor"), ErrorCode::MalformedIdentifier);
  FCR_CHECK_CODE(CapabilityNamespaceId::Parse("other.port"), ErrorCode::UnknownNamespace);

  // Ordering is deterministic and total.
  FCR_CHECK(Cap("fabric.port.mtu_range") < Cap("fabric.port.supported_speeds"));
  FCR_CHECK(Cap("fabric.port.mtu_range") == Cap("fabric.port.mtu_range"));
}

FCR_TEST(ids, token_charsets) {
  FCR_REQUIRE_OK(PublisherId::Parse("publisher-1"));
  FCR_CHECK_CODE(PublisherId::Parse("Publisher"), ErrorCode::MalformedIdentifier);
  FCR_CHECK_CODE(PublisherId::Parse("publisher 1"), ErrorCode::MalformedIdentifier);
  FCR_CHECK_CODE(WorkerBootId::Parse("0123456789abcdef"), ErrorCode::MalformedIdentifier);
  FCR_REQUIRE_OK(WorkerBootId::Parse("0123456789abcdef0123456789abcdef"));
  FCR_CHECK_CODE(WorkerBootId::Parse("0123456789ABCDEF0123456789ABCDEF"),
                 ErrorCode::MalformedIdentifier);
  FCR_CHECK_CODE(WorkerBootId::Parse("-0123456789abcdef0123456789abcdef"),
                 ErrorCode::MalformedIdentifier);
  FCR_REQUIRE_OK(MutationAttemptId::Parse("attempt 1 of 3"));
  FCR_CHECK_CODE(MutationAttemptId::Parse("attempt\t1"), ErrorCode::MalformedIdentifier);
  FCR_REQUIRE_OK(ReasonToken::Parse("firmware update removed the feature"));
  FCR_CHECK_CODE(ReasonToken::Parse(" leading space"), ErrorCode::MalformedIdentifier);
  FCR_CHECK_CODE(EvidenceId::Parse("0123"), ErrorCode::MalformedIdentifier);
  FCR_REQUIRE_OK(EvidenceId::Parse("0123456789abcdef0123456789abcdef"));
}

FCR_TEST(ids, counter_monotonicity) {
  EntityGeneration generation = EntityGeneration::FromValue(1);
  FCR_CHECK_EQ(generation.Value(), 1ull);
  auto next = generation.Next();
  FCR_REQUIRE_OK(next);
  FCR_CHECK_EQ(next.Value().Value(), 2ull);
  FCR_CHECK(!EntityGeneration{}.IsSet());

  CoordinatorEpoch huge = CoordinatorEpoch::FromValue(UINT64_MAX);
  FCR_CHECK_CODE(huge.Next(), ErrorCode::ArithmeticOverflow);

  FCR_CHECK(RegistryGeneration::FromValue(5) > RegistryGeneration::FromValue(4));
  FCR_CHECK(CapabilitySetGeneration::FromValue(0) == CapabilitySetGeneration{});
}

FCR_TEST(ids, capability_state_semantics) {
  FCR_CHECK_EQ(std::string(CapabilityStateName(CapabilityState::Unknown)), std::string("UNKNOWN"));
  FCR_CHECK_EQ(std::string(CapabilityStateName(CapabilityState::Supported)),
               std::string("SUPPORTED"));
  FCR_CHECK_EQ(std::string(CapabilityStateName(CapabilityState::Unsupported)),
               std::string("UNSUPPORTED"));
  FCR_CHECK_EQ(std::string(CapabilityStateName(CapabilityState::RevalidationRequired)),
               std::string("REVALIDATION_REQUIRED"));
  FCR_CHECK_EQ(std::string(CapabilityStateName(CapabilityState::Conflicted)),
               std::string("CONFLICTED"));

  FCR_CHECK(IsActionableSupport(CapabilityState::Supported));
  FCR_CHECK(!IsActionableSupport(CapabilityState::Unsupported));
  FCR_CHECK(!IsActionableSupport(CapabilityState::Unknown));
  FCR_CHECK(!IsActionableSupport(CapabilityState::RevalidationRequired));
  FCR_CHECK(!IsActionableSupport(CapabilityState::Conflicted));
  FCR_CHECK(IsAuthoritativeAbsence(CapabilityState::Unsupported));
  FCR_CHECK(!IsAuthoritativeAbsence(CapabilityState::Unknown));
  FCR_CHECK(FailsClosed(CapabilityState::Unknown));
  FCR_CHECK(FailsClosed(CapabilityState::RevalidationRequired));
  FCR_CHECK(FailsClosed(CapabilityState::Conflicted));
  FCR_CHECK(!FailsClosed(CapabilityState::Supported));

  FCR_REQUIRE_OK(ParseCapabilityState("revalidation-required"));
  FCR_REQUIRE_OK(ParseCapabilityState("SUPPORTED"));
  FCR_CHECK_CODE(ParseCapabilityState("maybe"), ErrorCode::MalformedValue);
}

FCR_TEST(ids, error_code_classification) {
  FCR_CHECK_EQ(std::string(ErrorCodeName(ErrorCode::WorkerBootFenced)),
               std::string("worker-boot-fenced"));
  FCR_CHECK(IsStaleRejection(ErrorCode::WorkerBootFenced));
  FCR_CHECK(IsStaleRejection(ErrorCode::EntityGenerationStale));
  FCR_CHECK(IsStaleRejection(ErrorCode::StaleReplay));
  FCR_CHECK(IsStaleRejection(ErrorCode::SourceGenerationStale));
  FCR_CHECK(!IsStaleRejection(ErrorCode::DuplicateClaim));
  FCR_CHECK(IsAuthorityRejection(ErrorCode::UnauthorizedPublisher));
  FCR_CHECK(IsAuthorityRejection(ErrorCode::AuthorityScopeViolation));
  FCR_CHECK(!IsAuthorityRejection(ErrorCode::NotFound));
  FCR_CHECK(IsMalformedInputRejection(ErrorCode::ValueContradiction));
  FCR_CHECK(!IsMalformedInputRejection(ErrorCode::StaleReplay));
  FCR_CHECK(IsIntegrityRejection(ErrorCode::PersistenceIntegrityFailure));
  FCR_CHECK(IsIntegrityRejection(ErrorCode::FrameIntegrityFailure));
  FCR_CHECK(!IsIntegrityRejection(ErrorCode::FrameTooLarge));

  Error error(ErrorCode::UnknownCapability, "capability is not declared", "fabric.x.y");
  FCR_CHECK_EQ(error.ToString(), std::string("unknown-capability: capability is not declared (fabric.x.y)"));
}

FCR_TEST(ids, digest_is_reproducible) {
  // Known SHA-256 vectors prove the digest implementation itself.
  FCR_CHECK_EQ(ComputeDigest(std::string_view("")).ToString(),
               std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  FCR_CHECK_EQ(ComputeDigest(std::string_view("abc")).ToString(),
               std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  FCR_CHECK_EQ(
      ComputeDigest(std::string_view("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))
          .ToString(),
      std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));

  const std::string long_text(1000, 'a');
  Digest streaming;
  {
    Sha256 hasher;
    for (int index = 0; index < 100; ++index) {
      hasher.Update(std::string_view(long_text).substr(0, 10));
    }
    streaming = hasher.Finish();
  }
  FCR_CHECK_EQ(streaming.ToString(), ComputeDigest(long_text).ToString());

  const std::string hex = ComputeDigest(std::string_view("x")).ToString();
  auto parsed = Digest::Parse(hex);
  FCR_REQUIRE_OK(parsed);
  FCR_CHECK_EQ(parsed.Value().ToString(), hex);
  FCR_CHECK_EQ(parsed.Value().ShortString().size(), std::size_t(32));
  FCR_CHECK_CODE(Digest::Parse("00"), ErrorCode::MalformedIdentifier);
  FCR_CHECK_CODE(Digest::Parse(std::string(64, 'z')), ErrorCode::MalformedIdentifier);
  FCR_CHECK(Digest{}.IsZero());

  // CRC-32 known vector.
  FCR_CHECK_EQ(Crc32(std::span<const std::byte>(
                  reinterpret_cast<const std::byte*>("123456789"), 9)),
               0xCBF43926u);
}
