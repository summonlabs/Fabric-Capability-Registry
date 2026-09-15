// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/capability/encoding.hpp"

#include <limits>

namespace fabric::capability {
namespace {

Status EncodeFailure(ErrorCode code, std::string_view what, std::size_t value,
                     std::size_t limit) {
  return Status::Failure(code,
                         std::string("cannot encode ") + std::string(what) +
                             ": declared length exceeds the allowed bound",
                         "length=" + std::to_string(value) + " bound=" + std::to_string(limit));
}

}  // namespace

void ByteWriter::U16(std::uint16_t v) {
  out_.push_back(static_cast<std::byte>(v & 0xFFu));
  out_.push_back(static_cast<std::byte>((v >> 8) & 0xFFu));
}

void ByteWriter::U32(std::uint32_t v) {
  for (int shift = 0; shift < 32; shift += 8) {
    out_.push_back(static_cast<std::byte>((v >> shift) & 0xFFu));
  }
}

void ByteWriter::U64(std::uint64_t v) {
  for (int shift = 0; shift < 64; shift += 8) {
    out_.push_back(static_cast<std::byte>((v >> shift) & 0xFFu));
  }
}

Outcome<void> ByteWriter::Bytes(std::span<const std::byte> data, std::size_t max_length) {
  if (data.size() > max_length) {
    return EncodeFailure(ErrorCode::PayloadTooLarge, "byte string", data.size(), max_length);
  }
  const auto length = CheckedU32(data.size());
  if (!length) return length.GetError();
  U32(length.Value());
  out_.insert(out_.end(), data.begin(), data.end());
  return Status::Success();
}

Outcome<void> ByteWriter::Text(std::string_view text, std::size_t max_length) {
  if (text.size() > max_length) {
    return EncodeFailure(ErrorCode::TextTooLong, "text", text.size(), max_length);
  }
  const auto length = CheckedU32(text.size());
  if (!length) return length.GetError();
  U32(length.Value());
  for (const char ch : text) {
    out_.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
  }
  return Status::Success();
}

Outcome<std::uint8_t> ByteReader::U8() {
  if (Remaining() < 1) {
    return Outcome<std::uint8_t>::Failure(ErrorCode::MalformedEncoding, "truncated byte");
  }
  const auto value = static_cast<std::uint8_t>(in_[offset_]);
  ++offset_;
  return value;
}

Outcome<std::uint16_t> ByteReader::U16() {
  if (Remaining() < 2) {
    return Outcome<std::uint16_t>::Failure(ErrorCode::MalformedEncoding, "truncated u16");
  }
  std::uint16_t value = 0;
  for (int index = 0; index < 2; ++index) {
    value |= static_cast<std::uint16_t>(static_cast<std::uint8_t>(in_[offset_ + index]))
             << (8 * index);
  }
  offset_ += 2;
  return value;
}

Outcome<std::uint32_t> ByteReader::U32() {
  if (Remaining() < 4) {
    return Outcome<std::uint32_t>::Failure(ErrorCode::MalformedEncoding, "truncated u32");
  }
  std::uint32_t value = 0;
  for (int index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(in_[offset_ + index]))
             << (8 * index);
  }
  offset_ += 4;
  return value;
}

Outcome<std::uint64_t> ByteReader::U64() {
  if (Remaining() < 8) {
    return Outcome<std::uint64_t>::Failure(ErrorCode::MalformedEncoding, "truncated u64");
  }
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(in_[offset_ + index]))
             << (8 * index);
  }
  offset_ += 8;
  return value;
}

Outcome<std::int64_t> ByteReader::I64() {
  auto raw = U64();
  if (!raw) return raw.GetError();
  return static_cast<std::int64_t>(raw.Value());
}

Outcome<bool> ByteReader::Bool() {
  auto raw = U8();
  if (!raw) return raw.GetError();
  if (raw.Value() > 1) {
    return Outcome<bool>::Failure(ErrorCode::MalformedEncoding,
                                  "boolean field is neither 0 nor 1",
                                  "value=" + std::to_string(raw.Value()));
  }
  return raw.Value() == 1;
}

Outcome<std::vector<std::byte>> ByteReader::Bytes(std::size_t max_length) {
  auto length = U32();
  if (!length) return length.GetError();
  const std::size_t declared = length.Value();
  if (declared > max_length) {
    return Outcome<std::vector<std::byte>>::Failure(
        ErrorCode::PayloadTooLarge, "declared byte string length exceeds the allowed bound",
        "length=" + std::to_string(declared) + " bound=" + std::to_string(max_length));
  }
  if (Remaining() < declared) {
    return Outcome<std::vector<std::byte>>::Failure(
        ErrorCode::MalformedEncoding, "declared byte string length exceeds the available data",
        "declared=" + std::to_string(declared) + " available=" + std::to_string(Remaining()));
  }
  std::vector<std::byte> out(in_.begin() + static_cast<std::ptrdiff_t>(offset_),
                             in_.begin() + static_cast<std::ptrdiff_t>(offset_ + declared));
  offset_ += declared;
  return out;
}

Outcome<std::string> ByteReader::Text(std::size_t max_length) {
  auto length = U32();
  if (!length) return length.GetError();
  const std::size_t declared = length.Value();
  if (declared > max_length) {
    return Outcome<std::string>::Failure(
        ErrorCode::TextTooLong, "declared text length exceeds the allowed bound",
        "length=" + std::to_string(declared) + " bound=" + std::to_string(max_length));
  }
  if (Remaining() < declared) {
    return Outcome<std::string>::Failure(
        ErrorCode::MalformedEncoding, "declared text length exceeds the available data",
        "declared=" + std::to_string(declared) + " available=" + std::to_string(Remaining()));
  }
  std::string text;
  text.reserve(declared);
  for (std::size_t index = 0; index < declared; ++index) {
    const auto raw = static_cast<std::uint8_t>(in_[offset_ + index]);
    if (raw < 0x20 || raw == 0x7F) {
      return Outcome<std::string>::Failure(ErrorCode::MalformedEncoding,
                                           "text field contains a control character",
                                           "byte=" + std::to_string(raw));
    }
    text.push_back(static_cast<char>(raw));
  }
  offset_ += declared;
  return text;
}

Outcome<void> ByteReader::ExpectEnd() const {
  if (!Empty()) {
    return Status::Failure(ErrorCode::FrameTrailingBytes,
                           "trailing bytes remain after decoding",
                           "remaining=" + std::to_string(Remaining()));
  }
  return Status::Success();
}

Outcome<std::uint64_t> CheckedAdd(std::uint64_t a, std::uint64_t b) {
  if (a > std::numeric_limits<std::uint64_t>::max() - b) {
    return Outcome<std::uint64_t>::Failure(ErrorCode::ArithmeticOverflow,
                                           "unsigned addition overflow",
                                           std::to_string(a) + " + " + std::to_string(b));
  }
  return a + b;
}

Outcome<std::uint64_t> CheckedMul(std::uint64_t a, std::uint64_t b) {
  if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) {
    return Outcome<std::uint64_t>::Failure(ErrorCode::ArithmeticOverflow,
                                           "unsigned multiplication overflow",
                                           std::to_string(a) + " * " + std::to_string(b));
  }
  return a * b;
}

Outcome<std::uint32_t> CheckedU32(std::size_t value) {
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    return Outcome<std::uint32_t>::Failure(ErrorCode::ArithmeticOverflow,
                                           "value does not fit in a 32 bit length prefix",
                                           std::to_string(value));
  }
  return static_cast<std::uint32_t>(value);
}

Outcome<std::size_t> CheckedSize(std::uint64_t value) {
  if (value > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return Outcome<std::size_t>::Failure(ErrorCode::ArithmeticOverflow,
                                         "value does not fit in size_t",
                                         std::to_string(value));
  }
  return static_cast<std::size_t>(value);
}

}  // namespace fabric::capability
