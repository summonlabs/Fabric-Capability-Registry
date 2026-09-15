// Fabric Capability Registry test suite: compatibility requirement engine.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/capability/fabric_capability.hpp"
#include "registry_fixture.hpp"
#include "test_framework.hpp"

using namespace fabric::capability;
using namespace fcr::test;

namespace {

Requirement Leaf(RequirementKind kind, const char* capability) {
  Requirement requirement;
  requirement.kind = kind;
  requirement.capability = Cap(capability);
  return requirement;
}

}  // namespace

FCR_TEST(requirement, state_requirement_fails_closed) {
  Fixture fixture;
  const EntityId entity = Entity("nic:0");
  FCR_REQUIRE_OK(fixture.PublishSnapshot(entity, EntityGeneration::FromValue(1),
                                         {SpeedSetClaim({400'000'000'000ull})}));

  Requirement supported = Leaf(RequirementKind::StateIs, "fabric.port.supported_speeds");
  supported.required_state = CapabilityState::Supported;
  auto satisfied = fixture.registry->Evaluate(entity, supported);
  FCR_CHECK(satisfied.outcome == RequirementOutcome::Satisfied);

  Requirement unknown = Leaf(RequirementKind::StateIs, "fabric.offload.rdma");
  unknown.required_state = CapabilityState::Supported;
  auto undetermined = fixture.registry->Evaluate(entity, unknown);
  FCR_CHECK(undetermined.outcome == RequirementOutcome::Undetermined);
  FCR_CHECK(!undetermined.explanation.steps.empty());

  // Without a proof demand the same requirement is reported as not satisfied,
  // but never as satisfied.
  Requirement lenient = unknown;
  lenient.require_proof = false;
  auto not_satisfied = fixture.registry->Evaluate(entity, lenient);
  FCR_CHECK(not_satisfied.outcome == RequirementOutcome::NotSatisfied);
}

FCR_TEST(requirement, quantity_and_set_requirements) {
  Fixture fixture;
  const EntityId entity = Entity("nic:0");
  FCR_REQUIRE_OK(fixture.PublishSnapshot(entity, EntityGeneration::FromValue(1),
                                         {SpeedSetClaim({100'000'000'000ull, 400'000'000'000ull}),
                                          QuantityClaim("fabric.queue.max_rx_queues", Unit::Count, 128),
                                          MtuRangeClaim(1500, 9216),
                                          ProtocolClaim({ProtocolId::Ethernet, ProtocolId::Ipv4}),
                                          TelemetryClaim({0, 1, 2})}));

  Requirement minimum = Leaf(RequirementKind::Minimum, "fabric.queue.max_rx_queues");
  auto quantity = CapabilityValue::QuantityValue(64, Unit::Count);
  FCR_REQUIRE_OK(quantity);
  minimum.operand = quantity.Value();
  FCR_CHECK(fixture.registry->Evaluate(entity, minimum).outcome == RequirementOutcome::Satisfied);

  Requirement too_much = minimum;
  auto high = CapabilityValue::QuantityValue(256, Unit::Count);
  FCR_REQUIRE_OK(high);
  too_much.operand = high.Value();
  FCR_CHECK(fixture.registry->Evaluate(entity, too_much).outcome ==
            RequirementOutcome::NotSatisfied);

  Requirement in_set = Leaf(RequirementKind::InSet, "fabric.port.supported_speeds");
  auto speed = CapabilityValue::QuantityValue(400'000'000'000ull, Unit::BitsPerSecond);
  FCR_REQUIRE_OK(speed);
  in_set.operand = speed.Value();
  FCR_CHECK(fixture.registry->Evaluate(entity, in_set).outcome == RequirementOutcome::Satisfied);
  Requirement absent_speed = in_set;
  auto other_speed = CapabilityValue::QuantityValue(10'000'000'000ull, Unit::BitsPerSecond);
  FCR_REQUIRE_OK(other_speed);
  absent_speed.operand = other_speed.Value();
  FCR_CHECK(fixture.registry->Evaluate(entity, absent_speed).outcome ==
            RequirementOutcome::NotSatisfied);

  Requirement range = Leaf(RequirementKind::RangeContains, "fabric.port.mtu_range");
  auto mtu = CapabilityValue::QuantityValue(9000, Unit::Bytes);
  FCR_REQUIRE_OK(mtu);
  range.operand = mtu.Value();
  FCR_CHECK(fixture.registry->Evaluate(entity, range).outcome == RequirementOutcome::Satisfied);
  Requirement outside = range;
  auto too_big = CapabilityValue::QuantityValue(64000, Unit::Bytes);
  FCR_REQUIRE_OK(too_big);
  outside.operand = too_big.Value();
  FCR_CHECK(fixture.registry->Evaluate(entity, outside).outcome == RequirementOutcome::NotSatisfied);

  Requirement protocol = Leaf(RequirementKind::ProtocolSupported, "fabric.protocol.families");
  protocol.operand_protocol = ProtocolId::Ethernet;
  FCR_CHECK(fixture.registry->Evaluate(entity, protocol).outcome == RequirementOutcome::Satisfied);
  protocol.operand_protocol = ProtocolId::Vxlan;
  FCR_CHECK(fixture.registry->Evaluate(entity, protocol).outcome == RequirementOutcome::NotSatisfied);

  Requirement telemetry = Leaf(RequirementKind::EnumerationSupported, "fabric.telemetry.families");
  telemetry.operand_enum_code = 2;
  FCR_CHECK(fixture.registry->Evaluate(entity, telemetry).outcome == RequirementOutcome::Satisfied);
  telemetry.operand_enum_code = 7;
  FCR_CHECK(fixture.registry->Evaluate(entity, telemetry).outcome == RequirementOutcome::NotSatisfied);

  Requirement boolean = Leaf(RequirementKind::BooleanEquals, "fabric.forwarding.ecmp_supported");
  boolean.operand = CapabilityValue::Boolean(true);
  auto boolean_outcome = fixture.registry->Evaluate(entity, boolean);
  FCR_CHECK(boolean_outcome.outcome == RequirementOutcome::Undetermined);
}

FCR_TEST(requirement, compound_expressions_are_bounded_and_deterministic) {
  Fixture fixture;
  const EntityId entity = Entity("nic:0");
  FCR_REQUIRE_OK(fixture.PublishSnapshot(entity, EntityGeneration::FromValue(1),
                                         {SpeedSetClaim({400'000'000'000ull}),
                                          QuantityClaim("fabric.queue.max_rx_queues", Unit::Count, 128),
                                          BoolClaim("fabric.forwarding.ecmp_supported", true)}));

  Requirement all;
  all.kind = RequirementKind::AllOf;
  Requirement speeds = Leaf(RequirementKind::StateIs, "fabric.port.supported_speeds");
  Requirement queues = Leaf(RequirementKind::Minimum, "fabric.queue.max_rx_queues");
  auto minimum_queues = CapabilityValue::QuantityValue(64, Unit::Count);
  FCR_REQUIRE_OK(minimum_queues);
  queues.operand = minimum_queues.Value();
  all.children = {speeds, queues};
  auto evaluation = fixture.registry->Evaluate(entity, all);
  FCR_CHECK(evaluation.outcome == RequirementOutcome::Satisfied);
  FCR_CHECK_EQ(evaluation.nodes_evaluated, evaluation.nodes.size());
  FCR_CHECK(evaluation.nodes.size() >= 3);
  FCR_CHECK(!evaluation.explanation.steps.empty());
  const std::string first = evaluation.explanation.ToText();
  FCR_CHECK_EQ(fixture.registry->Evaluate(entity, all).explanation.ToText(), first);

  Requirement any;
  any.kind = RequirementKind::AnyOf;
  Requirement impossible = Leaf(RequirementKind::StateIs, "fabric.offload.rdma");
  impossible.required_state = CapabilityState::Supported;
  any.children = {impossible, speeds};
  FCR_CHECK(fixture.registry->Evaluate(entity, any).outcome == RequirementOutcome::Satisfied);

  Requirement negated;
  negated.kind = RequirementKind::AllOf;
  Requirement not_supported = Leaf(RequirementKind::StateIs, "fabric.offload.rdma");
  not_supported.required_state = CapabilityState::Unsupported;
  Requirement negate;
  negate.kind = RequirementKind::Not;
  negate.children = {not_supported};
  negated.children = {negate};
  // The negated leaf demands proof that the capability is UNSUPPORTED; with no claim at
  // all the leaf is unproven and the negation stays undetermined.
  FCR_CHECK(fixture.registry->Evaluate(entity, negated).outcome ==
            RequirementOutcome::Undetermined);
  Requirement lenient_negation = negated;
  lenient_negation.children.front().children.front().require_proof = false;
  FCR_CHECK(fixture.registry->Evaluate(entity, lenient_negation).outcome ==
            RequirementOutcome::Satisfied);

  // NOT is not permitted as the root requirement.
  Requirement root_not;
  root_not.kind = RequirementKind::Not;
  root_not.children = {speeds};
  FCR_CHECK_CODE(ValidateRequirement(root_not), ErrorCode::InvalidArgument);
  const RequirementEvaluation rejected = fixture.registry->Evaluate(entity, root_not);
  FCR_CHECK(rejected.outcome == RequirementOutcome::Undetermined);
  FCR_CHECK_EQ(rejected.explanation.code, std::string("requirement.invalid"));

  // Depth and node bounds are enforced.
  Requirement deep = Leaf(RequirementKind::StateIs, "fabric.port.supported_speeds");
  for (int index = 0; index < limits::kMaxRequirementDepth + 2; ++index) {
    Requirement wrapper;
    wrapper.kind = RequirementKind::AllOf;
    wrapper.children = {deep};
    deep = wrapper;
  }
  FCR_CHECK_CODE(ValidateRequirement(deep), ErrorCode::NestingTooDeep);

  Requirement wide;
  wide.kind = RequirementKind::AllOf;
  for (std::size_t index = 0; index < limits::kMaxRequirementNodes + 4; ++index) {
    wide.children.push_back(Leaf(RequirementKind::StateIs, "fabric.port.supported_speeds"));
  }
  FCR_CHECK_CODE(ValidateRequirement(wide), ErrorCode::TooManyItems);

  // Operand shape is validated.
  Requirement malformed = Leaf(RequirementKind::Minimum, "fabric.queue.max_rx_queues");
  FCR_CHECK_CODE(ValidateRequirement(malformed), ErrorCode::ValueTypeMismatch);
  Requirement no_capability;
  no_capability.kind = RequirementKind::Minimum;
  FCR_CHECK_CODE(ValidateRequirement(no_capability), ErrorCode::InvalidArgument);
  FCR_CHECK_EQ(std::string(RequirementOutcomeName(RequirementOutcome::Undetermined)),
               std::string("undetermined"));
  FCR_REQUIRE_OK(ParseRequirementKind("all-of"));
  FCR_CHECK_CODE(ParseRequirementKind("script"), ErrorCode::MalformedValue);
}
