// Fabric Capability Registry test suite: fcrctl inspection CLI.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>
#include <vector>

#include "fabric/capability/fabric_capability.hpp"
#include "process_helper.hpp"
#include "temp_dir.hpp"
#include "test_framework.hpp"

using namespace fabric::capability;
using namespace fcr::test;

namespace {

struct Run {
  long exit_code = -1;
  std::string output;
};

Run RunCli(const std::vector<std::string>& arguments) {
  Run run;
  const std::string executable = CliExecutablePath();
  if (executable.empty()) {
    FCR_FAIL("the fcrctl executable path is not configured");
    return run;
  }
  auto child = ChildProcess::StartCapturing(executable, arguments);
  if (!child.has_value()) {
    FCR_FAIL("fcrctl could not be started");
    return run;
  }
  run.exit_code = static_cast<long>(child->WaitForExit());
  run.output = child->ReadCaptured();
  return run;
}

bool HasLine(const std::string& output, const std::string& prefix) {
  std::size_t start = 0;
  while (start <= output.size()) {
    const std::size_t end = output.find('\n', start);
    const std::string line = output.substr(start, end == std::string::npos ? std::string::npos
                                                                          : end - start);
    if (line.rfind(prefix, 0) == 0) return true;
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return false;
}

}  // namespace

FCR_TEST(cli, version_and_usage) {
  const Run version = RunCli({"version"});
  FCR_CHECK_EQ(version.exit_code, 0l);
  FCR_CHECK(version.output.find("FabricCapabilityRegistry 1.0.0") != std::string::npos);

  FCR_CHECK_EQ(RunCli({"not-a-command"}).exit_code, 2l);
  FCR_CHECK_EQ(RunCli({"entities"}).exit_code, 2l);
}

FCR_TEST(cli, import_inspect_and_query) {
  TempDir dir("cli");
  const std::string store = dir.string();
  const Run imported = RunCli({"import-synthetic", "--store", store, "--class", "leaf-switch",
                                "--devices", "2", "--seed", "3"});
  FCR_CHECK_EQ(imported.exit_code, 0l);
  FCR_CHECK(HasLine(imported.output, "imported entities="));
  FCR_CHECK(HasLine(imported.output, "digest="));

  const Run entities = RunCli({"entities", "--store", store});
  FCR_CHECK_EQ(entities.exit_code, 0l);
  FCR_CHECK(HasLine(entities.output, "entity=switch:syn-leaf-0000"));
  FCR_CHECK(entities.output.find('\t') == std::string::npos);

  const Run stats = RunCli({"stats", "--store", store});
  FCR_CHECK_EQ(stats.exit_code, 0l);
  FCR_CHECK(HasLine(stats.output, "entities=2"));

  const Run inspect = RunCli({"inspect", "--store", store});
  FCR_CHECK_EQ(inspect.exit_code, 0l);
  FCR_CHECK(HasLine(inspect.output, "integrity=ok"));

  const Run capability = RunCli({"capability", "--store", store, "--entity",
                                 "switch:syn-leaf-0000", "--capability",
                                 "fabric.port.supported_speeds"});
  FCR_CHECK_EQ(capability.exit_code, 0l);
  FCR_CHECK(capability.output.find("state=SUPPORTED") != std::string::npos);

  const Run unknown = RunCli({"capability", "--store", store, "--entity", "switch:syn-leaf-0000",
                              "--capability", "fabric.port.not_a_capability"});
  FCR_CHECK_EQ(unknown.exit_code, 3l);

  const Run explain = RunCli({"explain", "--store", store, "--entity", "switch:syn-leaf-0000",
                              "--capability", "fabric.port.supported_speeds"});
  FCR_CHECK_EQ(explain.exit_code, 0l);
  FCR_CHECK(explain.output.find("synthetic-test-backend") != std::string::npos);

  const Run satisfied = RunCli({"query", "--store", store, "--entity", "switch:syn-leaf-0000",
                                "--require", "fabric.port.supported_speeds=supported"});
  FCR_CHECK_EQ(satisfied.exit_code, 0l);
  FCR_CHECK(satisfied.output.find("outcome=satisfied") != std::string::npos);

  const Run unsatisfied = RunCli({"query", "--store", store, "--entity", "switch:syn-leaf-0000",
                                  "--require", "fabric.offload.rdma=supported"});
  FCR_CHECK_EQ(unsatisfied.exit_code, 3l);
  FCR_CHECK(unsatisfied.output.find("outcome=undetermined") != std::string::npos ||
            unsatisfied.output.find("outcome=not-satisfied") != std::string::npos);

  const Run diff = RunCli({"diff", "--store", store, "--against-store", store});
  FCR_CHECK_EQ(diff.exit_code, 0l);

  // The CLI is deterministic.
  const Run repeat = RunCli({"entities", "--store", store});
  FCR_CHECK_EQ(repeat.output, entities.output);
  const Run synthetic_one = RunCli({"synthetic", "show", "--class", "spine-switch", "--devices", "2"});
  FCR_CHECK_EQ(synthetic_one.exit_code, 0l);
  const Run synthetic_two = RunCli({"synthetic", "show", "--class", "spine-switch", "--devices", "2"});
  FCR_CHECK_EQ(synthetic_one.output, synthetic_two.output);
  const Run listed = RunCli({"synthetic", "list"});
  FCR_CHECK_EQ(listed.exit_code, 0l);
  FCR_CHECK(listed.output.find("class=leaf-switch") != std::string::npos);

  // REAL discovery runs on this host and is deterministic.
  const Run discovery = RunCli({"discover"});
  FCR_CHECK_EQ(discovery.exit_code, 0l);
  FCR_CHECK(discovery.output.find('\t') == std::string::npos);
  FCR_CHECK_EQ(RunCli({"discover"}).output, discovery.output);

  // Re-importing the same store remains valid and inspectable.
  const Run reimport = RunCli({"import-synthetic", "--store", store, "--class", "leaf-switch",
                               "--devices", "2", "--seed", "3"});
  FCR_CHECK_EQ(reimport.exit_code, 0l);
  FCR_CHECK(HasLine(RunCli({"inspect", "--store", store}).output, "integrity=ok"));
}
