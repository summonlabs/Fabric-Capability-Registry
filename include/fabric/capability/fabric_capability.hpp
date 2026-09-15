// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

/// Umbrella header for the Fabric Capability Registry public library API.
///
/// Ownership and lifetime: identifiers, values, claims, resolutions, diffs,
/// explanations and reports are value types. Capability sets returned by the
/// registry are immutable copies; snapshots hold immutable shared views. The
/// registry itself is non-copyable and non-movable and must outlive any
/// coordinator or client that references it.
///
/// Generation semantics: entity generations are owned by Fabric Registry and
/// are only observed here. Capability set, capability and evidence
/// generations are owned by this runtime and advance exactly once per
/// committed semantic change. An exact idempotent replay never advances a
/// generation; a stale replay is always rejected.

#include "fabric/capability/authority.hpp"
#include "fabric/capability/digest.hpp"
#include "fabric/capability/discovery.hpp"
#include "fabric/capability/distributed.hpp"
#include "fabric/capability/encoding.hpp"
#include "fabric/capability/error.hpp"
#include "fabric/capability/evidence.hpp"
#include "fabric/capability/ids.hpp"
#include "fabric/capability/limits.hpp"
#include "fabric/capability/persistence.hpp"
#include "fabric/capability/publication.hpp"
#include "fabric/capability/query.hpp"
#include "fabric/capability/record.hpp"
#include "fabric/capability/registry.hpp"
#include "fabric/capability/requirement.hpp"
#include "fabric/capability/schema.hpp"
#include "fabric/capability/state.hpp"
#include "fabric/capability/synthetic.hpp"
#include "fabric/capability/value.hpp"
#include "fabric/capability/version.hpp"
