// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "fabric/capability/error.hpp"

namespace fabric::capability {

/// Size in bytes of every digest produced by this library (SHA-256).
inline constexpr std::size_t kDigestBytes = 32;

/// 256 bit content digest. Rendering is lower case hexadecimal and stable.
class Digest {
 public:
  Digest() = default;
  explicit Digest(std::array<std::byte, kDigestBytes> bytes) : bytes_(bytes) {}

  static Outcome<Digest> Parse(std::string_view hex);
  static Digest Of(std::span<const std::byte> data);

  const std::array<std::byte, kDigestBytes>& Bytes() const noexcept { return bytes_; }
  std::string ToString() const;
  /// Short stable rendering used for derived identifiers (128 bit prefix).
  std::string ShortString() const;
  bool IsZero() const noexcept;

  friend bool operator==(const Digest&, const Digest&) = default;
  friend std::strong_ordering operator<=>(const Digest& lhs, const Digest& rhs) {
    return lhs.bytes_ <=> rhs.bytes_;
  }
  std::size_t Hash() const noexcept;

 private:
  std::array<std::byte, kDigestBytes> bytes_{};
};

/// Streaming SHA-256. Used for canonical state digests, persistence integrity
/// and wire frame integrity. Implemented in-tree so digests are reproducible
/// on every platform and toolchain.
class Sha256 {
 public:
  Sha256();
  void Update(std::span<const std::byte> data);
  void Update(std::string_view text);
  Digest Finish();

 private:
  void Transform(const std::uint8_t* block);
  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffered_ = 0;
  std::uint64_t total_bytes_ = 0;
  bool finished_ = false;
};

/// One-shot digest of a byte range.
Digest ComputeDigest(std::span<const std::byte> data);
/// One-shot digest of a text range.
Digest ComputeDigest(std::string_view text);

/// CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320). Used for frame level
/// transmission error detection; content integrity uses SHA-256.
std::uint32_t Crc32(std::span<const std::byte> data);

/// Incremental CRC-32 over several spans. Begin, extend once per span, finish.
std::uint32_t Crc32Begin() noexcept;
std::uint32_t Crc32Extend(std::uint32_t state, std::span<const std::byte> data) noexcept;
std::uint32_t Crc32Finish(std::uint32_t state) noexcept;

}  // namespace fabric::capability

namespace std {
template <>
struct hash<fabric::capability::Digest> {
  size_t operator()(const fabric::capability::Digest& digest) const noexcept {
    return digest.Hash();
  }
};
}  // namespace std
