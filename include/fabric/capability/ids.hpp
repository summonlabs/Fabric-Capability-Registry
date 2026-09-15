// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "fabric/capability/error.hpp"

namespace fabric::capability {

/// Character profile of a textual identifier.
enum class Charset : std::uint8_t {
  /// Lower case ascii letters, digits and single '.', '-', '_' separators.
  LowerToken,
  /// Lower case hexadecimal digits with optional single '-' separators.
  HexToken,
  /// Printable ascii without whitespace or control characters.
  PrintableToken,
};

namespace detail {
/// Validates a textual identifier against a character profile. Declared here
/// because token parsing is a template; defined in ids.cpp.
Outcome<void> ValidateToken(std::string_view text, Charset charset, std::size_t min_length,
                            std::size_t max_length, std::string_view what);
}  // namespace detail

/// Strongly typed textual identifier. Distinct tags produce distinct,
/// non-interconvertible types; the type never converts implicitly to or from
/// c std::string.
template <class Tag, Charset CharsetValue, std::size_t MinLength, std::size_t MaxLength>
class TokenId {
 public:
  TokenId() = default;

  static Outcome<TokenId> Parse(std::string_view text) {
    auto valid = detail::ValidateToken(text, CharsetValue, MinLength, MaxLength, Tag::kKindName);
    if (!valid) return valid.GetError();
    TokenId id;
    id.value_.assign(text);
    return id;
  }

  bool IsSet() const noexcept { return !value_.empty(); }
  const std::string& Value() const noexcept { return value_; }
  std::string_view View() const noexcept { return value_; }
  std::string ToString() const { return value_; }

  friend bool operator==(const TokenId&, const TokenId&) = default;
  friend std::strong_ordering operator<=>(const TokenId& lhs, const TokenId& rhs) {
    return lhs.value_ <=> rhs.value_;
  }

  std::size_t Hash() const noexcept { return std::hash<std::string>{}(value_); }

 private:
  std::string value_;
};

/// Strongly typed monotonically increasing generation/epoch counter. Zero
/// means "unset"; c Next() never wraps and never silently saturates.
template <class Tag>
class Counter {
 public:
  using value_type = std::uint64_t;

  constexpr Counter() noexcept = default;

  /// Constructs a counter from an already validated numeric value. Counters
  /// are values, not identities: callers that need to reject absurd numbers
  /// do so at the boundaries (publication, persistence, wire).
  static constexpr Counter FromValue(std::uint64_t value) noexcept { return Counter(value); }

  constexpr std::uint64_t Value() const noexcept { return value_; }
  constexpr bool IsSet() const noexcept { return value_ != 0; }

  Outcome<Counter> Next() const {
    if (value_ == UINT64_MAX) {
      return Outcome<Counter>::Failure(ErrorCode::ArithmeticOverflow,
                                       "generation counter exhausted");
    }
    return Counter(value_ + 1);
  }

  std::string ToString() const { return std::to_string(value_); }

  friend constexpr bool operator==(Counter, Counter) noexcept = default;
  friend constexpr std::strong_ordering operator<=>(Counter, Counter) noexcept = default;

 private:
  explicit constexpr Counter(std::uint64_t value) noexcept : value_(value) {}
  std::uint64_t value_ = 0;
};

// ---------------------------------------------------------------------------
// Identity tags. Each tag carries the diagnostic kind name used in errors.
// ---------------------------------------------------------------------------

#define FCR_DECLARE_TAG(TagName, KindText)          \
  struct TagName {                                  \
    static constexpr const char* kKindName = KindText; \
  }

FCR_DECLARE_TAG(SourceIdTag, "source id");
FCR_DECLARE_TAG(PublisherIdTag, "publisher id");
FCR_DECLARE_TAG(AuthorityScopeIdTag, "authority scope id");
FCR_DECLARE_TAG(WorkerBootIdTag, "worker boot id");
FCR_DECLARE_TAG(CapabilityProfileIdTag, "capability profile id");
FCR_DECLARE_TAG(CompatibilityClassIdTag, "compatibility class id");
FCR_DECLARE_TAG(MutationAttemptIdTag, "mutation attempt id");
FCR_DECLARE_TAG(PublicationIdTag, "publication id");
FCR_DECLARE_TAG(SnapshotIdTag, "snapshot id");
FCR_DECLARE_TAG(CapabilityRecordIdTag, "capability record id");
FCR_DECLARE_TAG(EvidenceIdTag, "evidence id");
FCR_DECLARE_TAG(EnumDomainIdTag, "enum domain id");
FCR_DECLARE_TAG(VersionTokenTag, "version token");
FCR_DECLARE_TAG(ReasonTokenTag, "reason");
FCR_DECLARE_TAG(ProfileNameTag, "profile name");
FCR_DECLARE_TAG(RequirementNameTag, "requirement name");

#undef FCR_DECLARE_TAG

// ---------------------------------------------------------------------------
// Identifier aliases.
// ---------------------------------------------------------------------------

using SourceId = TokenId<SourceIdTag, Charset::LowerToken, 1, 96>;
using PublisherId = TokenId<PublisherIdTag, Charset::LowerToken, 1, 96>;
using AuthorityScopeId = TokenId<AuthorityScopeIdTag, Charset::LowerToken, 1, 96>;
using WorkerBootId = TokenId<WorkerBootIdTag, Charset::HexToken, 8, 64>;
using CapabilityProfileId = TokenId<CapabilityProfileIdTag, Charset::LowerToken, 1, 96>;
using CompatibilityClassId = TokenId<CompatibilityClassIdTag, Charset::LowerToken, 1, 96>;
using MutationAttemptId = TokenId<MutationAttemptIdTag, Charset::PrintableToken, 1, 96>;
using PublicationId = TokenId<PublicationIdTag, Charset::PrintableToken, 1, 96>;
using SnapshotId = TokenId<SnapshotIdTag, Charset::HexToken, 32, 32>;
using CapabilityRecordId = TokenId<CapabilityRecordIdTag, Charset::HexToken, 32, 32>;
using EvidenceId = TokenId<EvidenceIdTag, Charset::HexToken, 32, 32>;
using EnumDomainId = TokenId<EnumDomainIdTag, Charset::LowerToken, 1, 96>;
using VersionToken = TokenId<VersionTokenTag, Charset::PrintableToken, 1, 64>;
using ReasonToken = TokenId<ReasonTokenTag, Charset::PrintableToken, 1, 192>;
using ProfileName = TokenId<ProfileNameTag, Charset::PrintableToken, 1, 96>;
using RequirementName = TokenId<RequirementNameTag, Charset::PrintableToken, 1, 96>;

// ---------------------------------------------------------------------------
// Generation and epoch counters.
// ---------------------------------------------------------------------------

struct EntityGenerationTag {};
struct CapabilitySetGenerationTag {};
struct CapabilityGenerationTag {};
struct EvidenceGenerationTag {};
struct SourceGenerationTag {};
struct RegistryGenerationTag {};
struct ProfileGenerationTag {};
struct CoordinatorEpochTag {};

/// Generation of an entity as issued by Fabric Registry. Fabric Registry owns
/// canonical identity; this runtime only binds capability claims to the exact
/// generation it is told about and fences claims bound to superseded ones.
using EntityGeneration = Counter<EntityGenerationTag>;
/// Generation of an entity capability set. Advances exactly once per
/// committed semantic mutation of that set.
using CapabilitySetGeneration = Counter<CapabilitySetGenerationTag>;
/// Generation of a single capability inside a set. Advances when that
/// capability's resolved claim changes.
using CapabilityGeneration = Counter<CapabilityGenerationTag>;
/// Generation of the evidence a single source holds for one capability.
using EvidenceGeneration = Counter<EvidenceGenerationTag>;
/// Monotonic publication sequence of one source.
using SourceGeneration = Counter<SourceGenerationTag>;
/// Monotonic registry wide mutation counter, used for snapshot currentness.
using RegistryGeneration = Counter<RegistryGenerationTag>;
/// Generation of a capability profile definition.
using ProfileGeneration = Counter<ProfileGenerationTag>;
/// Coordinator epoch. Advanced by every fresh coordinator start; live
/// publication authority never survives an epoch advance.
using CoordinatorEpoch = Counter<CoordinatorEpochTag>;

// ---------------------------------------------------------------------------
// Entity identity (consumed from Fabric Registry).
// ---------------------------------------------------------------------------

/// Entity classes this runtime can bind capability claims to. The vocabulary
/// mirrors the canonical identity classes owned by Fabric Registry; this
/// runtime validates the shape of an identity token but never mints one.
enum class FabricEntityKind : std::uint8_t {
  Unknown = 0,
  Fabric,
  Site,
  Device,
  Switch,
  Router,
  Nic,
  SmartNic,
  Dpu,
  Port,
  Link,
  Endpoint,
};

std::string_view FabricEntityKindName(FabricEntityKind kind) noexcept;
Outcome<FabricEntityKind> ParseFabricEntityKind(std::string_view text);

/// Canonical entity identity: a Fabric Registry issued entity class plus a
/// Fabric Registry issued canonical name. The rendered form is
/// "<kind>:<canonical-name>".
class EntityId {
 public:
  EntityId() = default;

  /// Parses "<kind>:<canonical-name>".
  static Outcome<EntityId> Parse(std::string_view text);
  /// Builds an identity from a class and a canonical name.
  static Outcome<EntityId> Create(FabricEntityKind kind, std::string_view canonical_name);

  bool IsSet() const noexcept { return kind_ != FabricEntityKind::Unknown; }
  FabricEntityKind Kind() const noexcept { return kind_; }
  const std::string& CanonicalName() const noexcept { return canonical_; }
  const std::string& ToString() const noexcept { return rendered_; }
  std::string_view View() const noexcept { return rendered_; }

  friend bool operator==(const EntityId&, const EntityId&) = default;
  friend std::strong_ordering operator<=>(const EntityId& lhs, const EntityId& rhs) {
    return lhs.rendered_ <=> rhs.rendered_;
  }
  std::size_t Hash() const noexcept { return std::hash<std::string>{}(rendered_); }

 private:
  FabricEntityKind kind_ = FabricEntityKind::Unknown;
  std::string canonical_;
  std::string rendered_;
};

// ---------------------------------------------------------------------------
// Capability identity.
// ---------------------------------------------------------------------------

/// Explicit capability namespace. Canonical (vendor neutral) namespaces are
/// "fabric.<domain>"; vendor extensions are "vendor.<vendor>[.<domain>]".
class CapabilityNamespaceId {
 public:
  CapabilityNamespaceId() = default;

  static Outcome<CapabilityNamespaceId> Parse(std::string_view text);

  bool IsSet() const noexcept { return !text_.empty(); }
  bool IsVendorExtension() const noexcept { return IsVendor(text_); }
  const std::string& Value() const noexcept { return text_; }
  std::string_view View() const noexcept { return text_; }
  std::string ToString() const { return text_; }

  static bool IsVendor(std::string_view text) noexcept;

  friend bool operator==(const CapabilityNamespaceId&, const CapabilityNamespaceId&) = default;
  friend std::strong_ordering operator<=>(const CapabilityNamespaceId& lhs,
                                          const CapabilityNamespaceId& rhs) {
    return lhs.text_ <=> rhs.text_;
  }
  std::size_t Hash() const noexcept { return std::hash<std::string>{}(text_); }

 private:
  std::string text_;
};

/// Fully qualified capability identifier: "<namespace>.<local-name>".
class CapabilityId {
 public:
  CapabilityId() = default;

  static Outcome<CapabilityId> Parse(std::string_view text);
  static Outcome<CapabilityId> Create(const CapabilityNamespaceId& ns, std::string_view local);

  bool IsSet() const noexcept { return !rendered_.empty(); }
  bool IsVendorExtension() const noexcept { return ns_.IsVendorExtension(); }
  const CapabilityNamespaceId& Namespace() const noexcept { return ns_; }
  const std::string& LocalName() const noexcept { return local_; }
  const std::string& ToString() const noexcept { return rendered_; }
  std::string_view View() const noexcept { return rendered_; }

  friend bool operator==(const CapabilityId&, const CapabilityId&) = default;
  friend std::strong_ordering operator<=>(const CapabilityId& lhs, const CapabilityId& rhs) {
    return lhs.rendered_ <=> rhs.rendered_;
  }
  std::size_t Hash() const noexcept { return std::hash<std::string>{}(rendered_); }

 private:
  CapabilityNamespaceId ns_;
  std::string local_;
  std::string rendered_;
};

}  // namespace fabric::capability

namespace std {
template <class Tag, fabric::capability::Charset CS, size_t MinL, size_t MaxL>
struct hash<fabric::capability::TokenId<Tag, CS, MinL, MaxL>> {
  size_t operator()(const fabric::capability::TokenId<Tag, CS, MinL, MaxL>& id) const noexcept {
    return id.Hash();
  }
};
template <class Tag>
struct hash<fabric::capability::Counter<Tag>> {
  size_t operator()(const fabric::capability::Counter<Tag>& counter) const noexcept {
    return std::hash<uint64_t>{}(counter.Value());
  }
};
template <>
struct hash<fabric::capability::EntityId> {
  size_t operator()(const fabric::capability::EntityId& id) const noexcept { return id.Hash(); }
};
template <>
struct hash<fabric::capability::CapabilityNamespaceId> {
  size_t operator()(const fabric::capability::CapabilityNamespaceId& id) const noexcept {
    return id.Hash();
  }
};
template <>
struct hash<fabric::capability::CapabilityId> {
  size_t operator()(const fabric::capability::CapabilityId& id) const noexcept { return id.Hash(); }
};
}  // namespace std
