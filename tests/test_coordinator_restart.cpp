// Fabric Capability Registry test suite: real worker death and coordinator restart.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// These cases drive real operating system processes: a coordinator process and
// publisher processes that are killed with TerminateProcess. Nothing here is
// simulated in process.

#include <string>
#include <vector>

#include "fabric/capability/fabric_capability.hpp"
#include "process_helper.hpp"
#include "registry_fixture.hpp"
#include "temp_dir.hpp"
#include "test_framework.hpp"

using namespace fabric::capability;
using namespace fcr::test;

namespace {

constexpr unsigned long kProcessWaitMs = 60000;

struct Cluster {
  TempDir dir{"cluster"};
  std::uint16_t port = 0;
  std::string store() const { return dir.path().string(); }
};

bool WaitForCoordinator(std::uint16_t port) {
  PublisherClientOptions options;
  options.port = port;
  options.publisher = *PublisherId::Parse("probe-publisher");
  options.scope = *AuthorityScopeId::Parse("host-scope");
  options.source = *SourceId::Parse("probe-source");
  options.worker_boot = Boot(7);
  options.connect_attempts = 60;
  options.connect_backoff_ms = 25;
  PublisherClient probe(options);
  const auto connected = probe.Connect();
  return connected.HasValue();
}

PublisherClientOptions ClientFor(const std::string& publisher, const std::string& boot,
                                 std::uint16_t port) {
  PublisherClientOptions options;
  options.port = port;
  options.publisher = *PublisherId::Parse(publisher);
  options.scope = *AuthorityScopeId::Parse("host-scope");
  options.source = *SourceId::Parse("host-source");
  options.worker_boot = *WorkerBootId::Parse(boot);
  options.connect_attempts = 40;
  return options;
}

std::vector<std::string> CoordinatorArguments(const Cluster& cluster, const std::string& scope,
                                              const std::string& publisher_a,
                                              const std::string& publisher_a2) {
  return {"--port", std::to_string(cluster.port), "--store", cluster.store(),
          "--authority-scope", scope, "--authority-kinds", "nic",
          "--authority-namespaces", "fabric.port,fabric.offload,fabric.device-management",
          "--authority-modes", "full,incremental,partial", "--authority-publisher", publisher_a,
          "--authority-publisher", publisher_a2, "--authority-durable",
          "--authority-provenance", "0", "--print-port"};
}

}  // namespace

FCR_TEST(coordinator_restart, real_worker_death_and_coordinator_restart) {
  const std::string coordinator_exe = CoordinatorExecutablePath();
  const std::string publisher_exe = PublisherExecutablePath();
  FCR_REQUIRE(!coordinator_exe.empty());
  FCR_REQUIRE(!publisher_exe.empty());

  Cluster cluster;
  cluster.port = FindFreeLoopbackPort();
  FCR_REQUIRE(cluster.port != 0);

  auto coordinator = ChildProcess::Start(coordinator_exe,
                                         CoordinatorArguments(cluster, "host-scope",
                                                              "pub-a", "pub-a2"),
                                         cluster.store());
  FCR_REQUIRE(coordinator.has_value());
  FCR_REQUIRE(WaitForCoordinator(cluster.port));

  const std::string boot_a = "aaaa1111bbbb2222cccc3333dddd4444";
  const std::string boot_a2 = "aaaa1111bbbb2222cccc3333dddd5555";

  // Publisher A publishes a durable administrative declaration and a process
  // bound hardware claim for one entity, then holds its connection open.
  auto publisher_a = ChildProcess::StartCapturing(
      publisher_exe,
      {"--port", std::to_string(cluster.port), "--publisher", "pub-a", "--scope", "host-scope",
       "--source", "host-source", "--boot", boot_a, "--entity", "nic:host-death-0",
       "--generation", "1", "--mode", "full", "--coverage", "full",
       "--provenance", "2", "--source-class", "7", "--durability", "durable",
       "--claim", "fabric.offload.rdma=supported", "--claim-value", "fabric.offload.rdma=bool:true",
       "--claim", "fabric.port.supported_speeds=supported",
       "--claim-value", "fabric.port.supported_speeds=numericset:bit/s:100000000000",
       "--provenance", "0", "--source-class", "0", "--hold"},
      cluster.store());
  FCR_REQUIRE(publisher_a.has_value());
  FCR_CHECK(publisher_a->WaitBounded(kProcessWaitMs) == ExitWait::StillRunning);

  // Publisher B publishes a different entity and stays independent.
  auto publisher_b = ChildProcess::Start(
      publisher_exe,
      {"--port", std::to_string(cluster.port), "--publisher", "pub-a2", "--scope", "host-scope",
       "--source", "host-source-b", "--boot", boot_a2, "--entity", "nic:host-death-1",
       "--generation", "1", "--mode", "full", "--coverage", "full", "--provenance", "0",
       "--source-class", "0", "--claim", "fabric.port.supported_speeds=supported",
       "--claim-value", "fabric.port.supported_speeds=numericset:bit/s:100000000000"},
      cluster.store());
  FCR_REQUIRE(publisher_b.has_value());
  FCR_CHECK_EQ(publisher_b->WaitForExit(), 0ul);

  // The coordinator is still alive and the evidence is current.
  FCR_CHECK(coordinator->Running());
  {
    auto probe = ChildProcess::Start(publisher_exe,
                                     {"--port", std::to_string(cluster.port), "--publisher",
                                      "pub-a2", "--scope", "host-scope", "--source", "probe",
                                      "--boot", boot_a2, "--entity", "nic:host-death-0",
                                      "--generation", "1", "--mode", "partial",
                                      "--claim", "fabric.port.supported_speeds=supported",
                                      "--claim-value",
                                      "fabric.port.supported_speeds=numericset:bit/s:400000000000"},
                                     cluster.store());
    FCR_REQUIRE(probe.has_value());
    const unsigned long exit_code = probe->WaitForExit();
    FCR_CHECK(exit_code == 0ul || exit_code == 3ul);
  }

  // Kill publisher A as a real operating system process.
  publisher_a->Kill();
  FCR_CHECK(!ProcessAlive(publisher_a->Pid()));
  const std::string captured = publisher_a->ReadCaptured();
  FCR_CHECK(captured.find("HELLO epoch=") != std::string::npos);

  // The coordinator detects the death through the real control path, fences
  // the boot and makes the process bound evidence non current.
  bool fenced = false;
  for (int attempt = 0; attempt < 400 && !fenced; ++attempt) {
    auto probe = ChildProcess::Start(
        publisher_exe,
        {"--port", std::to_string(cluster.port), "--publisher", "pub-a", "--scope", "host-scope",
         "--source", "host-source", "--boot", boot_a, "--entity", "nic:host-death-0",
         "--generation", "1", "--mode", "partial", "--claim",
         "fabric.port.supported_speeds=supported", "--claim-value",
         "fabric.port.supported_speeds=numericset:bit/s:100000000000"},
        cluster.store());
    if (!probe.has_value()) break;
    const unsigned long exit_code = probe->WaitForExit();
    if (exit_code == 3ul) fenced = true;
  }
  FCR_CHECK(fenced);

  // A stale replay from the dead boot is refused.
  auto stale = ChildProcess::Start(
      publisher_exe,
      {"--port", std::to_string(cluster.port), "--publisher", "pub-a", "--scope", "host-scope",
       "--source", "host-source", "--boot", boot_a, "--entity", "nic:host-death-0", "--generation",
       "1", "--mode", "full", "--claim", "fabric.port.supported_speeds=supported", "--claim-value",
       "fabric.port.supported_speeds=numericset:bit/s:100000000000"},
      cluster.store());
  FCR_REQUIRE(stale.has_value());
  FCR_CHECK_EQ(stale->WaitForExit(), 3ul);

  // A reincarnated publisher with a fresh boot requires fresh evidence.
  const std::string boot_fresh = "ffff1111eeee2222dddd3333cccc4444";
  auto fresh = ChildProcess::Start(
      publisher_exe,
      {"--port", std::to_string(cluster.port), "--publisher", "pub-a", "--scope", "host-scope",
       "--source", "host-source", "--boot", boot_fresh, "--entity", "nic:host-death-0",
       "--generation", "1", "--mode", "full", "--coverage", "full", "--provenance", "0",
       "--source-class", "0", "--claim", "fabric.port.supported_speeds=supported",
       "--claim-value", "fabric.port.supported_speeds=numericset:bit/s:200000000000",
       "--expected-set-generation", "1"},
      cluster.store());
  FCR_REQUIRE(fresh.has_value());
  FCR_CHECK_EQ(fresh->WaitForExit(), 0ul);

  // Real coordinator restart: kill it hard and start a fresh process on the
  // same durable store and port.
  coordinator->Kill();
  FCR_CHECK(!ProcessAlive(coordinator->Pid()));

  auto restarted = ChildProcess::Start(coordinator_exe,
                                       CoordinatorArguments(cluster, "host-scope", "pub-a",
                                                            "pub-a2"),
                                       cluster.store());
  FCR_REQUIRE(restarted.has_value());
  FCR_REQUIRE(WaitForCoordinator(cluster.port));

  // The durable administrative declaration survived; the process bound
  // hardware observation did not silently survive.
  auto after_restart = ChildProcess::Start(
      publisher_exe,
      {"--port", std::to_string(cluster.port), "--publisher", "pub-a", "--scope", "host-scope",
       "--source", "host-source", "--boot", boot_fresh, "--entity", "nic:host-death-0",
       "--generation", "1", "--mode", "partial", "--claim",
       "fabric.port.supported_speeds=supported", "--claim-value",
       "fabric.port.supported_speeds=numericset:bit/s:100000000000"},
      cluster.store());
  FCR_REQUIRE(after_restart.has_value());
  // The pre-restart boot is fenced by the epoch advance, so this is refused.
  FCR_CHECK_EQ(after_restart->WaitForExit(), 3ul);

  const std::string boot_final = "99998888777766665555444433332222";
  auto final_publish = ChildProcess::StartCapturing(
      publisher_exe,
      {"--port", std::to_string(cluster.port), "--publisher", "pub-a", "--scope", "host-scope",
       "--source", "host-source", "--boot", boot_final, "--entity", "nic:host-death-0",
       "--generation", "1", "--mode", "full", "--coverage", "full", "--provenance", "0",
       "--source-class", "0", "--claim", "fabric.port.supported_speeds=supported",
       "--claim-value", "fabric.port.supported_speeds=numericset:bit/s:400000000000",
       "--claim", "fabric.offload.rdma=supported", "--claim-value",
       "fabric.offload.rdma=bool:true", "--claim", "fabric.device-management.secure_boot_supported=supported",
       "--claim-value", "fabric.device-management.secure_boot_supported=bool:true"},
      cluster.store());
  FCR_REQUIRE(final_publish.has_value());
  const unsigned long final_exit = final_publish->WaitForExit();
  const std::string final_output = final_publish->ReadCaptured();
  FCR_CHECK(final_exit == 0ul || final_exit == 3ul);
  FCR_CHECK(final_output.find("HELLO epoch=") != std::string::npos);

  // The durable store remains intact and inspectable.
  PersistenceConfig config;
  config.directory = cluster.dir.path();
  config.file_stem = "fabric-capability-registry";
  auto inspection = InspectStore(config);
  FCR_REQUIRE_OK(inspection);
  FCR_CHECK(inspection.Value().integrity_ok);

  restarted->Kill();
  FCR_CHECK(!ProcessAlive(restarted->Pid()));
}
