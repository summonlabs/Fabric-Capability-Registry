// Fabric Capability Registry inspection CLI.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Deterministic, script friendly inspection of capability truth: REAL host
// discovery, synthetic profiles, persisted stores, explanations, snapshots,
// diffs and bounded compatibility queries.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "fabric/capability/fabric_capability.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

using namespace fabric::capability;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitLookup = 3;
constexpr int kExitStore = 4;

struct Arguments {
  std::vector<std::string> items;

  bool Has(const std::string& name) const {
    return std::find(items.begin(), items.end(), name) != items.end();
  }

  std::string Value(const std::string& name, const std::string& fallback = std::string()) const {
    for (std::size_t index = 0; index + 1 < items.size(); ++index) {
      if (items[index] == name) return items[index + 1];
    }
    return fallback;
  }

  std::vector<std::string> Values(const std::string& name) const {
    std::vector<std::string> values;
    for (std::size_t index = 0; index + 1 < items.size(); ++index) {
      if (items[index] == name) values.push_back(items[index + 1]);
    }
    return values;
  }
};

bool ParseU64(const std::string& text, std::uint64_t* out) {
  if (text.empty()) return false;
  std::uint64_t value = 0;
  for (const char ch : text) {
    if (ch < '0' || ch > '9') return false;
    value = value * 10u + static_cast<std::uint64_t>(ch - '0');
    if (value > limits::kMaxQuantity) return false;
  }
  *out = value;
  return true;
}

void Usage() {
  std::fprintf(stderr,
               "fcrctl - Fabric Capability Registry inspection tool\n"
               "\n"
               "  version\n"
               "  discover [--include-loopback] [--no-pnp] [--max-adapters N] [--json]\n"
               "  synthetic list\n"
               "  synthetic show --class NAME [--devices N] [--seed N] [--conflicts] [--changing] [--json]\n"
               "  import-synthetic --store DIR [--class NAME] [--devices N] [--seed N] [--conflicts] [--changing]\n"
               "  import-discovery --store DIR [--include-loopback] [--no-pnp]\n"
               "  entities --store DIR [--json]\n"
               "  entity --store DIR --entity KIND:NAME\n"
               "  capability --store DIR --entity KIND:NAME [--generation N] --capability ID [--json]\n"
               "  evidence --store DIR --entity KIND:NAME --capability ID\n"
               "  explain --store DIR --entity KIND:NAME --capability ID\n"
               "  generation --store DIR [--entity KIND:NAME]\n"
               "  snapshot --store DIR [--entity KIND:NAME] [--namespace NS] [--json]\n"
               "  stats --store DIR [--json]\n"
               "  diff --store DIR --against-store DIR2 [--entity KIND:NAME]\n"
               "  query --store DIR --entity KIND:NAME --require \"EXPR\" [--json]\n"
               "  inspect --store DIR [--json]\n"
               "\n"
               "EXPR is a ';' separated conjunction of leaves:\n"
               "  capability=STATE              STATE in supported|unsupported|unknown|\n"
               "                                revalidation-required|conflicted\n"
               "  capability>=NUMBER[UNIT]      quantity minimum\n"
               "  capability<=NUMBER[UNIT]      quantity maximum\n"
               "  capability contains PROTOCOL  e.g. 'fabric.protocol.families contains ethernet'\n"
               "  NUMERIC suffixes: k, M, G, T (1000 based) and Ki, Mi, Gi, Ti (1024 based).\n"
               "Exit codes: 0 success, 2 usage error, 3 lookup or query failure, 4 store failure.\n");
}

int Fail(int code, const std::string& message) {
  std::fprintf(stderr, "error: %s\n", message.c_str());
  return code;
}

PersistenceConfig StoreConfig(const Arguments& arguments, const std::string& flag = "--store") {
  PersistenceConfig config;
  config.directory = arguments.Value(flag);
  config.file_stem = "fabric-capability-registry";
  return config;
}

Status LoadStore(const Arguments& arguments, CapabilityRegistry& registry) {
  const PersistenceConfig config = StoreConfig(arguments);
  if (config.directory.empty()) {
    return Status::Failure(ErrorCode::InvalidArgument, "--store DIR is required");
  }
  auto loaded = registry.Load(config);
  if (!loaded.HasValue()) return loaded.GetError();
  return Status::Success();
}

/// Fresh worker boot identity for one import run. A capability import is a
/// publishing worker like any other, so it mints a new boot per process and
/// never reuses the authority of a previous run.
WorkerBootId FreshBoot() {
  std::vector<std::byte> bytes;
  ByteWriter writer(bytes);
  writer.U64(static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count()));
  writer.U64(static_cast<std::uint64_t>(
#if defined(_WIN32)
      GetCurrentProcessId()
#else
      static_cast<unsigned long>(::getpid())
#endif
      ));
  writer.U64(std::hash<std::thread::id>{}(std::this_thread::get_id()));
  const Digest digest = ComputeDigest(bytes);
  auto parsed = WorkerBootId::Parse(digest.ShortString());
  return parsed.HasValue() ? parsed.Value() : WorkerBootId{};
}

// --- query expression parsing ---------------------------------------------

std::string Trim(const std::string& text) {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
  while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) --end;
  return text.substr(begin, end - begin);
}

std::vector<std::string> Split(const std::string& text, char separator) {
  std::vector<std::string> parts;
  std::string current;
  for (const char ch : text) {
    if (ch == separator) {
      parts.push_back(current);
      current.clear();
      continue;
    }
    current.push_back(ch);
  }
  parts.push_back(current);
  return parts;
}

bool ParseQuantity(const std::string& text, const std::string& default_unit, CapabilityValue* out) {
  const std::string trimmed = Trim(text);
  std::size_t split = trimmed.size();
  while (split > 0 && ((trimmed[split - 1] >= 'a' && trimmed[split - 1] <= 'z') ||
                       (trimmed[split - 1] >= 'A' && trimmed[split - 1] <= 'Z') ||
                       trimmed[split - 1] == '/')) {
    --split;
  }
  const std::string number = trimmed.substr(0, split);
  std::string unit = Trim(trimmed.substr(split));
  if (unit.empty()) unit = default_unit;
  std::uint64_t multiplier = 1;
  std::string digits = number;
  const std::vector<std::pair<std::string, std::uint64_t>> suffixes = {
      {"Ki", 1024ull},        {"Mi", 1024ull * 1024},        {"Gi", 1024ull * 1024 * 1024},
      {"Ti", 1024ull * 1024 * 1024 * 1024}, {"k", 1000ull}, {"M", 1000ull * 1000},
      {"G", 1000ull * 1000 * 1000},         {"T", 1000ull * 1000 * 1000 * 1000}};
  for (const auto& suffix : suffixes) {
    if (digits.size() > suffix.first.size() &&
        digits.compare(digits.size() - suffix.first.size(), suffix.first.size(), suffix.first) == 0) {
      multiplier = suffix.second;
      digits = digits.substr(0, digits.size() - suffix.first.size());
      break;
    }
  }
  std::uint64_t value = 0;
  if (!ParseU64(digits, &value)) return false;
  const auto product = CheckedMul(value, multiplier);
  if (!product.HasValue() || product.Value() > limits::kMaxQuantity) return false;
  auto parsed_unit = ParseUnit(unit);
  if (!parsed_unit.HasValue()) return false;
  auto built = CapabilityValue::QuantityValue(product.Value(), parsed_unit.Value());
  if (!built.HasValue()) return false;
  *out = built.Value();
  return true;
}

bool BuildRequirement(const std::string& expression, Requirement* out, std::string* error) {
  out->kind = RequirementKind::AllOf;
  for (const std::string& raw_leaf : Split(expression, ';')) {
    const std::string leaf = Trim(raw_leaf);
    if (leaf.empty()) continue;
    Requirement requirement;
    std::string capability_text;
    std::string remainder;
    const std::size_t ge = leaf.find(">=");
    const std::size_t le = leaf.find("<=");
    const std::size_t equals = leaf.find('=');
    const std::size_t contains = leaf.find(" contains ");
    if (ge != std::string::npos) {
      requirement.kind = RequirementKind::Minimum;
      capability_text = Trim(leaf.substr(0, ge));
      remainder = leaf.substr(ge + 2);
    } else if (le != std::string::npos) {
      requirement.kind = RequirementKind::Maximum;
      capability_text = Trim(leaf.substr(0, le));
      remainder = leaf.substr(le + 2);
    } else if (contains != std::string::npos) {
      requirement.kind = RequirementKind::ProtocolSupported;
      capability_text = Trim(leaf.substr(0, contains));
      const std::string protocol = Trim(leaf.substr(contains + 10));
      auto parsed = ParseProtocol(protocol);
      if (!parsed.HasValue()) {
        *error = "unknown protocol '" + protocol + "'";
        return false;
      }
      requirement.operand_protocol = parsed.Value();
    } else if (equals != std::string::npos) {
      requirement.kind = RequirementKind::StateIs;
      capability_text = Trim(leaf.substr(0, equals));
      const std::string state = Trim(leaf.substr(equals + 1));
      auto parsed = ParseCapabilityState(state);
      if (!parsed.HasValue()) {
        *error = "unknown capability state '" + state + "'";
        return false;
      }
      requirement.required_state = parsed.Value();
    } else {
      *error = "cannot parse requirement leaf '" + leaf + "'";
      return false;
    }
    auto capability = CapabilityId::Parse(capability_text);
    if (!capability.HasValue()) {
      *error = "malformed capability identifier '" + capability_text + "'";
      return false;
    }
    requirement.capability = capability.Value();
    if (requirement.kind == RequirementKind::Minimum ||
        requirement.kind == RequirementKind::Maximum) {
      CapabilityValue operand;
      if (!ParseQuantity(remainder, std::string(), &operand)) {
        auto with_unit = ParseQuantity(remainder, std::string("count"), &operand);
        if (!with_unit) {
          *error = "cannot parse quantity '" + Trim(remainder) + "'";
          return false;
        }
      }
      requirement.operand = operand;
    }
    requirement.name = *RequirementName::Parse(leaf.substr(0, 96));
    out->children.push_back(std::move(requirement));
  }
  if (out->children.empty()) {
    *error = "the requirement expression is empty";
    return false;
  }
  return true;
}

// --- rendering helpers -----------------------------------------------------

std::string JsonEscape(const std::string& text) {
  std::string out;
  for (const char ch : text) {
    switch (ch) {
      case '"':
        out.append("\\\"");
        break;
      case '\\':
        out.append("\\\\");
        break;
      case '\n':
        out.append("\\n");
        break;
      case '\t':
        out.append("\\t");
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  return out;
}

void PrintEntities(const CapabilityRegistry& registry) {
  for (const EntityId& entity : registry.Entities()) {
    auto set = registry.QueryEntity(entity);
    if (!set.HasValue()) continue;
    const EntityCapabilitySet& capabilities = set.Value();
    std::printf(
        "entity=%s generation=%s set_generation=%s capabilities=%zu supported=%zu unsupported=%zu "
        "unknown=%zu revalidation_required=%zu conflicted=%zu digest=%s\n",
        entity.ToString().c_str(), capabilities.entity_generation.ToString().c_str(),
        capabilities.set_generation.ToString().c_str(), capabilities.capabilities.size(),
        capabilities.CountOf(CapabilityState::Supported),
        capabilities.CountOf(CapabilityState::Unsupported),
        capabilities.CountOf(CapabilityState::Unknown),
        capabilities.CountOf(CapabilityState::RevalidationRequired),
        capabilities.CountOf(CapabilityState::Conflicted), capabilities.digest.ToString().c_str());
  }
}

void PrintStats(const CapabilityRegistry& registry) {
  const RegistryStatistics stats = registry.Statistics();
  std::printf("entities=%zu\n", stats.entities);
  std::printf("capability_records=%zu\n", stats.capability_records);
  std::printf("evidence_records=%zu\n", stats.evidence_records);
  std::printf("supported=%zu\n", stats.supported);
  std::printf("unsupported=%zu\n", stats.unsupported);
  std::printf("unknown=%zu\n", stats.unknown);
  std::printf("revalidation_required=%zu\n", stats.revalidation_required);
  std::printf("conflicted=%zu\n", stats.conflicted);
  std::printf("authority_scopes=%zu\n", stats.authority_scopes);
  std::printf("registered_publishers=%zu\n", stats.registered_publishers);
  std::printf("fenced_worker_boots=%zu\n", stats.fenced_worker_boots);
  std::printf("vendor_descriptors=%zu\n", stats.vendor_descriptors);
  std::printf("replay_entries=%zu\n", stats.replay_entries);
  std::printf("rejection_journal_entries=%zu\n", stats.rejection_journal_entries);
  std::printf("retired_generations=%zu\n", stats.retired_generations);
  std::printf("registry_generation=%s\n", stats.registry_generation.ToString().c_str());
  std::printf("epoch=%s\n", stats.epoch.ToString().c_str());
}

int ImportSynthetic(const Arguments& arguments) {
  const std::string directory = arguments.Value("--store");
  if (directory.empty()) return Fail(kExitUsage, "--store DIR is required");
  auto device_class = ParseSyntheticDeviceClass(arguments.Value("--class", "leaf-switch"));
  if (!device_class.HasValue()) return Fail(kExitUsage, "unknown synthetic device class");

  SyntheticOptions options;
  std::uint64_t devices = 1;
  if (arguments.Has("--devices") && !ParseU64(arguments.Value("--devices"), &devices)) {
    return Fail(kExitUsage, "--devices must be a number");
  }
  options.device_count = static_cast<std::size_t>(devices);
  std::uint64_t seed = 1;
  if (arguments.Has("--seed") && !ParseU64(arguments.Value("--seed"), &seed)) {
    return Fail(kExitUsage, "--seed must be a number");
  }
  options.seed = seed;
  options.include_conflicting_source = arguments.Has("--conflicts");
  options.include_changing_source = arguments.Has("--changing");

  const std::vector<SyntheticPublication> publications =
      BuildSyntheticFabric(device_class.Value(), options);
  if (publications.empty()) return Fail(kExitStore, "the synthetic profile produced no data");

  CapabilityRegistry registry;
  PersistenceConfig config;
  config.directory = directory;
  config.file_stem = "fabric-capability-registry";
  auto loaded = registry.Load(config);
  if (!loaded.HasValue()) return Fail(kExitStore, loaded.GetError().ToString());

  // A synthetic model is a static declaration, so it is published as a durable
  // synthetic profile; it is never presented as physical proof.
  const SyntheticPublication& first = publications.front();
  AuthorityGrant grant;
  grant.scope = first.scope;
  grant.entity_kinds = {FabricEntityKind::Nic, FabricEntityKind::Switch, FabricEntityKind::Port,
                        FabricEntityKind::Device, FabricEntityKind::SmartNic,
                        FabricEntityKind::Dpu, FabricEntityKind::Router};
  for (const CapabilityDescriptor& descriptor : registry.Schema().Descriptors()) {
    if (std::find(grant.namespaces.begin(), grant.namespaces.end(), descriptor.id.Namespace()) ==
        grant.namespaces.end()) {
      grant.namespaces.push_back(descriptor.id.Namespace());
    }
  }
  grant.modes = static_cast<std::uint8_t>(PublicationMode::FullSnapshot) |
                static_cast<std::uint8_t>(PublicationMode::Incremental) |
                static_cast<std::uint8_t>(PublicationMode::PartialObservation);
  grant.may_publish_durable = true;
  grant.strongest_provenance = ProvenanceClass::SyntheticTestBackend;
  grant.allowed_publishers = {first.publisher};
  grant.description = *ReasonToken::Parse("synthetic capability import");
  auto declared = registry.DeclareAuthority(grant);
  if (!declared.HasValue()) return Fail(kExitStore, declared.GetError().ToString());
  auto registered = registry.RegisterPublisher(first.publisher, grant.scope, FreshBoot());
  if (!registered.HasValue()) return Fail(kExitStore, registered.GetError().ToString());

  std::size_t claims = 0;
  std::size_t entities = 0;
  for (const SyntheticPublication& publication : publications) {
    PublicationRequest request;
    request.mode = PublicationMode::PartialObservation;
    const std::string stem = "import-" + publication.entity.CanonicalName() + "-" +
                             publication.source.Value();
    auto publication_id = PublicationId::Parse(stem);
    auto attempt_id = MutationAttemptId::Parse(stem);
    if (!publication_id.HasValue() || !attempt_id.HasValue()) {
      return Fail(kExitStore, "deterministic identifiers could not be built");
    }
    request.publication = publication_id.Value();
    request.attempt = attempt_id.Value();
    request.epoch = registry.CurrentEpoch();
    request.authority = AuthorityContext{grant.scope, first.publisher, registered.Value().worker_boot,
                                        publication.source, SourceGeneration::FromValue(1)};
    request.entity = publication.entity;
    request.entity_generation = publication.entity_generation;
    request.coverage = publication.coverage;
    request.claims = publication.claims;
    auto record = registry.EntityRecord(publication.entity);
    if (record.HasValue() && record.Value().has_current_set) {
      request.expected_set_generation = record.Value().current_set.set_generation;
    }
    auto result = registry.Publish(request);
    if (!result.HasValue()) return Fail(kExitStore, result.GetError().ToString());
    if (result.Value().Rejected()) {
      return Fail(kExitStore, "publication rejected: " + result.Value().message);
    }
    claims += publication.claims.size();
    ++entities;
  }

  auto saved = registry.Save(config);
  if (!saved.HasValue()) return Fail(kExitStore, saved.GetError().ToString());
  std::printf("imported entities=%zu claims=%zu\n", entities, claims);
  std::printf("store=%s\n", saved.Value().path.string().c_str());
  std::printf("digest=%s\n", registry.ComputeDigest().ToString().c_str());
  return kExitOk;
}

}  // namespace

int main(int argc, char** argv) {
  Arguments arguments;
  for (int index = 1; index < argc; ++index) arguments.items.emplace_back(argv[index]);
  if (arguments.items.empty()) {
    Usage();
    return kExitUsage;
  }
  const std::string command = arguments.items.front();

  if (command == "--help" || command == "-h" || command == "help") {
    Usage();
    return kExitOk;
  }
  if (command == "version") {
    std::printf("%s %s\n", kLibraryName, kVersionString);
    return kExitOk;
  }
  if (command == "synthetic") {
    const std::string sub = arguments.items.size() > 1 ? arguments.items[1] : std::string();
    if (sub == "list") {
      for (const SyntheticDeviceClass device_class : SyntheticDeviceClasses()) {
        std::printf("class=%s\n", std::string(SyntheticDeviceClassName(device_class)).c_str());
      }
      return kExitOk;
    }
    if (sub == "show") {
      auto device_class = ParseSyntheticDeviceClass(arguments.Value("--class"));
      if (!device_class.HasValue()) return Fail(kExitUsage, "--class NAME is required");
      SyntheticOptions options;
      std::uint64_t devices = 1;
      if (arguments.Has("--devices") && !ParseU64(arguments.Value("--devices"), &devices)) {
        return Fail(kExitUsage, "--devices must be a number");
      }
      options.device_count = static_cast<std::size_t>(devices);
      std::uint64_t seed = 1;
      if (arguments.Has("--seed") && !ParseU64(arguments.Value("--seed"), &seed)) {
        return Fail(kExitUsage, "--seed must be a number");
      }
      options.seed = seed;
      options.include_conflicting_source = arguments.Has("--conflicts");
      options.include_changing_source = arguments.Has("--changing");
      const std::vector<SyntheticPublication> publications =
          BuildSyntheticFabric(device_class.Value(), options);
      for (const SyntheticPublication& publication : publications) {
        std::printf("entity=%s generation=%s source=%s publisher=%s provenance=%s coverage=%s claims=%zu\n",
                    publication.entity.ToString().c_str(),
                    publication.entity_generation.ToString().c_str(),
                    publication.source.Value().c_str(), publication.publisher.Value().c_str(),
                    std::string(ProvenanceClassName(publication.provenance)).c_str(),
                    std::string(CoverageName(publication.coverage)).c_str(),
                    publication.claims.size());
        for (const CapabilityClaim& claim : publication.claims) {
          std::printf("  claim=%s state=%s value=%s\n", claim.capability.ToString().c_str(),
                      std::string(CapabilityStateName(claim.state)).c_str(),
                      claim.value.IsAbsent() ? "absent" : claim.value.ToText().c_str());
        }
      }
      return kExitOk;
    }
    Usage();
    return kExitUsage;
  }
  if (command == "import-synthetic") return ImportSynthetic(arguments);
  if (command == "import-discovery") {
    const std::string directory = arguments.Value("--store");
    if (directory.empty()) return Fail(kExitUsage, "--store DIR is required");
    HostDiscoveryOptions options;
    options.include_loopback = arguments.Has("--include-loopback");
    options.include_pnp_properties = !arguments.Has("--no-pnp");
    auto report = DiscoverHostCapabilities(options);
    if (!report.HasValue()) return Fail(kExitStore, report.GetError().ToString());

    CapabilityRegistry registry;
    PersistenceConfig config;
    config.directory = directory;
    config.file_stem = "fabric-capability-registry";
    auto loaded = registry.Load(config);
    if (!loaded.HasValue()) return Fail(kExitStore, loaded.GetError().ToString());

    const PublisherId publisher = *PublisherId::Parse("host-discovery");
    const AuthorityScopeId scope = *AuthorityScopeId::Parse("host-discovery-scope");
    const SourceId source = *SourceId::Parse("host-discovery-source");
    const WorkerBootId boot = FreshBoot();
    AuthorityGrant grant;
    grant.scope = scope;
    grant.entity_kinds = {FabricEntityKind::Nic};
    for (const CapabilityDescriptor& descriptor : registry.Schema().Descriptors()) {
      if (std::find(grant.namespaces.begin(), grant.namespaces.end(), descriptor.id.Namespace()) ==
          grant.namespaces.end()) {
        grant.namespaces.push_back(descriptor.id.Namespace());
      }
    }
    grant.modes = static_cast<std::uint8_t>(PublicationMode::FullSnapshot) |
                  static_cast<std::uint8_t>(PublicationMode::PartialObservation);
    grant.strongest_provenance = ProvenanceClass::DirectDeviceOrOsApi;
    grant.allowed_publishers = {publisher};
    grant.may_publish_durable = false;
    auto declared = registry.DeclareAuthority(grant);
    if (!declared.HasValue()) return Fail(kExitStore, declared.GetError().ToString());
    auto registered = registry.RegisterPublisher(publisher, scope, boot);
    if (!registered.HasValue()) return Fail(kExitStore, registered.GetError().ToString());

    std::size_t facts = 0;
    std::size_t not_reported = 0;
    std::size_t entities = 0;
    for (const DiscoveredEntity& entity : report.Value().entities) {
      PublicationRequest request;
      request.mode = PublicationMode::PartialObservation;
      const std::string stem = "discovery-" + entity.entity.CanonicalName();
      request.publication = PublicationId::Parse(stem).Value();
      request.attempt = MutationAttemptId::Parse(stem).Value();
      request.epoch = registry.CurrentEpoch();
      request.authority = AuthorityContext{scope, publisher, boot, source,
                                           SourceGeneration::FromValue(1)};
      request.entity = entity.entity;
      request.entity_generation = EntityGeneration::FromValue(1);
      request.coverage = Coverage::Partial;
      for (const DiscoveredFact& fact : entity.facts) {
        CapabilityClaim claim;
        claim.capability = fact.capability;
        claim.state = fact.state;
        claim.value = fact.value;
        claim.provenance = entity.provenance;
        claim.source_class = entity.source_class;
        claim.durability = DurabilityClass::ProcessBound;
        claim.coverage = Coverage::Partial;
        if (fact.status != DiscoveryFactStatus::Reported) ++not_reported;
        request.claims.push_back(std::move(claim));
      }
      auto record = registry.EntityRecord(entity.entity);
      if (record.HasValue() && record.Value().has_current_set) {
        request.expected_set_generation = record.Value().current_set.set_generation;
      }
      auto result = registry.Publish(request);
      if (!result.HasValue()) return Fail(kExitStore, result.GetError().ToString());
      facts += entity.facts.size();
      ++entities;
    }
    auto saved = registry.Save(config);
    if (!saved.HasValue()) return Fail(kExitStore, saved.GetError().ToString());
    std::printf("imported entities=%zu facts=%zu not_reported=%zu\n", entities, facts, not_reported);
    std::printf("store=%s\n", saved.Value().path.string().c_str());
    std::printf("digest=%s\n", registry.ComputeDigest().ToString().c_str());
    return kExitOk;
  }

  CapabilityRegistry registry;
  if (command == "discover") {
    HostDiscoveryOptions options;
    options.include_loopback = arguments.Has("--include-loopback");
    options.include_pnp_properties = !arguments.Has("--no-pnp");
    if (arguments.Has("--max-adapters")) {
      std::uint64_t adapters = 0;
      if (!ParseU64(arguments.Value("--max-adapters"), &adapters)) {
        return Fail(kExitUsage, "--max-adapters must be a number");
      }
      options.max_adapters = static_cast<std::size_t>(adapters);
    }
    auto report = DiscoverHostCapabilities(options);
    if (!report.HasValue()) return Fail(kExitLookup, report.GetError().ToString());
    if (arguments.Has("--json")) {
      std::printf("{\n  \"adapters\": %zu,\n  \"entities\": [\n",
                  report.Value().entities.size());
      for (std::size_t index = 0; index < report.Value().entities.size(); ++index) {
        const DiscoveredEntity& entity = report.Value().entities[index];
        std::printf("    {\n      \"entity\": \"%s\",\n      \"facts\": [\n",
                    JsonEscape(entity.entity.ToString()).c_str());
        for (std::size_t fact_index = 0; fact_index < entity.facts.size(); ++fact_index) {
          const DiscoveredFact& fact = entity.facts[fact_index];
          std::printf(
              "        {\"capability\": \"%s\", \"note\": \"%s\", \"state\": \"%s\", "
              "\"status\": \"%s\", \"value\": \"%s\"}%s\n",
              JsonEscape(fact.capability.ToString()).c_str(), JsonEscape(fact.note).c_str(),
              std::string(CapabilityStateName(fact.state)).c_str(),
              std::string(DiscoveryFactStatusName(fact.status)).c_str(),
              JsonEscape(fact.has_value ? fact.value.ToText() : std::string("absent")).c_str(),
              fact_index + 1 == entity.facts.size() ? "" : ",");
        }
        std::printf("      ]\n    }%s\n", index + 1 == report.Value().entities.size() ? "" : ",");
      }
      std::printf("  ],\n  \"platform\": \"%s\"\n}\n", JsonEscape(report.Value().platform).c_str());
      return kExitOk;
    }
    std::printf("%s\n", RenderDiscoveryReport(report.Value()).c_str());
    return kExitOk;
  }
  if (command == "inspect") {
    const PersistenceConfig config = StoreConfig(arguments);
    if (config.directory.empty()) return Fail(kExitUsage, "--store DIR is required");
    auto inspection = InspectStore(config);
    if (!inspection.HasValue()) return Fail(kExitStore, inspection.GetError().ToString());
    std::printf("format_version=%u\n", inspection.Value().format_version);
    std::printf("flags=%u\n", inspection.Value().flags);
    std::printf("payload_bytes=%llu\n",
                static_cast<unsigned long long>(inspection.Value().payload_bytes));
    std::printf("entity_records=%zu\n", inspection.Value().entity_records);
    std::printf("evidence_records=%zu\n", inspection.Value().evidence_records);
    std::printf("retired_generations=%zu\n", inspection.Value().retired_generations);
    std::printf("fence_records=%zu\n", inspection.Value().fence_records);
    std::printf("authority_grants=%zu\n", inspection.Value().authority_grants);
    std::printf("registry_generation=%s\n", inspection.Value().registry_generation.ToString().c_str());
    std::printf("epoch=%s\n", inspection.Value().epoch.ToString().c_str());
    std::printf("payload_digest=%s\n", inspection.Value().payload_digest.ToString().c_str());
    std::printf("integrity=%s\n", inspection.Value().integrity_ok ? "ok" : "corrupt");
    std::printf("store=%s\n", inspection.Value().path.string().c_str());
    return kExitOk;
  }

  // Command validation precedes any store access, so an unknown command is a usage error and
  // never a store error.
  const bool known_command =
      command == "entities" || command == "entity" || command == "capability" ||
      command == "evidence" || command == "explain" || command == "generation" ||
      command == "snapshot" || command == "stats" || command == "diff" || command == "query";
  if (!known_command) {
    Usage();
    return kExitUsage;
  }

  auto loaded = LoadStore(arguments, registry);
  if (!loaded.HasValue()) {
    return Fail(loaded.Code() == ErrorCode::InvalidArgument ? kExitUsage : kExitStore,
                loaded.GetError().ToString());
  }

  if (command == "entities") {
    PrintEntities(registry);
    return kExitOk;
  }
  if (command == "stats") {
    PrintStats(registry);
    return kExitOk;
  }
  if (command == "entity") {
    auto entity = EntityId::Parse(arguments.Value("--entity"));
    if (!entity.HasValue()) return Fail(kExitUsage, "--entity KIND:NAME is required");
    auto set = registry.QueryEntity(entity.Value());
    if (!set.HasValue()) return Fail(kExitLookup, set.GetError().ToString());
    std::printf("%s\n", set.Value().ToText().c_str());
    return kExitOk;
  }
  if (command == "capability") {
    auto entity = EntityId::Parse(arguments.Value("--entity"));
    auto capability = CapabilityId::Parse(arguments.Value("--capability"));
    if (!entity.HasValue() || !capability.HasValue()) {
      return Fail(kExitUsage, "--entity KIND:NAME and --capability ID are required");
    }
    auto result = registry.Query(entity.Value(), capability.Value());
    if (!result.HasValue()) return Fail(kExitLookup, result.GetError().ToString());
    const CapabilityQueryResult& query = result.Value();
    if (!query.record_exists) {
      std::printf("capability=%s state=%s\n", capability.Value().ToString().c_str(),
                  std::string(CapabilityStateName(query.state)).c_str());
      return kExitLookup;
    }
    std::printf(
        "entity=%s capability=%s state=%s value=%s actionable=%s fails_closed=%s "
        "set_generation=%s capability_generation=%s registry_generation=%s provenance=%s "
        "source_class=%s coverage=%s durability=%s evidence=%zu current=%zu outranked=%zu "
        "conflicting=%zu winning_evidence=%s\n",
        entity.Value().ToString().c_str(), capability.Value().ToString().c_str(),
        std::string(CapabilityStateName(query.state)).c_str(),
        query.has_value ? query.value.ToText().c_str() : "absent",
        query.actionable ? "true" : "false", query.fails_closed ? "true" : "false",
        query.set_generation.ToString().c_str(), query.capability_generation.ToString().c_str(),
        query.registry_generation.ToString().c_str(),
        std::string(ProvenanceClassName(query.provenance)).c_str(),
        std::string(EvidenceSourceClassName(query.source_class)).c_str(),
        std::string(CoverageName(query.coverage)).c_str(),
        std::string(DurabilityClassName(query.durability)).c_str(), query.evidence_count,
        query.current_evidence_count, query.outranked_evidence_count,
        query.conflicting_evidence_count, query.winning_evidence.Value().c_str());
    std::printf("%s\n", query.explanation.ToText().c_str());
    return kExitOk;
  }
  if (command == "evidence") {
    auto entity = EntityId::Parse(arguments.Value("--entity"));
    auto capability = CapabilityId::Parse(arguments.Value("--capability"));
    if (!entity.HasValue() || !capability.HasValue()) {
      return Fail(kExitUsage, "--entity KIND:NAME and --capability ID are required");
    }
    const std::vector<EvidenceSummary> evidence =
        registry.EvidenceFor(entity.Value(), capability.Value());
    if (evidence.empty()) return Fail(kExitLookup, "no evidence is held for this capability");
    for (const EvidenceSummary& summary : evidence) {
      std::printf(
          "evidence=%s source=%s publisher=%s boot=%s epoch=%s provenance=%s source_class=%s "
          "durability=%s coverage=%s state=%s value=%s evidence_generation=%s "
          "source_generation=%s currentness=%s winning=%s conflicting=%s outranked=%s reason=%s\n",
          summary.id.Value().c_str(), summary.source.Value().c_str(),
          summary.publisher.Value().c_str(), summary.worker_boot.Value().c_str(),
          summary.epoch.ToString().c_str(),
          std::string(ProvenanceClassName(summary.provenance)).c_str(),
          std::string(EvidenceSourceClassName(summary.source_class)).c_str(),
          std::string(DurabilityClassName(summary.durability)).c_str(),
          std::string(CoverageName(summary.coverage)).c_str(),
          std::string(CapabilityStateName(summary.state)).c_str(),
          summary.has_value ? summary.value.ToText().c_str() : "absent",
          summary.evidence_generation.ToString().c_str(),
          summary.source_generation.ToString().c_str(),
          std::string(EvidenceCurrentnessName(summary.currentness)).c_str(),
          summary.winning ? "true" : "false", summary.conflicting ? "true" : "false",
          summary.outranked ? "true" : "false",
          summary.reason.IsSet() ? summary.reason.Value().c_str() : "-");
    }
    return kExitOk;
  }
  if (command == "explain") {
    auto entity = EntityId::Parse(arguments.Value("--entity"));
    auto capability = CapabilityId::Parse(arguments.Value("--capability"));
    if (!entity.HasValue() || !capability.HasValue()) {
      return Fail(kExitUsage, "--entity KIND:NAME and --capability ID are required");
    }
    std::printf("%s\n", registry.ExplainCapability(entity.Value(), capability.Value()).ToText().c_str());
    return kExitOk;
  }
  if (command == "generation") {
    const std::string entity_text = arguments.Value("--entity");
    if (entity_text.empty()) {
      std::printf("registry_generation=%s epoch=%s\n", registry.Generation().ToString().c_str(),
                  registry.CurrentEpoch().ToString().c_str());
      for (const EntityId& entity : registry.Entities()) {
        auto set = registry.QueryEntity(entity);
        if (!set.HasValue()) continue;
        std::printf("entity=%s generation=%s set_generation=%s\n", entity.ToString().c_str(),
                    set.Value().entity_generation.ToString().c_str(),
                    set.Value().set_generation.ToString().c_str());
      }
      return kExitOk;
    }
    auto entity = EntityId::Parse(entity_text);
    if (!entity.HasValue()) return Fail(kExitUsage, "--entity KIND:NAME is malformed");
    auto set = registry.QueryEntity(entity.Value());
    if (!set.HasValue()) return Fail(kExitLookup, set.GetError().ToString());
    std::printf("entity=%s generation=%s set_generation=%s\n", entity_text.c_str(),
                set.Value().entity_generation.ToString().c_str(),
                set.Value().set_generation.ToString().c_str());
    return kExitOk;
  }
  if (command == "snapshot") {
    SnapshotScope scope;
    const std::string entity_text = arguments.Value("--entity");
    if (!entity_text.empty()) {
      auto entity = EntityId::Parse(entity_text);
      if (!entity.HasValue()) return Fail(kExitUsage, "--entity KIND:NAME is malformed");
      scope.all_entities = false;
      scope.entities.push_back(entity.Value());
    }
    const std::string namespace_text = arguments.Value("--namespace");
    if (!namespace_text.empty()) {
      auto ns = CapabilityNamespaceId::Parse(namespace_text);
      if (!ns.HasValue()) return Fail(kExitUsage, "--namespace is malformed");
      scope.namespaces.push_back(ns.Value());
    }
    auto snapshot = registry.CreateSnapshot(scope);
    if (!snapshot.HasValue()) return Fail(kExitLookup, snapshot.GetError().ToString());
    const SnapshotCurrentness currentness = registry.CheckSnapshot(snapshot.Value());
    std::printf("%s current=%s\n", snapshot.Value().ToText().c_str(),
                currentness.current ? "true" : "false");
    for (const std::string& reason : currentness.reasons) {
      std::printf("  stale_reason=%s\n", reason.c_str());
    }
    return kExitOk;
  }
  if (command == "diff") {
    const std::string other_directory = arguments.Value("--against-store");
    if (other_directory.empty()) return Fail(kExitUsage, "--against-store DIR is required");
    CapabilityRegistry other;
    PersistenceConfig other_config;
    other_config.directory = other_directory;
    other_config.file_stem = "fabric-capability-registry";
    auto other_loaded = other.Load(other_config);
    if (!other_loaded.HasValue()) return Fail(kExitStore, other_loaded.GetError().ToString());
    SnapshotScope scope;
    const std::string entity_text = arguments.Value("--entity");
    if (!entity_text.empty()) {
      auto entity = EntityId::Parse(entity_text);
      if (!entity.HasValue()) return Fail(kExitUsage, "--entity KIND:NAME is malformed");
      scope.all_entities = false;
      scope.entities.push_back(entity.Value());
    }
    auto snapshot = other.CreateSnapshot(scope);
    if (!snapshot.HasValue()) return Fail(kExitLookup, snapshot.GetError().ToString());
    auto diff = registry.DiffAgainstSnapshot(snapshot.Value());
    if (!diff.HasValue()) return Fail(kExitLookup, diff.GetError().ToString());
    std::printf("%s\n", diff.Value().ToText().c_str());
    return kExitOk;
  }
  if (command == "query") {
    auto entity = EntityId::Parse(arguments.Value("--entity"));
    if (!entity.HasValue()) return Fail(kExitUsage, "--entity KIND:NAME is required");
    const std::string expression = arguments.Value("--require");
    if (expression.empty()) return Fail(kExitUsage, "--require EXPR is required");
    Requirement requirement;
    std::string error;
    if (!BuildRequirement(expression, &requirement, &error)) {
      return Fail(kExitUsage, error);
    }
    const RequirementEvaluation evaluation = registry.Evaluate(entity.Value(), requirement);
    std::printf("outcome=%s entity=%s nodes=%zu\n",
                std::string(RequirementOutcomeName(evaluation.outcome)).c_str(),
                entity.Value().ToString().c_str(), evaluation.nodes_evaluated);
    for (const RequirementNodeResult& node : evaluation.nodes) {
      std::printf("  requirement=%s kind=%s outcome=%s observed_state=%s observed_value=%s detail=%s\n",
                  node.name.c_str(), std::string(RequirementKindName(node.kind)).c_str(),
                  std::string(RequirementOutcomeName(node.outcome)).c_str(),
                  std::string(CapabilityStateName(node.observed_state)).c_str(),
                  node.has_observed_value ? node.observed_value.ToText().c_str() : "absent",
                  node.detail.c_str());
    }
    std::printf("%s\n", evaluation.explanation.ToText().c_str());
    return evaluation.outcome == RequirementOutcome::Satisfied ? kExitOk : kExitLookup;
  }

  Usage();
  return kExitUsage;
}
