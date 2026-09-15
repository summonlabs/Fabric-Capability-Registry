// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fabric/capability/error.hpp"

namespace fabric::capability {

/// Deterministic little-endian byte writer used by canonical digests,
/// persistence and the wire protocol. Every length-prefixed write is bounded
/// by an explicit caller supplied limit; the writer never grows without a
/// caller declared bound.
class ByteWriter {
 public:
  explicit ByteWriter(std::vector<std::byte>& out) : out_(out) {}

  void U8(std::uint8_t v) { out_.push_back(static_cast<std::byte>(v)); }
  void U16(std::uint16_t v);
  void U32(std::uint32_t v);
  void U64(std::uint64_t v);
  void I64(std::int64_t v) { U64(static_cast<std::uint64_t>(v)); }
  void Bool(bool v) { U8(v ? 1u : 0u); }

  /// Length-prefixed bounded byte string (u32 length + payload).
  Outcome<void> Bytes(std::span<const std::byte> data, std::size_t max_length);
  /// Length-prefixed bounded text (u32 length + payload bytes).
  Outcome<void> Text(std::string_view text, std::size_t max_length);

  std::size_t size() const noexcept { return out_.size(); }

 private:
  std::vector<std::byte>& out_;
};

/// Deterministic little-endian bounds-checked reader. Every read either
/// returns exactly the requested bytes or fails; the reader never reads past
/// the end of the buffer and never allocates based on an unvalidated length.
class ByteReader {
 public:
  explicit ByteReader(std::span<const std::byte> in) : in_(in) {}

  std::size_t Remaining() const noexcept { return in_.size() - offset_; }
  bool Empty() const noexcept { return Remaining() == 0; }
  std::size_t offset() const noexcept { return offset_; }

  Outcome<std::uint8_t> U8();
  Outcome<std::uint16_t> U16();
  Outcome<std::uint32_t> U32();
  Outcome<std::uint64_t> U64();
  Outcome<std::int64_t> I64();
  Outcome<bool> Bool();

  /// Length-prefixed bounded byte string. Rejects lengths above p max_length
  /// before allocating.
  Outcome<std::vector<std::byte>> Bytes(std::size_t max_length);
  /// Length-prefixed bounded text. Rejects non UTF-8-ish control bytes and
  /// lengths above p max_length before allocating.
  Outcome<std::string> Text(std::size_t max_length);

  /// Fails unless every byte has been consumed. Used to reject trailing data.
  Outcome<void> ExpectEnd() const;

 private:
  std::span<const std::byte> in_;
  std::size_t offset_ = 0;
};

/// Checked unsigned addition; fails instead of wrapping.
Outcome<std::uint64_t> CheckedAdd(std::uint64_t a, std::uint64_t b);
/// Checked unsigned multiplication; fails instead of wrapping.
Outcome<std::uint64_t> CheckedMul(std::uint64_t a, std::uint64_t b);
/// Checked conversion from size_t to u32 for length prefixes.
Outcome<std::uint32_t> CheckedU32(std::size_t value);
/// Checked conversion from u64 to size_t (rejects values above SIZE_MAX).
Outcome<std::size_t> CheckedSize(std::uint64_t value);

}  // namespace fabric::capability
