// Fabric Capability Registry coordinator process.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Real coordinator: loads durable capability state, advances the coordinator
// epoch, binds a listener and serves capability publications until it is asked
// to stop or its standard input closes.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <string>
#include <vector>

#include "fabric/capability/distributed.hpp"
#include "fabric/capability/ids.hpp"
#include "fabric/capability/version.hpp"

namespace {

using fabric::capability::AuthorityGrant;
using fabric::capability::AuthorityScopeId;
using fabric::capability::CapabilityNamespaceId;
using fabric::capability::CapabilityRegistry;
using fabric::capability::CapabilityCoordinator;
using fabric::capability::CoordinatorOptions;
using fabric::capability::FabricEntityKind;
using fabric::capability::ParseFabricEntityKind;
using fabric::capability::PublicationMode;
using fabric::capability::PublisherId;
using fabric::capability::ReasonToken;

void Usage() {
  std::fprintf(stderr,
               "usage: fcr_coordinator [options]\n"
               "  --port N                     listener port (0 selects an ephemeral port)\n"
               "  --bind ADDR                  IPv4 literal to bind (default 127.0.0.1)\n"
               "  --store DIR                  persistence directory (omitted: no persistence)\n"
               "  --no-epoch-advance           do not advance the coordinator epoch on start\n"
               "  --persist-on-mutation        persist after every committed publication\n"
               "  --authority-scope ID         start describing an authority scope\n"
               "  --authority-kinds LIST       entity classes, e.g. nic,switch,port\n"
               "  --authority-namespaces LIST  capability namespaces, e.g. fabric.port\n"
               "  --authority-modes LIST       full,incremental,partial\n"
               "  --authority-exclusive        allow authoritative full snapshots\n"
               "  --authority-durable          allow durable declarations\n"
               "  --authority-provenance N     strongest provenance class rank (0..6)\n"
               "  --authority-publisher ID     publisher allowlist entry (repeatable)\n"
               "  --authority-description TEXT scope description\n"
               "  --print-port                 print PORT=<n> before READY\n");
}

std::vector<std::string> SplitList(const std::string& text) {
  std::vector<std::string> items;
  std::string current;
  for (const char ch : text) {
    if (ch == ',') {
      if (!current.empty()) items.push_back(current);
      current.clear();
      continue;
    }
    current.push_back(ch);
  }
  if (!current.empty()) items.push_back(current);
  return items;
}

struct ScopeDescription {
  std::string scope;
  std::vector<FabricEntityKind> kinds;
  std::vector<CapabilityNamespaceId> namespaces;
  std::uint8_t modes = 0;
  bool exclusive = false;
  bool durable = false;
  std::uint8_t provenance = 0;
  std::vector<PublisherId> publishers;
  ReasonToken description;
};

}  // namespace

int main(int argc, char** argv) {
  CoordinatorOptions options;
  bool print_port = false;
  std::vector<ScopeDescription> scopes;
  ScopeDescription* current = nullptr;

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
    if (argument == "--port") {
      const char* value = require_value(index, "--port");
      if (value == nullptr) return 2;
      const long parsed = std::strtol(value, nullptr, 10);
      if (parsed < 0 || parsed > 65535) {
        std::fprintf(stderr, "error: --port is out of range\n");
        return 2;
      }
      options.port = static_cast<std::uint16_t>(parsed);
      ++index;
      continue;
    }
    if (argument == "--bind") {
      const char* value = require_value(index, "--bind");
      if (value == nullptr) return 2;
      options.bind_address = value;
      ++index;
      continue;
    }
    if (argument == "--store") {
      const char* value = require_value(index, "--store");
      if (value == nullptr) return 2;
      options.persistence.directory = value;
      ++index;
      continue;
    }
    if (argument == "--no-epoch-advance") {
      options.advance_epoch_on_start = false;
      continue;
    }
    if (argument == "--persist-on-mutation") {
      options.persist_on_mutation = true;
      continue;
    }
    if (argument == "--print-port") {
      print_port = true;
      continue;
    }
    if (argument == "--authority-scope") {
      const char* value = require_value(index, "--authority-scope");
      if (value == nullptr) return 2;
      ScopeDescription scope;
      scope.scope = value;
      scopes.push_back(std::move(scope));
      current = &scopes.back();
      ++index;
      continue;
    }
    if (argument == "--authority-kinds" || argument == "--authority-namespaces" ||
        argument == "--authority-modes" || argument == "--authority-provenance" ||
        argument == "--authority-publisher" || argument == "--authority-description") {
      if (current == nullptr) {
        std::fprintf(stderr, "error: %s must follow --authority-scope\n", argument.c_str());
        return 2;
      }
      const char* value = require_value(index, argument.c_str());
      if (value == nullptr) return 2;
      ++index;
      if (argument == "--authority-kinds") {
        for (const std::string& item : SplitList(value)) {
          auto parsed = ParseFabricEntityKind(item);
          if (!parsed.HasValue()) {
            std::fprintf(stderr, "error: unknown entity class '%s'\n", item.c_str());
            return 2;
          }
          current->kinds.push_back(parsed.Value());
        }
        continue;
      }
      if (argument == "--authority-namespaces") {
        for (const std::string& item : SplitList(value)) {
          auto parsed = CapabilityNamespaceId::Parse(item);
          if (!parsed.HasValue()) {
            std::fprintf(stderr, "error: malformed capability namespace '%s'\n", item.c_str());
            return 2;
          }
          current->namespaces.push_back(parsed.Value());
        }
        continue;
      }
      if (argument == "--authority-modes") {
        for (const std::string& item : SplitList(value)) {
          if (item == "full") {
            current->modes |= static_cast<std::uint8_t>(PublicationMode::FullSnapshot);
          } else if (item == "incremental") {
            current->modes |= static_cast<std::uint8_t>(PublicationMode::Incremental);
          } else if (item == "partial") {
            current->modes |= static_cast<std::uint8_t>(PublicationMode::PartialObservation);
          } else {
            std::fprintf(stderr, "error: unknown publication mode '%s'\n", item.c_str());
            return 2;
          }
        }
        continue;
      }
      if (argument == "--authority-provenance") {
        const long parsed = std::strtol(value, nullptr, 10);
        if (parsed < 0 || parsed > 6) {
          std::fprintf(stderr, "error: --authority-provenance is out of range\n");
          return 2;
        }
        current->provenance = static_cast<std::uint8_t>(parsed);
        continue;
      }
      if (argument == "--authority-publisher") {
        auto parsed = PublisherId::Parse(value);
        if (!parsed.HasValue()) {
          std::fprintf(stderr, "error: malformed publisher identifier '%s'\n", value);
          return 2;
        }
        current->publishers.push_back(parsed.Value());
        continue;
      }
      auto parsed = ReasonToken::Parse(value);
      if (parsed.HasValue()) current->description = parsed.Value();
      continue;
    }
    if (argument == "--authority-exclusive") {
      if (current == nullptr) {
        std::fprintf(stderr, "error: --authority-exclusive must follow --authority-scope\n");
        return 2;
      }
      current->exclusive = true;
      continue;
    }
    if (argument == "--authority-durable") {
      if (current == nullptr) {
        std::fprintf(stderr, "error: --authority-durable must follow --authority-scope\n");
        return 2;
      }
      current->durable = true;
      continue;
    }
    std::fprintf(stderr, "error: unknown argument '%s'\n", argument.c_str());
    Usage();
    return 2;
  }
  CapabilityCoordinator coordinator(options);
  auto started = coordinator.Start();
  if (!started.HasValue()) {
    std::printf("ERROR code=%s message=%s\n", std::string(fabric::capability::ErrorCodeName(
                                                  started.Code())).c_str(),
                started.GetError().message.c_str());
    std::fflush(stdout);
    return 4;
  }

  for (const ScopeDescription& scope : scopes) {
    AuthorityGrant grant;
    auto scope_id = AuthorityScopeId::Parse(scope.scope);
    if (!scope_id.HasValue()) {
      std::printf("ERROR code=malformed-identifier message=authority scope\n");
      std::fflush(stdout);
      coordinator.Stop();
      return 2;
    }
    grant.scope = scope_id.Value();
    grant.entity_kinds = scope.kinds;
    grant.namespaces = scope.namespaces;
    grant.modes = scope.modes;
    grant.exclusive = scope.exclusive;
    grant.may_publish_durable = scope.durable;
    grant.strongest_provenance = static_cast<fabric::capability::ProvenanceClass>(scope.provenance);
    grant.allowed_publishers = scope.publishers;
    grant.description = scope.description;
    auto declared = coordinator.Registry().DeclareAuthority(grant);
    if (!declared.HasValue()) {
      std::printf("ERROR code=%s message=%s\n",
                  std::string(fabric::capability::ErrorCodeName(declared.Code())).c_str(),
                  declared.GetError().message.c_str());
      std::fflush(stdout);
      coordinator.Stop();
      return 4;
    }
  }

  std::printf("PORT=%u\n", static_cast<unsigned>(coordinator.BoundPort()));
  std::printf("EPOCH=%s\n", coordinator.Epoch().ToString().c_str());
  if (print_port) std::fflush(stdout);
  std::printf("READY\n");
  std::fflush(stdout);

  // A serving coordinator does not stop merely because an unrelated stdin reached end of
  // input: it serves until it is explicitly asked to stop or the process is terminated.
  bool stop_requested = false;
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line == "stop") {
      stop_requested = true;
      break;
    }
  }
  while (!stop_requested) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  coordinator.Stop();
  std::printf("STOPPED\n");
  std::fflush(stdout);
  return 0;
}
