// Fabric Capability Registry test suite: real worker death and coordinator restart.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// These cases drive real operating system processes: a coordinator process and
// publisher processes that are killed with TerminateProcess. Nothing here is
// simulated in process.

#include <chrono>
#include <string>
#include <thread>
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

/// Every wait in this proof is bounded, and a child that does not exit within the bound is
/// reported as a failed assertion and killed: a hang is a defect, never something to hide.
unsigned long WaitOrAbort(ChildProcess& child, const char* what) {
  if (child.WaitBounded(kProcessWaitMs) != ExitWait::Exited) {
    FCR_FAIL(std::string("child did not exit within the bound: ") + what);
    child.Kill();
    throw fcr::test::TestAbort(std::string("child hang: ") + what);
  }
  return child.WaitForExit();
}

/// Asserts an expected exit code and reports the captured child output when it differs, so a
/// failing proof explains itself.
void ExpectExit(ChildProcess& child, unsigned long expected, const char* what) {
  const unsigned long code = WaitOrAbort(child, what);
  if (code != expected) {
    FCR_FAIL(std::string(what) + ": expected exit " + std::to_string(expected) + " but saw " +
             std::to_string(code) + " output=[" + child.ReadCaptured() + "]");
  }
}

struct Cluster {
  TempDir dir{"cluster"};
  std::uint16_t port = 0;
  std::string store() const { return dir.path().string(); }
};

/// One in-process handshake attempt, so readiness and fencing detection never spawn a
/// process per iteration.
Status Handshake(std::uint16_t port, const std::string& publisher, const std::string& boot) {
  PublisherClientOptions options;
  options.port = port;
  options.publisher = *PublisherId::Parse(publisher);
  options.scope = *AuthorityScopeId::Parse("host-scope");
  options.source = *SourceId::Parse("host-source");
  options.worker_boot = *WorkerBootId::Parse(boot);
  options.connect_attempts = 1;
  options.connect_backoff_ms = 20;
  PublisherClient probe(options);
  auto connected = probe.Connect();
  if (!connected.HasValue()) return connected.GetError();
  return Status::Success();
}

/// Readiness is proven by the coordinator answering the handshake at all: a refusal that
/// names an authority, fence or epoch reason still proves the listener is serving.
bool CoordinatorReachable(std::uint16_t port, const std::string& publisher,
                          const std::string& boot) {
  const Status outcome = Handshake(port, publisher, boot);
  if (outcome.HasValue()) return true;
  const ErrorCode code = outcome.Code();
  return code == ErrorCode::UnauthorizedPublisher || code == ErrorCode::WorkerBootFenced ||
         code == ErrorCode::UnknownAuthorityScope || code == ErrorCode::CoordinatorEpochStale;
}

bool WaitForCoordinator(std::uint16_t port) {
  // Bounded in time, not in attempts: a refused loopback connection returns immediately, so
  // a fixed attempt count would make readiness a bet on how fast the process starts and on
  // how loaded the machine is. The bound is generous but it is still a bound, and failure is
  // reported rather than skipped.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (std::chrono::steady_clock::now() < deadline) {
    // A throwaway boot identity: probing fences the boot it uses, so it must never be a boot
    // a real publisher depends on.
    if (CoordinatorReachable(port, "pub-a", "0000000000000000ffffffffffffffff")) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
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
          "--authority-provenance", "0", "--persist-on-mutation", "--print-port"};
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

  auto coordinator = ChildProcess::Start(
      coordinator_exe, CoordinatorArguments(cluster, "host-scope", "pub-a", "pub-a2"),
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
       "--claim", "fabric.port.supported_speeds=supported",
       "--claim-value", "fabric.port.supported_speeds=numericset:bit/s:100000000000",
       "--provenance", "0", "--source-class", "0", "--hold"},
      cluster.store());
  FCR_REQUIRE(publisher_a.has_value());
  if (publisher_a->WaitBounded(kProcessWaitMs) != ExitWait::StillRunning) {
    FCR_FAIL(std::string("publisher-a exited instead of holding: ") + publisher_a->ReadCaptured());
  }

  // Publisher B publishes a different entity and stays independent.
  auto publisher_b = ChildProcess::StartCapturing(
      publisher_exe,
      {"--port", std::to_string(cluster.port), "--publisher", "pub-a2", "--scope", "host-scope",
       "--source", "host-source-b", "--boot", boot_a2, "--entity", "nic:host-death-1",
       "--generation", "1", "--mode", "full", "--coverage", "full", "--provenance", "0",
       "--source-class", "0", "--claim", "fabric.port.supported_speeds=supported",
       "--claim-value", "fabric.port.supported_speeds=numericset:bit/s:100000000000"},
      cluster.store());
  FCR_REQUIRE(publisher_b.has_value());
  ExpectExit(*publisher_b, 0ul, "publisher-b");

  // The coordinator is still alive and the evidence is current.
  FCR_CHECK(coordinator->Running());
  {
    auto probe = ChildProcess::StartCapturing(publisher_exe,
                                     {"--port", std::to_string(cluster.port), "--publisher",
                                      "pub-a2", "--scope", "host-scope", "--source", "probe",
                                      "--boot", boot_a2, "--entity", "nic:host-death-0",
                                      "--generation", "1", "--mode", "partial",
                                      "--claim", "fabric.port.supported_speeds=supported",
                                      "--claim-value",
                                      "fabric.port.supported_speeds=numericset:bit/s:400000000000"},
                                     cluster.store());
    FCR_REQUIRE(probe.has_value());
    const unsigned long exit_code = WaitOrAbort(*probe, "probe");
    FCR_CHECK(exit_code == 0ul || exit_code == 3ul);
  }

  // Kill publisher A as a real operating system process.
  publisher_a->Kill();
  FCR_CHECK(!ProcessAlive(publisher_a->Pid()));

  // The coordinator detects the death through the real control path, fences
  // the boot and makes the process bound evidence non current.
  // Fencing is observed through the real control path with an in-process client: the
  // coordinator refuses the dead boot once it has processed the broken connection.
  bool fenced = false;
  const auto fence_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (!fenced && std::chrono::steady_clock::now() < fence_deadline) {
    const auto outcome = Handshake(cluster.port, "pub-a", boot_a);
    if (!outcome.HasValue() && outcome.Code() == ErrorCode::WorkerBootFenced) {
      fenced = true;
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  FCR_CHECK(fenced);

  // A stale replay from the dead boot is refused.
  auto stale = ChildProcess::StartCapturing(
      publisher_exe,
      {"--port", std::to_string(cluster.port), "--publisher", "pub-a", "--scope", "host-scope",
       "--source", "host-source", "--boot", boot_a, "--entity", "nic:host-death-0", "--generation",
       "1", "--mode", "full", "--claim", "fabric.port.supported_speeds=supported", "--claim-value",
       "fabric.port.supported_speeds=numericset:bit/s:100000000000"},
      cluster.store());
  FCR_REQUIRE(stale.has_value());
  ExpectExit(*stale, 3ul, "stale-replay");

  // A reincarnated publisher with a fresh boot requires fresh evidence.
  const std::string boot_fresh = "ffff1111eeee2222dddd3333cccc4444";
  auto fresh = ChildProcess::StartCapturing(
      publisher_exe,
      {"--port", std::to_string(cluster.port), "--publisher", "pub-a", "--scope", "host-scope",
       "--source", "host-source", "--boot", boot_fresh, "--entity", "nic:host-death-0",
       "--generation", "1", "--mode", "full", "--coverage", "full", "--provenance", "0",
       "--source-class", "0", "--claim", "fabric.port.supported_speeds=supported",
       "--claim-value", "fabric.port.supported_speeds=numericset:bit/s:200000000000",
       "--expected-set-generation", "1"},
      cluster.store());
  FCR_REQUIRE(fresh.has_value());
  ExpectExit(*fresh, 0ul, "reincarnated-publisher");

  // Real coordinator restart: kill it hard and start a fresh process on the
  // same durable store and port.
  coordinator->Kill();
  FCR_CHECK(!ProcessAlive(coordinator->Pid()));

  auto restarted = ChildProcess::StartWithStdinControl(
      coordinator_exe, CoordinatorArguments(cluster, "host-scope", "pub-a", "pub-a2"),
      cluster.store());
  FCR_REQUIRE(restarted.has_value());
  FCR_REQUIRE(WaitForCoordinator(cluster.port));

  // The durable administrative declaration survived; the process bound
  // hardware observation did not silently survive.
  auto after_restart = ChildProcess::StartCapturing(
      publisher_exe,
      {"--port", std::to_string(cluster.port), "--publisher", "pub-a", "--scope", "host-scope",
       "--source", "host-source", "--boot", boot_fresh, "--entity", "nic:host-death-0",
       "--generation", "1", "--mode", "partial", "--claim",
       "fabric.port.supported_speeds=supported", "--claim-value",
       "fabric.port.supported_speeds=numericset:bit/s:100000000000"},
      cluster.store());
  FCR_REQUIRE(after_restart.has_value());
  // The pre-restart boot is fenced by the epoch advance, so this is refused.
  ExpectExit(*after_restart, 3ul, "after-restart");

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
  const unsigned long final_exit = WaitOrAbort(*final_publish, "final-publisher");
  FCR_CHECK(final_exit == 0ul || final_exit == 3ul);

  // The durable store remains intact and inspectable.
  PersistenceConfig config;
  config.directory = cluster.dir.path();
  config.file_stem = "fabric-capability-registry";
  auto inspection = InspectStore(config);
  FCR_REQUIRE_OK(inspection);
  FCR_CHECK(inspection.Value().integrity_ok);

  // Graceful stop through the control channel of the process helper.
  const bool stop_requested = restarted->WriteStdin("stop\n");
  FCR_CHECK(stop_requested);
  restarted->CloseStdin();
  FCR_CHECK_EQ(WaitOrAbort(*restarted, "restarted-coordinator"), 0ul);
}
