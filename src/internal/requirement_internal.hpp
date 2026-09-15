// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Internal boundary between the requirement engine (requirement.cpp) and the
// registry (registry.cpp). The engine only ever uses the public query API.

#pragma once

#include "fabric/capability/ids.hpp"
#include "fabric/capability/requirement.hpp"

namespace fabric::capability {

class CapabilityRegistry;

/// Evaluates one requirement expression against one entity using only public
/// registry queries. Deterministic, hard constraint first, fail closed.
RequirementEvaluation EvaluateRequirementAgainst(const CapabilityRegistry& registry,
                                                 const EntityId& entity,
                                                 const Requirement& requirement);

}  // namespace fabric::capability
