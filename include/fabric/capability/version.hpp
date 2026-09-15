// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>

namespace fabric::capability {

/// Library version. Kept in lockstep with the CMake project version and the
/// release tag.
inline constexpr int kVersionMajor = 1;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 0;

/// Wire and persistence format version. Independent of the library version.
inline constexpr std::uint32_t kFormatVersion = 1;

inline constexpr const char* kLibraryName = "FabricCapabilityRegistry";
inline constexpr const char* kVersionString = "1.0.0";

/// CMake package name used by find_package().
inline constexpr const char* kPackageName = "FabricCapabilityRegistry";

/// Namespaced CMake target exported by the install tree.
inline constexpr const char* kPackageTarget = "SummonSoftwareLabs::FabricCapabilityRegistry";

}  // namespace fabric::capability
