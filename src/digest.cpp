// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/capability/digest.hpp"

#include <cstring>

namespace fabric::capability {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

constexpr char kHexDigits[] = "0123456789abcdef";

std::uint32_t RotateRight(std::uint32_t value, unsigned bits) {
  return (value >> bits) | (value << (32u - bits));
}

int HexValue(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

}  // namespace

Sha256::Sha256()
    : state_{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
             0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u} {}

void Sha256::Transform(const std::uint8_t* block) {
  std::uint32_t schedule[64];
  for (int index = 0; index < 16; ++index) {
    schedule[index] = (static_cast<std::uint32_t>(block[index * 4]) << 24) |
                      (static_cast<std::uint32_t>(block[index * 4 + 1]) << 16) |
                      (static_cast<std::uint32_t>(block[index * 4 + 2]) << 8) |
                      static_cast<std::uint32_t>(block[index * 4 + 3]);
  }
  for (int index = 16; index < 64; ++index) {
    const std::uint32_t s0 = RotateRight(schedule[index - 15], 7) ^
                             RotateRight(schedule[index - 15], 18) ^
                             (schedule[index - 15] >> 3);
    const std::uint32_t s1 = RotateRight(schedule[index - 2], 17) ^
                             RotateRight(schedule[index - 2], 19) ^
                             (schedule[index - 2] >> 10);
    schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (int index = 0; index < 64; ++index) {
    const std::uint32_t s1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
    const std::uint32_t choose = (e & f) ^ (~e & g);
    const std::uint32_t temp1 = h + s1 + choose + kRoundConstants[index] + schedule[index];
    const std::uint32_t s0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::Update(std::span<const std::byte> data) {
  if (finished_) return;
  total_bytes_ += data.size();
  std::size_t offset = 0;
  while (offset < data.size()) {
    const std::size_t space = buffer_.size() - buffered_;
    const std::size_t take = (data.size() - offset) < space ? (data.size() - offset) : space;
    std::memcpy(buffer_.data() + buffered_, data.data() + offset, take);
    buffered_ += take;
    offset += take;
    if (buffered_ == buffer_.size()) {
      Transform(buffer_.data());
      buffered_ = 0;
    }
  }
}

void Sha256::Update(std::string_view text) {
  Update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

Digest Sha256::Finish() {
  if (!finished_) {
    const std::uint64_t bit_length = total_bytes_ * 8u;
    const std::uint8_t padding = 0x80u;
    Update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(&padding), 1));
    const std::uint8_t zero = 0x00u;
    while (buffered_ != 56) {
      Update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(&zero), 1));
    }
    std::uint8_t length_bytes[8];
    for (int index = 0; index < 8; ++index) {
      length_bytes[index] = static_cast<std::uint8_t>((bit_length >> (8 * (7 - index))) & 0xFFu);
    }
    Update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(length_bytes), 8));
    finished_ = true;
  }
  std::array<std::byte, kDigestBytes> bytes{};
  for (std::size_t index = 0; index < state_.size(); ++index) {
    bytes[index * 4] = static_cast<std::byte>((state_[index] >> 24) & 0xFFu);
    bytes[index * 4 + 1] = static_cast<std::byte>((state_[index] >> 16) & 0xFFu);
    bytes[index * 4 + 2] = static_cast<std::byte>((state_[index] >> 8) & 0xFFu);
    bytes[index * 4 + 3] = static_cast<std::byte>(state_[index] & 0xFFu);
  }
  return Digest(bytes);
}

Digest ComputeDigest(std::span<const std::byte> data) {
  Sha256 hasher;
  hasher.Update(data);
  return hasher.Finish();
}

Digest ComputeDigest(std::string_view text) {
  Sha256 hasher;
  hasher.Update(text);
  return hasher.Finish();
}

std::uint32_t Crc32(std::span<const std::byte> data) {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (const std::byte raw : data) {
    crc ^= static_cast<std::uint32_t>(static_cast<std::uint8_t>(raw));
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask = 0u - (crc & 1u);
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return crc ^ 0xFFFFFFFFu;
}

static std::string RenderDigest(const std::array<std::byte, kDigestBytes>& bytes,
                                std::size_t count) {
  std::string text;
  text.reserve(count * 2);
  for (std::size_t index = 0; index < count; ++index) {
    const auto value = static_cast<std::uint8_t>(bytes[index]);
    text.push_back(kHexDigits[value >> 4]);
    text.push_back(kHexDigits[value & 0x0Fu]);
  }
  return text;
}

std::string Digest::ToString() const { return RenderDigest(bytes_, kDigestBytes); }

std::string Digest::ShortString() const { return RenderDigest(bytes_, kDigestBytes / 2); }

bool Digest::IsZero() const noexcept {
  for (const std::byte value : bytes_) {
    if (value != std::byte{0}) return false;
  }
  return true;
}

std::size_t Digest::Hash() const noexcept {
  std::size_t hash = 1469598103934665603ull;
  for (std::size_t index = 0; index < 8; ++index) {
    hash ^= static_cast<std::uint8_t>(bytes_[index]);
    hash *= 1099511628211ull;
  }
  return hash;
}

Outcome<Digest> Digest::Parse(std::string_view hex) {
  if (hex.size() != kDigestBytes * 2) {
    return Outcome<Digest>::Failure(ErrorCode::MalformedIdentifier,
                                    "digest must be exactly 64 hexadecimal characters",
                                    "length=" + std::to_string(hex.size()));
  }
  std::array<std::byte, kDigestBytes> bytes{};
  for (std::size_t index = 0; index < kDigestBytes; ++index) {
    const int high = HexValue(hex[index * 2]);
    const int low = HexValue(hex[index * 2 + 1]);
    if (high < 0 || low < 0) {
      return Outcome<Digest>::Failure(ErrorCode::MalformedIdentifier,
                                      "digest contains a non hexadecimal character");
    }
    bytes[index] = static_cast<std::byte>((high << 4) | low);
  }
  return Digest(bytes);
}

Digest Digest::Of(std::span<const std::byte> data) { return ComputeDigest(data); }

}  // namespace fabric::capability
