// Fabric Capability Registry test suite: governed capability schema.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/capability/fabric_capability.hpp"
#include "registry_fixture.hpp"
#include "test_framework.hpp"

using namespace fabric::capability;
using namespace fcr::test;

FCR_TEST(schema, canonical_descriptors_are_well_formed) {
  const CapabilitySchema& schema = CanonicalSchema();
  const std::vector<CapabilityDescriptor> descriptors = schema.Descriptors();
  FCR_CHECK(descriptors.size() >= 80);

  std::size_t absence_negative = 0;
  for (const CapabilityDescriptor& descriptor : descriptors) {
    FCR_CHECK(descriptor.id.IsSet());
    FCR_CHECK(descriptor.id.IsVendorExtension() == false);
    FCR_CHECK(!descriptor.description.empty());
    if (descriptor.absence_is_negative) ++absence_negative;
    // Every canonical descriptor resolves through the schema itself.
    auto found = schema.Find(descriptor.id);
    FCR_REQUIRE_OK(found);
    FCR_CHECK(found.Value()->kind == descriptor.kind);
  }
  // Absence may only establish UNSUPPORTED for the narrow documented set.
  FCR_CHECK_EQ(absence_negative, std::size_t(2));
  FCR_REQUIRE_OK(schema.Find(Cap("fabric.virtualization.sriov_supported")));
  FCR_REQUIRE_OK(schema.Find(Cap("fabric.timestamping.hardware_timestamp_supported")));
  FCR_CHECK(schema.Find(Cap("fabric.virtualization.sriov_supported")).Value()->absence_is_negative);
  FCR_CHECK(!schema.Find(Cap("fabric.queue.max_rx_queues")).Value()->absence_is_negative);

  FCR_CHECK(schema.Contains(Cap("fabric.port.supported_speeds")));
  FCR_CHECK(schema.Contains(Cap("fabric.queue.max_rx_queues")));
  FCR_CHECK(schema.Contains(Cap("fabric.offload.checksum_rx")));
  FCR_CHECK(schema.Contains(Cap("fabric.telemetry.families")));
  FCR_CHECK(schema.Contains(Cap("fabric.protocol.families")));
  FCR_CHECK(schema.Contains(Cap("fabric.rdma.transport_families")));
  FCR_CHECK(!schema.Contains(Cap("fabric.port.not_a_capability")));
  FCR_CHECK_CODE(schema.Find(Cap("fabric.port.not_a_capability")), ErrorCode::UnknownCapability);

  const std::vector<CapabilityNamespaceId> namespaces = CanonicalNamespaces();
  FCR_CHECK(namespaces.size() >= 15);
  for (const CapabilityNamespaceId& ns : namespaces) {
    FCR_CHECK(ns.IsVendorExtension() == false);
  }
}

FCR_TEST(schema, value_must_match_descriptor) {
  auto speeds = CanonicalSchema().Find(Cap("fabric.port.supported_speeds"));
  FCR_REQUIRE_OK(speeds);
  const std::array<std::uint64_t, 2> values = {100ull * 1000 * 1000 * 1000,
                                               400ull * 1000 * 1000 * 1000};
  auto set = CapabilityValue::NumericSetValue(Unit::BitsPerSecond, values);
  FCR_REQUIRE_OK(set);
  FCR_CHECK_OK(ValidateValueAgainstDescriptor(*speeds.Value(), set.Value()));

  // Wrong typed form.
  FCR_CHECK_CODE(ValidateValueAgainstDescriptor(*speeds.Value(), CapabilityValue::Boolean(true)),
                 ErrorCode::ValueTypeMismatch);
  // Wrong unit.
  auto bytes = CapabilityValue::NumericSetValue(Unit::Bytes, values);
  FCR_REQUIRE_OK(bytes);
  FCR_CHECK_CODE(ValidateValueAgainstDescriptor(*speeds.Value(), bytes.Value()),
                 ErrorCode::ValueTypeMismatch);
  // Absurd speed is outside the descriptor bound.
  const std::array<std::uint64_t, 1> absurd = {limits::kMaxQuantity};
  auto absurd_set = CapabilityValue::NumericSetValue(Unit::BitsPerSecond, absurd);
  FCR_REQUIRE_OK(absurd_set);
  FCR_CHECK_CODE(ValidateValueAgainstDescriptor(*speeds.Value(), absurd_set.Value()),
                 ErrorCode::ValueOutOfRange);

  // Enumeration codes must be declared by the domain.
  auto fec = CanonicalSchema().Find(Cap("fabric.port.supported_fec_modes"));
  FCR_REQUIRE_OK(fec);
  const std::array<std::uint32_t, 1> undeclared = {99};
  auto domain = EnumDomainId::Parse("fabric.port.fec_mode");
  FCR_REQUIRE_OK(domain);
  auto enum_set = CapabilityValue::EnumerationSetValue(domain.Value(), undeclared);
  FCR_REQUIRE_OK(enum_set);
  FCR_CHECK_CODE(ValidateValueAgainstDescriptor(*fec.Value(), enum_set.Value()),
                 ErrorCode::UnknownEnumDomain);
  FCR_CHECK(!EnumDomainDeclares(domain.Value(), 99));
  FCR_CHECK(EnumDomainDeclares(domain.Value(), 1));
  FCR_CHECK_EQ(EnumCodeText(domain.Value(), 1), std::string("rs"));
  FCR_CHECK_EQ(EnumCodeText(domain.Value(), 42), std::string("fabric.port.fec_mode#42"));

  // Queue limit must be a quantity in counts, not a string.
  auto queues = CanonicalSchema().Find(Cap("fabric.queue.max_rx_queues"));
  FCR_REQUIRE_OK(queues);
  auto wrong_unit = CapabilityValue::QuantityValue(128, Unit::Bytes);
  FCR_REQUIRE_OK(wrong_unit);
  FCR_CHECK_CODE(ValidateValueAgainstDescriptor(*queues.Value(), wrong_unit.Value()),
                 ErrorCode::ValueTypeMismatch);
  auto wrong_value = CapabilityValue::QuantityValue(1'000'001, Unit::Count);
  FCR_REQUIRE_OK(wrong_value);
  FCR_CHECK_CODE(ValidateValueAgainstDescriptor(*queues.Value(), wrong_value.Value()),
                 ErrorCode::ValueOutOfRange);
}

FCR_TEST(schema, vendor_extensions_are_isolated) {
  CapabilitySchema schema;
  const std::size_t canonical_size = schema.Size();

  CapabilityDescriptor extension;
  extension.id = Cap("vendor.nvidia.spectrum.some_feature");
  extension.kind = ValueKind::Boolean;
  extension.vendor_extension = true;
  extension.description = "vendor specific feature";
  FCR_REQUIRE_OK(schema.RegisterVendorDescriptor(extension));
  FCR_CHECK_EQ(schema.Size(), canonical_size + 1);
  FCR_REQUIRE_OK(schema.Find(extension.id));
  // Re-registering the identical descriptor is idempotent.
  FCR_REQUIRE_OK(schema.RegisterVendorDescriptor(extension));

  CapabilityDescriptor conflicting = extension;
  conflicting.kind = ValueKind::Quantity;
  conflicting.unit = Unit::Count;
  FCR_CHECK_CODE(schema.RegisterVendorDescriptor(conflicting), ErrorCode::DuplicateClaim);

  // A vendor descriptor must live in a vendor namespace.
  CapabilityDescriptor canonical = extension;
  canonical.id = Cap("fabric.port.vendor_thing");
  FCR_CHECK_CODE(schema.RegisterVendorDescriptor(canonical), ErrorCode::SchemaViolation);

  // A canonical descriptor may not be registered at run time.
  CapabilityDescriptor shadow = extension;
  shadow.id = Cap("fabric.forwarding.weighted_ecmp_supported");
  FCR_CHECK_CODE(schema.RegisterVendorDescriptor(shadow), ErrorCode::SchemaViolation);

  // A vendor descriptor without the extension flag is rejected.
  CapabilityDescriptor unflagged = extension;
  unflagged.vendor_extension = false;
  FCR_CHECK_CODE(schema.RegisterVendorDescriptor(unflagged), ErrorCode::SchemaViolation);

  // The canonical schema is unaffected by a vendor registration.
  FCR_CHECK(!CanonicalSchema().Contains(extension.id));
}

FCR_TEST(schema, enum_domains_are_declared) {
  FCR_CHECK(BuiltinEnumDomains().size() >= 15);
  auto domain = EnumDomainId::Parse("fabric.telemetry.family");
  FCR_REQUIRE_OK(domain);
  auto found = FindEnumDomain(domain.Value());
  FCR_REQUIRE_OK(found);
  FCR_CHECK(!found.Value()->codes.empty());
  for (const EnumCodeDescriptor& code : found.Value()->codes) {
    FCR_CHECK(!code.name.empty());
  }
  auto missing = EnumDomainId::Parse("fabric.unknown.domain");
  FCR_REQUIRE_OK(missing);
  FCR_CHECK_CODE(FindEnumDomain(missing.Value()), ErrorCode::UnknownEnumDomain);
}
