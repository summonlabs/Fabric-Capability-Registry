// Fabric Capability Registry test suite: REAL host capability discovery.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <set>

#include "fabric/capability/fabric_capability.hpp"
#include "test_framework.hpp"

using fcr::test::ReportNote;

using namespace fabric::capability;

FCR_TEST(discovery, host_discovery_is_truthful_and_deterministic) {
  HostDiscoveryOptions options;
  auto report = DiscoverHostCapabilities(options);
  if (!report.HasValue()) {
    // Non Windows hosts have no truthful source and must say so instead of
    // fabricating capability data.
    FCR_CHECK(report.Code() == ErrorCode::DiscoveryUnavailable);
    ReportNote("REAL host discovery is UNSUPPORTED on this platform");
    return;
  }

  const HostDiscoveryReport& first = report.Value();
  FCR_CHECK_EQ(first.adapter_count, first.entities.size());
  FCR_CHECK(!first.platform.empty());
  const std::string text = RenderDiscoveryReport(first);
  FCR_CHECK_EQ(first.ToText(), text);
  FCR_CHECK(text.find('\t') == std::string::npos);

  auto second = DiscoverHostCapabilities(options);
  FCR_REQUIRE_OK(second);
  FCR_CHECK_EQ(RenderDiscoveryReport(second.Value()), text);

  std::set<std::string> names;
  for (const DiscoveredEntity& entity : first.entities) {
    FCR_CHECK(entity.entity.IsSet());
    FCR_CHECK(entity.entity.Kind() == FabricEntityKind::Nic);
    FCR_CHECK(EntityId::Parse(entity.entity.ToString()).HasValue());
    FCR_CHECK(names.insert(entity.entity.ToString()).second);
    FCR_CHECK(!entity.operator_label.empty());
    FCR_CHECK(entity.provenance == ProvenanceClass::DirectDeviceOrOsApi);
    FCR_CHECK(entity.source_class == EvidenceSourceClass::OperatingSystemApi);
    FCR_CHECK(entity.coverage == Coverage::Partial);

    std::set<std::string> capabilities;
    for (const DiscoveredFact& fact : entity.facts) {
      FCR_CHECK(fact.capability.IsSet());
      FCR_CHECK(!fact.capability.IsVendorExtension());
      FCR_CHECK(capabilities.insert(fact.capability.ToString()).second);
      FCR_CHECK(fact.note.size() <= 200);
      FCR_CHECK(!fact.note.empty());
      switch (fact.status) {
        case DiscoveryFactStatus::Reported:
          FCR_CHECK(fact.has_value);
          FCR_CHECK(fact.state == CapabilityState::Supported);
          break;
        case DiscoveryFactStatus::NotReported:
          FCR_CHECK(!fact.has_value);
          FCR_CHECK(fact.state == CapabilityState::Unknown);
          break;
        case DiscoveryFactStatus::SourceUnavailable:
          FCR_CHECK(!fact.has_value);
          FCR_CHECK(fact.state == CapabilityState::Unknown);
          break;
      }
      // Absence from a host API is never physical UNSUPPORTED.
      FCR_CHECK(fact.state != CapabilityState::Unsupported);
    }
    // Facts are strictly sorted and unique.
    for (std::size_t index = 1; index < entity.facts.size(); ++index) {
      FCR_CHECK(entity.facts[index - 1].capability < entity.facts[index].capability);
    }
    FCR_CHECK(entity.facts.size() <= options.max_facts_per_entity);
  }

  HostDiscoveryOptions bounded;
  bounded.max_facts_per_entity = 4;
  auto truncated = DiscoverHostCapabilities(bounded);
  FCR_REQUIRE_OK(truncated);
  for (const DiscoveredEntity& entity : truncated.Value().entities) {
    FCR_CHECK(entity.facts.size() <= 4);
  }

  HostDiscoveryOptions single;
  single.max_adapters = 1;
  auto limited = DiscoverHostCapabilities(single);
  FCR_REQUIRE_OK(limited);
  FCR_CHECK(limited.Value().entities.size() <= 1);

  if (first.entities.empty()) {
    ReportNote("this host exposes no non loopback adapter");
  }
}
