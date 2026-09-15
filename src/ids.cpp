// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/capability/ids.hpp"

#include <array>
#include <vector>

#include "fabric/capability/limits.hpp"

namespace fabric::capability {
namespace {

bool IsLowerAlpha(char ch) { return ch >= 'a' && ch <= 'z'; }
bool IsDigit(char ch) { return ch >= '0' && ch <= '9'; }
bool IsHexDigit(char ch) { return IsDigit(ch) || (ch >= 'a' && ch <= 'f'); }
bool IsSeparator(char ch) { return ch == '.' || ch == '-' || ch == '_'; }
/// Canonical entity names as issued by Fabric Registry may contain a slash,
/// for example a port named "1/1". Capability and vendor identifiers may not.
bool IsEntitySeparator(char ch) { return IsSeparator(ch) || ch == '/'; }
bool IsPrintable(char ch) {
  return static_cast<unsigned char>(ch) >= 0x20u && static_cast<unsigned char>(ch) <= 0x7Eu;
}

Outcome<void> TokenFailure(ErrorCode code, std::string_view what, std::string_view detail) {
  return Status::Failure(code, std::string("malformed ") + std::string(what),
                         std::string(detail));
}

std::vector<std::string_view> SplitSegments(std::string_view text) {
  std::vector<std::string_view> segments;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t dot = text.find('.', start);
    if (dot == std::string_view::npos) {
      segments.push_back(text.substr(start));
      break;
    }
    segments.push_back(text.substr(start, dot - start));
    start = dot + 1;
  }
  return segments;
}

}  // namespace

namespace detail {

Outcome<void> ValidateToken(std::string_view text, Charset charset, std::size_t min_length,
                            std::size_t max_length, std::string_view what) {
  if (text.size() < min_length || text.size() > max_length) {
    return TokenFailure(ErrorCode::MalformedIdentifier, what,
                        "length=" + std::to_string(text.size()) + " allowed=[" +
                            std::to_string(min_length) + "," + std::to_string(max_length) + "]");
  }
  switch (charset) {
    case Charset::LowerToken: {
      if (!IsLowerAlpha(text.front()) && !IsDigit(text.front())) {
        return TokenFailure(ErrorCode::MalformedIdentifier, what,
                            "must start with a lower case letter or digit");
      }
      if (!IsLowerAlpha(text.back()) && !IsDigit(text.back())) {
        return TokenFailure(ErrorCode::MalformedIdentifier, what,
                            "must end with a lower case letter or digit");
      }
      bool previous_separator = false;
      for (const char ch : text) {
        const bool separator = charset == Charset::LowerToken && IsEntitySeparator(ch);
        if (!IsLowerAlpha(ch) && !IsDigit(ch) && !separator) {
          return TokenFailure(ErrorCode::MalformedIdentifier, what,
                              "contains a character outside [a-z0-9._-]");
        }
        if (separator && previous_separator) {
          return TokenFailure(ErrorCode::MalformedIdentifier, what,
                              "contains consecutive separators");
        }
        previous_separator = separator;
      }
      return Status::Success();
    }
    case Charset::HexToken: {
      if (text.front() == '-' || text.back() == '-') {
        return TokenFailure(ErrorCode::MalformedIdentifier, what,
                            "must not start or end with a separator");
      }
      bool previous_separator = false;
      for (const char ch : text) {
        const bool separator = ch == '-';
        if (!IsHexDigit(ch) && !separator) {
          return TokenFailure(ErrorCode::MalformedIdentifier, what,
                              "contains a character outside [0-9a-f-]");
        }
        if (separator && previous_separator) {
          return TokenFailure(ErrorCode::MalformedIdentifier, what,
                              "contains consecutive separators");
        }
        previous_separator = separator;
      }
      return Status::Success();
    }
    case Charset::PrintableToken: {
      if (text.front() == ' ' || text.back() == ' ') {
        return TokenFailure(ErrorCode::MalformedIdentifier, what,
                            "must not start or end with a space");
      }
      for (const char ch : text) {
        if (!IsPrintable(ch)) {
          return TokenFailure(ErrorCode::MalformedIdentifier, what,
                              "contains a non printable character");
        }
      }
      return Status::Success();
    }
  }
  return TokenFailure(ErrorCode::MalformedIdentifier, what, "unknown character profile");
}

}  // namespace detail

namespace {

struct EntityKindName {
  FabricEntityKind kind;
  std::string_view name;
};

constexpr std::array<EntityKindName, 11> kEntityKindNames = {{
    {FabricEntityKind::Fabric, "fabric"},
    {FabricEntityKind::Site, "site"},
    {FabricEntityKind::Device, "device"},
    {FabricEntityKind::Switch, "switch"},
    {FabricEntityKind::Router, "router"},
    {FabricEntityKind::Nic, "nic"},
    {FabricEntityKind::SmartNic, "smartnic"},
    {FabricEntityKind::Dpu, "dpu"},
    {FabricEntityKind::Port, "port"},
    {FabricEntityKind::Link, "link"},
    {FabricEntityKind::Endpoint, "endpoint"},
}};

}  // namespace

std::string_view FabricEntityKindName(FabricEntityKind kind) noexcept {
  for (const EntityKindName& entry : kEntityKindNames) {
    if (entry.kind == kind) return entry.name;
  }
  return "unknown";
}

Outcome<FabricEntityKind> ParseFabricEntityKind(std::string_view text) {
  for (const EntityKindName& entry : kEntityKindNames) {
    if (entry.name == text) return entry.kind;
  }
  return Outcome<FabricEntityKind>::Failure(
      ErrorCode::MalformedIdentifier, "unknown fabric entity kind", std::string(text));
}

Outcome<EntityId> EntityId::Create(FabricEntityKind kind, std::string_view canonical_name) {
  if (kind == FabricEntityKind::Unknown) {
    return Outcome<EntityId>::Failure(ErrorCode::MalformedIdentifier,
                                      "entity kind must be a known fabric entity class");
  }
  auto valid = detail::ValidateToken(canonical_name, Charset::LowerToken, 1,
                                     limits::kMaxEntityNameLength, "entity name");
  if (!valid) return valid.GetError();
  EntityId id;
  id.kind_ = kind;
  id.canonical_.assign(canonical_name);
  id.rendered_.reserve(canonical_name.size() + 12);
  id.rendered_.assign(FabricEntityKindName(kind));
  id.rendered_.push_back(':');
  id.rendered_.append(canonical_name);
  return id;
}

Outcome<EntityId> EntityId::Parse(std::string_view text) {
  const std::size_t colon = text.find(':');
  if (colon == std::string_view::npos) {
    return Outcome<EntityId>::Failure(ErrorCode::MalformedIdentifier,
                                      "entity identifier must be <kind>:<canonical-name>",
                                      std::string(text));
  }
  auto kind = ParseFabricEntityKind(text.substr(0, colon));
  if (!kind) return kind.GetError();
  return Create(kind.Value(), text.substr(colon + 1));
}

Outcome<CapabilityNamespaceId> CapabilityNamespaceId::Parse(std::string_view text) {
  if (text.size() > limits::kMaxCapabilityIdLength) {
    return Outcome<CapabilityNamespaceId>::Failure(ErrorCode::MalformedIdentifier,
                                                   "capability namespace is too long");
  }
  const std::vector<std::string_view> segments = SplitSegments(text);
  const bool canonical = !segments.empty() && segments[0] == "fabric";
  const bool vendor = !segments.empty() && segments[0] == "vendor";
  if (!canonical && !vendor) {
    return Outcome<CapabilityNamespaceId>::Failure(
        ErrorCode::UnknownNamespace,
        "capability namespace must start with 'fabric' (vendor neutral) or 'vendor' (extension)",
        std::string(text));
  }
  if (canonical && segments.size() != 2) {
    return Outcome<CapabilityNamespaceId>::Failure(
        ErrorCode::MalformedIdentifier,
        "canonical capability namespace must be 'fabric.<domain>'", std::string(text));
  }
  if (vendor && segments.size() != 2 && segments.size() != 3) {
    return Outcome<CapabilityNamespaceId>::Failure(
        ErrorCode::MalformedIdentifier,
        "vendor capability namespace must be 'vendor.<vendor>' or 'vendor.<vendor>.<domain>'",
        std::string(text));
  }
  for (std::size_t index = 1; index < segments.size(); ++index) {
    auto valid = detail::ValidateToken(segments[index], Charset::LowerToken, 1,
                                       limits::kMaxNamespaceSegmentLength, "namespace segment");
    if (!valid) return valid.GetError();
  }
  CapabilityNamespaceId id;
  id.text_.assign(text);
  return id;
}

bool CapabilityNamespaceId::IsVendor(std::string_view text) noexcept {
  return text.size() >= 7 && text.compare(0, 7, "vendor.") == 0;
}

Outcome<CapabilityId> CapabilityId::Create(const CapabilityNamespaceId& ns,
                                           std::string_view local) {
  if (!ns.IsSet()) {
    return Outcome<CapabilityId>::Failure(ErrorCode::UnknownNamespace,
                                          "capability namespace is not set");
  }
  auto valid = detail::ValidateToken(local, Charset::LowerToken, 1,
                                     limits::kMaxCapabilityLocalLength, "capability local name");
  if (!valid) return valid.GetError();
  CapabilityId id;
  id.ns_ = ns;
  id.local_.assign(local);
  id.rendered_.reserve(ns.Value().size() + local.size() + 1);
  id.rendered_.append(ns.Value());
  id.rendered_.push_back('.');
  id.rendered_.append(local);
  if (id.rendered_.size() > limits::kMaxCapabilityIdLength) {
    return Outcome<CapabilityId>::Failure(ErrorCode::MalformedIdentifier,
                                          "capability identifier is too long",
                                          id.rendered_);
  }
  return id;
}

Outcome<CapabilityId> CapabilityId::Parse(std::string_view text) {
  if (text.empty() || text.size() > limits::kMaxCapabilityIdLength) {
    return Outcome<CapabilityId>::Failure(ErrorCode::MalformedIdentifier,
                                          "capability identifier length is out of range");
  }
  const std::vector<std::string_view> segments = SplitSegments(text);
  if (segments.size() < 3 || segments.size() > limits::kMaxCapabilityIdSegments) {
    return Outcome<CapabilityId>::Failure(
        ErrorCode::MalformedIdentifier,
        "capability identifier must have between three and six dot separated segments",
        std::string(text));
  }
  std::string_view namespace_text = text.substr(0, text.size() - segments.back().size() - 1);
  auto ns = CapabilityNamespaceId::Parse(namespace_text);
  if (!ns) return ns.GetError();
  return Create(ns.Value(), segments.back());
}

}  // namespace fabric::capability
