// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Internal store file codec boundary. registry.cpp builds and consumes a
// StorePayload; persistence.cpp owns the byte layout, the integrity digest and
// the atomic replacement.

#pragma once

#include "fabric/capability/error.hpp"
#include "fabric/capability/persistence.hpp"
#include "fabric/capability/publication.hpp"
#include "src/internal/store_format.hpp"

namespace fabric::capability::internal {

/// Serialises a payload, verifies it, writes it atomically and reports what
/// was written. Never modifies the payload.
Outcome<SaveReport> WriteStore(const PersistenceConfig& config, const StorePayload& payload);

/// Reads, verifies and decodes a store file. Every bound is enforced before
/// allocation and every duplicate record is rejected.
Outcome<LoadReport> ReadStore(const PersistenceConfig& config, StorePayload& payload);

}  // namespace fabric::capability::internal
