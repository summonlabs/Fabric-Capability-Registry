// Fabric Capability Registry publisher process.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Real publisher worker: connects to a coordinator, performs the handshake and
// publishes one capability request built entirely from command line arguments.

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "fabric/capability/distributed.hpp"
#include "fabric/capability/ids.hpp"
#include "fabric/capability/synthetic.hpp"
#include "fabric/capability/value.hpp"
#include "fabric/capability/version.hpp"

namespace {

using namespace fabric::capability;

std::vector<std::string> SplitList(const std::string& text, char separator) {
  std::vector<std::string> items;
  std::string current;
  for (const char ch : text) {
    if (ch == separator) {
      if (!current.empty()) items.push_back(current);
      current.clear();
      continue;
    }
    current.push_back(ch);
  }
  if (!current.empty()) items.push_back(current);
  return items;
}

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

bool ParseU32(const std::string& text, std::uint32_t* out) {
  std::uint64_t value = 0;
  if (!ParseU64(text, &value) || value > 0xFFFFFFFFull) return false;
  *out = static_cast<std::uint32_t>(value);
  return true;
}

Outcome<CapabilityValue> ParseValue(const std::string& text) {
  const std::size_t colon = text.find(':');
  if (colon == std::string::npos) {
    return Outcome<CapabilityValue>::Failure(ErrorCode::MalformedValue,
                                             "claim value must be KIND:VALUE", text);
  }
  const std::string kind = text.substr(0, colon);
  const std::string body = text.substr(colon + 1);
  if (kind == "bool") {
    if (body == "true") return CapabilityValue::Boolean(true);
    if (body == "false") return CapabilityValue::Boolean(false);
    return Outcome<CapabilityValue>::Failure(ErrorCode::MalformedValue, "boolean must be true/false",
                                             body);
  }
  if (kind == "u64") {
    std::uint64_t value = 0;
    if (!ParseU64(body, &value)) {
      return Outcome<CapabilityValue>::Failure(ErrorCode::MalformedValue, "malformed u64", body);
    }
    if (value > static_cast<std::uint64_t>(limits::kMaxInteger)) {
      return Outcome<CapabilityValue>::Failure(ErrorCode::ValueOutOfRange, "u64 above the bound");
    }
    return CapabilityValue::Integer(static_cast<std::int64_t>(value));
  }
  if (kind == "quantity" || kind == "numericset" || kind == "range") {
    const std::vector<std::string> parts = SplitList(body, ':');
    if (kind == "quantity") {
      if (parts.size() != 2) {
        return Outcome<CapabilityValue>::Failure(ErrorCode::MalformedValue,
                                                 "quantity expects VALUE:UNIT", body);
      }
      std::uint64_t value = 0;
      if (!ParseU64(parts[0], &value)) {
        return Outcome<CapabilityValue>::Failure(ErrorCode::MalformedValue, "malformed quantity",
                                                 parts[0]);
      }
      auto unit = ParseUnit(parts[1]);
      if (!unit) return unit.GetError();
      return CapabilityValue::QuantityValue(value, unit.Value());
    }
    if (kind == "numericset") {
      if (parts.size() != 2) {
        return Outcome<CapabilityValue>::Failure(ErrorCode::MalformedValue,
                                                 "numericset expects UNIT:V1,V2", body);
      }
      auto unit = ParseUnit(parts[0]);
      if (!unit) return unit.GetError();
      std::vector<std::uint64_t> values;
      for (const std::string& item : SplitList(parts[1], ',')) {
        std::uint64_t value = 0;
        if (!ParseU64(item, &value)) {
          return Outcome<CapabilityValue>::Failure(ErrorCode::MalformedValue,
                                                   "malformed set member", item);
        }
        values.push_back(value);
      }
      return CapabilityValue::NumericSetValue(unit.Value(), values);
    }
    if (parts.size() != 3) {
      return Outcome<CapabilityValue>::Failure(ErrorCode::MalformedValue,
                                               "range expects UNIT:MIN:MAX", body);
    }
    auto unit = ParseUnit(parts[0]);
    if (!unit) return unit.GetError();
    std::uint64_t minimum = 0;
    std::uint64_t maximum = 0;
    if (!ParseU64(parts[1], &minimum) || !ParseU64(parts[2], &maximum)) {
      return Outcome<CapabilityValue>::Failure(ErrorCode::MalformedValue, "malformed range bounds",
                                               body);
    }
    return CapabilityValue::NumericRangeValue(unit.Value(), minimum, maximum);
  }
  if (kind == "versionset") {
    const std::vector<std::string> parts = SplitList(body, ':');
    if (parts.size() != 2) {
      return Outcome<CapabilityValue>::Failure(ErrorCode::MalformedValue,
                                               "versionset expects MIN:MAX", body);
    }
    auto minimum = Version::Parse(parts[0]);
    if (!minimum) return minimum.GetError();
    auto maximum = Version::Parse(parts[1]);
    if (!maximum) return maximum.GetError();
    return CapabilityValue::VersionIntervalValue(minimum.Value(), true, maximum.Value(), true);
  }
  if (kind == "enumset") {
    const std::vector<std::string> parts = SplitList(body, ':');
    if (parts.size() != 2) {
      return Outcome<CapabilityValue>::Failure(ErrorCode::MalformedValue,
                                               "enumset expects DOMAIN:C1,C2", body);
    }
    auto domain = EnumDomainId::Parse(parts[0]);
    if (!domain) return domain.GetError();
    std::vector<std::uint32_t> codes;
    for (const std::string& item : SplitList(parts[1], ',')) {
      std::uint32_t code = 0;
      if (!ParseU32(item, &code)) {
        return Outcome<CapabilityValue>::Failure(ErrorCode::MalformedValue,
                                                 "malformed enumeration code", item);
      }
      codes.push_back(code);
    }
    return CapabilityValue::EnumerationSetValue(domain.Value(), codes);
  }
  if (kind == "protocolset") {
    std::vector<ProtocolId> protocols;
    for (const std::string& item : SplitList(body, ',')) {
      auto protocol = ParseProtocol(item);
      if (!protocol) return protocol.GetError();
      protocols.push_back(protocol.Value());
    }
    return CapabilityValue::ProtocolSetValue(protocols);
  }
  return Outcome<CapabilityValue>::Failure(ErrorCode::MalformedValue,
                                           "unknown claim value kind", kind);
}

void Usage() {
  std::fprintf(stderr,
               "usage: fcr_publisher --port N --publisher ID --scope ID --source ID --boot HEX\n"
               "                      --entity KIND:NAME --generation N\n"
               "                      [--mode full|incremental|partial]\n"
               "                      [--claim capability=STATE] [--claim-value capability=KIND:VALUE]\n"
               "                      [--profile DEVICE-CLASS] [--repeat N] [--hold]\n"
               "                      [--epoch N] [--expected-set-generation N]\n"
               "                      [--provenance N] [--source-class N]\n"
               "                      [--durability durable|process] [--coverage partial|full]\n"
               "                      [--reason TEXT] [--fence-boot HEX]\n");
}

}  // namespace

int main(int argc, char** argv) {
  PublisherClientOptions options;
  std::string entity_text;
  std::string profile;
  std::uint64_t generation = 1;
  bool generation_set = false;
  PublicationMode mode = PublicationMode::FullSnapshot;
  Coverage coverage = Coverage::Partial;
  std::uint8_t provenance = 6;
  std::uint8_t source_class = 9;
  DurabilityClass durability = DurabilityClass::Durable;
  std::uint64_t expected_set_generation = 0;
  std::uint64_t repeat = 1;
  bool hold = false;
  std::string fence_boot_text;
  ReasonToken reason;
  std::map<std::string, CapabilityState> claim_states;
  std::map<std::string, std::string> claim_values;

  const auto require_value = [&argc, &argv](int index, const char* name) -> const char* {
    if (index + 1 >= argc) {
      std::fprintf(stderr, "error: %s requires a value\n", name);
      return nullptr;
    }
    return argv[index + 1];
  };

  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--help" || argument == "-h") {
      Usage();
      return 0;
    }
    const char* value = nullptr;
    if (argument.rfind("--", 0) == 0) {
      value = require_value(index, argument.c_str());
      if (value == nullptr) return 2;
      ++index;
    }
    if (argument == "--host") {
      options.host = value;
      continue;
    }
    if (argument == "--port") {
      const long parsed = std::strtol(value, nullptr, 10);
      if (parsed <= 0 || parsed > 65535) {
        std::fprintf(stderr, "error: --port is out of range\n");
        return 2;
      }
      options.port = static_cast<std::uint16_t>(parsed);
      continue;
    }
    if (argument == "--publisher" || argument == "--scope" || argument == "--source" ||
        argument == "--boot") {
      if (argument == "--publisher") {
        auto parsed = PublisherId::Parse(value);
        if (!parsed) {
          std::fprintf(stderr, "error: malformed publisher identifier\n");
          return 2;
        }
        options.publisher = parsed.Value();
      } else if (argument == "--scope") {
        auto parsed = AuthorityScopeId::Parse(value);
        if (!parsed) {
          std::fprintf(stderr, "error: malformed authority scope identifier\n");
          return 2;
        }
        options.scope = parsed.Value();
      } else if (argument == "--source") {
        auto parsed = SourceId::Parse(value);
        if (!parsed) {
          std::fprintf(stderr, "error: malformed source identifier\n");
          return 2;
        }
        options.source = parsed.Value();
      } else {
        auto parsed = WorkerBootId::Parse(value);
        if (!parsed) {
          std::fprintf(stderr, "error: malformed worker boot identifier\n");
          return 2;
        }
        options.worker_boot = parsed.Value();
      }
      continue;
    }
    if (argument == "--entity") {
      entity_text = value;
      continue;
    }
    if (argument == "--generation") {
      if (!ParseU64(value, &generation) || generation == 0) {
        std::fprintf(stderr, "error: malformed entity generation\n");
        return 2;
      }
      generation_set = true;
      continue;
    }
    if (argument == "--mode") {
      const std::string text = value;
      if (text == "full") {
        mode = PublicationMode::FullSnapshot;
      } else if (text == "incremental") {
        mode = PublicationMode::Incremental;
      } else if (text == "partial") {
        mode = PublicationMode::PartialObservation;
      } else {
        std::fprintf(stderr, "error: unknown publication mode\n");
        return 2;
      }
      continue;
    }
    if (argument == "--claim") {
      const std::string text = value;
      const std::size_t equals = text.find('=');
      if (equals == std::string::npos) {
        std::fprintf(stderr, "error: --claim expects capability=STATE\n");
        return 2;
      }
      auto state = ParseCapabilityState(text.substr(equals + 1));
      if (!state) {
        std::fprintf(stderr, "error: unknown capability state '%s'\n",
                     text.substr(equals + 1).c_str());
        return 2;
      }
      claim_states[text.substr(0, equals)] = state.Value();
      continue;
    }
    if (argument == "--claim-value") {
      const std::string text = value;
      const std::size_t equals = text.find('=');
      if (equals == std::string::npos) {
        std::fprintf(stderr, "error: --claim-value expects capability=KIND:VALUE\n");
        return 2;
      }
      claim_values[text.substr(0, equals)] = text.substr(equals + 1);
      continue;
    }
    if (argument == "--profile") {
      profile = value;
      continue;
    }
    if (argument == "--epoch") {
      std::uint64_t epoch = 0;
      if (!ParseU64(value, &epoch)) {
        std::fprintf(stderr, "error: malformed epoch\n");
        return 2;
      }
      options.epoch = CoordinatorEpoch::FromValue(epoch);
      continue;
    }
    if (argument == "--expected-set-generation") {
      if (!ParseU64(value, &expected_set_generation)) {
        std::fprintf(stderr, "error: malformed expected set generation\n");
        return 2;
      }
      continue;
    }
    if (argument == "--repeat") {
      if (!ParseU64(value, &repeat) || repeat == 0 || repeat > 1000) {
        std::fprintf(stderr, "error: --repeat is out of range\n");
        return 2;
      }
      continue;
    }
    if (argument == "--provenance") {
      std::uint64_t parsed = 0;
      if (!ParseU64(value, &parsed) || parsed > 6) {
        std::fprintf(stderr, "error: --provenance is out of range\n");
        return 2;
      }
      provenance = static_cast<std::uint8_t>(parsed);
      continue;
    }
    if (argument == "--source-class") {
      std::uint64_t parsed = 0;
      if (!ParseU64(value, &parsed) || parsed > 9) {
        std::fprintf(stderr, "error: --source-class is out of range\n");
        return 2;
      }
      source_class = static_cast<std::uint8_t>(parsed);
      continue;
    }
    if (argument == "--durability") {
      const std::string text = value;
      if (text == "durable") {
        durability = DurabilityClass::Durable;
      } else if (text == "process") {
        durability = DurabilityClass::ProcessBound;
      } else {
        std::fprintf(stderr, "error: unknown durability class\n");
        return 2;
      }
      continue;
    }
    if (argument == "--coverage") {
      const std::string text = value;
      if (text == "partial") {
        coverage = Coverage::Partial;
      } else if (text == "full") {
        coverage = Coverage::FullEnumeration;
      } else {
        std::fprintf(stderr, "error: unknown coverage class\n");
        return 2;
      }
      continue;
    }
    if (argument == "--reason") {
      auto parsed = ReasonToken::Parse(value);
      if (!parsed) {
        std::fprintf(stderr, "error: malformed reason text\n");
        return 2;
      }
      reason = parsed.Value();
      continue;
    }
    if (argument == "--fence-boot") {
      fence_boot_text = value;
      continue;
    }
    if (argument == "--hold") {
      hold = true;
      continue;
    }
    std::fprintf(stderr, "error: unknown argument '%s'\n", argument.c_str());
    Usage();
    return 2;
  }

  std::vector<CapabilityClaim> claims;
  EntityId entity;
  if (!profile.empty()) {
    auto device_class = ParseSyntheticDeviceClass(profile);
    if (!device_class) {
      std::fprintf(stderr, "error: unknown synthetic profile '%s'\n", profile.c_str());
      return 2;
    }
    SyntheticOptions synthetic_options;
    synthetic_options.device_count = 1;
    auto publications = BuildSyntheticFabric(device_class.Value(), synthetic_options);
    if (publications.empty()) {
      std::fprintf(stderr, "error: the synthetic profile produced no publication\n");
      return 3;
    }
    claims = publications.front().claims;
    entity = publications.front().entity;
    if (!generation_set) generation = publications.front().entity_generation.Value();
  }
  if (!entity_text.empty()) {
    auto parsed = EntityId::Parse(entity_text);
    if (!parsed) {
      std::fprintf(stderr, "error: malformed entity identifier '%s'\n", entity_text.c_str());
      return 2;
    }
    entity = parsed.Value();
  }
  if (!entity.IsSet()) {
    std::fprintf(stderr, "error: --entity or --profile is required\n");
    return 2;
  }
  if (!options.publisher.IsSet() || !options.scope.IsSet() || !options.source.IsSet() ||
      options.port == 0) {
    std::fprintf(stderr, "error: --port, --publisher, --scope and --source are required\n");
    return 2;
  }

  for (const auto& entry : claim_states) {
    CapabilityClaim claim;
    auto parsed = CapabilityId::Parse(entry.first);
    if (!parsed) {
      std::fprintf(stderr, "error: malformed capability identifier '%s'\n", entry.first.c_str());
      return 2;
    }
    claim.capability = parsed.Value();
    claim.state = entry.second;
    claim.provenance = static_cast<ProvenanceClass>(provenance);
    claim.source_class = static_cast<EvidenceSourceClass>(source_class);
    claim.durability = durability;
    claim.coverage = coverage;
    claim.reason = reason;
    const auto value_entry = claim_values.find(entry.first);
    if (value_entry != claim_values.end()) {
      auto value = ParseValue(value_entry->second);
      if (!value) {
        std::fprintf(stderr, "error: %s\n", value.GetError().ToString().c_str());
        return 2;
      }
      claim.value = value.Value();
    }
    claims.erase(std::remove_if(claims.begin(), claims.end(),
                                [&claim](const CapabilityClaim& existing) {
                                  return existing.capability == claim.capability;
                                }),
                 claims.end());
    claims.push_back(std::move(claim));
  }

  PublisherClient client(options);
  auto connected = client.Connect();
  if (!connected.HasValue()) {
    std::printf("HELLO accepted=false code=%s message=%s\n",
                std::string(ErrorCodeName(connected.Code())).c_str(),
                connected.GetError().message.c_str());
    std::printf("DONE status=rejected\n");
    std::fflush(stdout);
    return connected.Code() == ErrorCode::WorkerBootFenced ||
                   connected.Code() == ErrorCode::UnauthorizedPublisher ||
                   connected.Code() == ErrorCode::CoordinatorEpochStale ||
                   connected.Code() == ErrorCode::UnknownAuthorityScope
               ? 3
               : 2;
  }
  std::printf("HELLO epoch=%s\n", client.Epoch().ToString().c_str());
  std::fflush(stdout);

  if (!fence_boot_text.empty()) {
    auto boot = WorkerBootId::Parse(fence_boot_text);
    if (!boot) {
      std::fprintf(stderr, "error: malformed fence target\n");
      return 2;
    }
    auto fenced = client.Fence(boot.Value(), reason);
    std::printf("FENCE status=%s\n", fenced.HasValue() ? "ok" : "rejected");
    std::printf("DONE status=%s\n", fenced.HasValue() ? "committed" : "rejected");
    std::fflush(stdout);
    client.Close();
    return fenced.HasValue() ? 0 : 3;
  }

  PublicationRequest request;
  request.mode = mode;
  request.publication = PublicationId{};
  auto publication_id = PublicationId::Parse(std::string("publication-") + entity.CanonicalName());
  if (publication_id.HasValue()) request.publication = publication_id.Value();
  request.coverage = coverage;
  request.reason = reason;
  request.authority.publisher = options.publisher;
  request.authority.scope = options.scope;
  request.authority.source = options.source;
  request.authority.worker_boot = options.worker_boot.IsSet() ? options.worker_boot
                                                             : client.Epoch().IsSet()
                                                                   ? options.worker_boot
                                                                   : options.worker_boot;
  request.authority.source_generation = SourceGeneration::FromValue(1);
  request.entity = entity;
  request.entity_generation = EntityGeneration::FromValue(generation);
  request.expected_set_generation = CapabilitySetGeneration::FromValue(expected_set_generation);
  request.claims = claims;

  PublicationStatus last_status = PublicationStatus::Rejected;
  ErrorCode last_code = ErrorCode::Ok;
  std::string last_message;
  for (std::uint64_t attempt = 0; attempt < repeat; ++attempt) {
    std::string attempt_text = "attempt-" + entity.CanonicalName() + "-" + std::to_string(attempt);
    auto attempt_id = MutationAttemptId::Parse(attempt_text);
    if (!attempt_id.HasValue()) {
      std::fprintf(stderr, "error: malformed attempt identifier\n");
      return 2;
    }
    request.attempt = attempt_id.Value();
    auto result = client.Publish(request);
    if (!result.HasValue()) {
      std::printf("PUBLISH status=rejected code=%s message=%s\n",
                  std::string(ErrorCodeName(result.Code())).c_str(),
                  result.GetError().message.c_str());
      std::printf("DONE status=rejected\n");
      std::fflush(stdout);
      client.Close();
      return 2;
    }
    last_status = result.Value().status;
    last_code = result.Value().code;
    last_message = result.Value().message;
    std::printf("PUBLISH status=%s code=%s set_generation=%s claims=%zu diff=%zu\n",
                std::string(PublicationStatusName(result.Value().status)).c_str(),
                std::string(ErrorCodeName(result.Value().code)).c_str(),
                result.Value().new_set_generation.ToString().c_str(),
                result.Value().claims_applied, result.Value().diff.entries.size());
    if (result.Value().status == PublicationStatus::Rejected) {
      std::printf("REJECT reason=%s\n", result.Value().message.c_str());
    }
    std::fflush(stdout);
  }

  if (hold) {
    std::string line;
    while (std::getline(std::cin, line)) {
      if (line == "stop") break;
    }
  }
  std::printf("DONE status=%s\n", std::string(PublicationStatusName(last_status)).c_str());
  std::fflush(stdout);
  client.Close();
  if (last_status == PublicationStatus::Rejected) {
    (void)last_code;
    (void)last_message;
    return 3;
  }
  return 0;
}
