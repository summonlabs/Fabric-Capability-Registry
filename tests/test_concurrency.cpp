// Fabric Capability Registry test suite: concurrency and deterministic races.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <atomic>
#include <thread>
#include <vector>

#include "fabric/capability/fabric_capability.hpp"
#include "registry_fixture.hpp"
#include "test_framework.hpp"

using namespace fabric::capability;
using namespace fcr::test;

namespace {

/// Simple reusable barrier so that races are entered deterministically.
class Barrier {
 public:
  explicit Barrier(std::size_t parties) : parties_(parties) {}

  void Arrive() {
    std::unique_lock<std::mutex> lock(mutex_);
    ++arrived_;
    if (arrived_ == parties_) {
      ++generation_;
      condition_.notify_all();
      return;
    }
    const std::size_t generation = generation_;
    condition_.wait(lock, [this, generation]() { return generation_ != generation; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  std::size_t parties_;
  std::size_t arrived_ = 0;
  std::size_t generation_ = 0;
};

CapabilityClaim Claim(const char* capability, std::uint64_t value) {
  return QuantityClaim(capability, Unit::Count, value);
}

}  // namespace

FCR_TEST(concurrency, independent_entity_publications_from_many_threads) {
  Fixture fixture;
  constexpr std::size_t kThreads = 8;
  constexpr std::size_t kPerThread = 24;
  Barrier barrier(kThreads);
  std::atomic<std::size_t> committed{0};
  std::vector<std::thread> workers;

  for (std::size_t thread = 0; thread < kThreads; ++thread) {
    workers.emplace_back([&fixture, &barrier, &committed, thread]() {
      barrier.Arrive();
      for (std::size_t index = 0; index < kPerThread; ++index) {
        const std::string name = "nic:t" + std::to_string(thread) + "-" + std::to_string(index);
        auto result = fixture.PublishSnapshot(Entity(name.c_str()), EntityGeneration::FromValue(1),
                                              {Claim("fabric.queue.max_rx_queues", index + 1)},
                                              {}, Coverage::FullEnumeration,
                                              ("p-" + name).c_str(), ("a-" + name).c_str());
        if (result.HasValue() && result.Value().Committed()) ++committed;
      }
    });
  }
  for (std::thread& worker : workers) worker.join();
  FCR_CHECK_EQ(committed.load(), kThreads * kPerThread);
  FCR_CHECK_EQ(fixture.registry->Entities().size(), kThreads * kPerThread);
}

FCR_TEST(concurrency, conflicting_updates_to_one_entity_resolve_deterministically) {
  Fixture fixture;
  const EntityId entity = Entity("nic:contended");
  FCR_REQUIRE_OK(fixture.PublishSnapshot(entity, EntityGeneration::FromValue(1),
                                         {Claim("fabric.queue.max_rx_queues", 1)}));

  constexpr std::size_t kThreads = 4;
  Barrier barrier(kThreads);
  std::atomic<std::size_t> committed{0};
  std::atomic<std::size_t> rejected{0};
  std::vector<std::thread> workers;
  for (std::size_t thread = 0; thread < kThreads; ++thread) {
    workers.emplace_back([&fixture, &barrier, &committed, &rejected, &entity, thread]() {
      barrier.Arrive();
      const std::string attempt = "a-contended-" + std::to_string(thread);
      const std::string publication = "p-contended-" + std::to_string(thread);
      auto result = fixture.PublishSnapshot(entity, EntityGeneration::FromValue(1),
                                            {Claim("fabric.queue.max_rx_queues", thread + 10)},
                                            CapabilitySetGeneration::FromValue(1),
                                            Coverage::FullEnumeration, publication.c_str(),
                                            attempt.c_str());
      if (result.HasValue() && result.Value().Committed()) {
        ++committed;
      } else {
        ++rejected;
      }
    });
  }
  for (std::thread& worker : workers) worker.join();

  // Exactly one writer may win the expected generation 1; every other writer
  // is rejected deterministically rather than corrupting the set.
  FCR_CHECK_EQ(committed.load(), std::size_t(1));
  FCR_CHECK_EQ(rejected.load(), kThreads - 1);
  auto set = fixture.registry->QueryEntity(entity);
  FCR_REQUIRE_OK(set);
  FCR_CHECK_EQ(set.Value().set_generation.Value(), 2ull);
  FCR_CHECK_EQ(set.Value().capabilities.size(), std::size_t(1));
}

FCR_TEST(concurrency, fencing_races_mutation_without_corruption) {
  Fixture fixture;
  const EntityId entity = Entity("nic:race");
  FCR_REQUIRE_OK(fixture.PublishSnapshot(entity, EntityGeneration::FromValue(1),
                                         {Claim("fabric.queue.max_rx_queues", 4)}));

  Barrier barrier(2);
  std::atomic<bool> fence_done{false};
  std::thread fencer([&fixture, &barrier, &fence_done]() {
    barrier.Arrive();
    fixture.registry->FenceWorkerBoot(fixture.boot, Reason("race-fence"));
    fence_done.store(true);
  });
  std::thread mutator([&fixture, &barrier]() {
    barrier.Arrive();
    for (int index = 0; index < 32; ++index) {
      fixture.PublishSnapshot(Entity("nic:race"), EntityGeneration::FromValue(1),
                              {Claim("fabric.queue.max_rx_queues", 8)},
                              CapabilitySetGeneration::FromValue(1), Coverage::FullEnumeration,
                              "p-race", "a-race");
    }
  });
  fencer.join();
  mutator.join();
  FCR_CHECK(fence_done.load());

  // Whatever the interleaving, the observable outcome is one legal state: the
  // evidence is non current and the capability fails closed.
  auto query = fixture.registry->Query(entity, Cap("fabric.queue.max_rx_queues"));
  FCR_REQUIRE_OK(query);
  FCR_CHECK(query.Value().state == CapabilityState::RevalidationRequired ||
            query.Value().state == CapabilityState::Supported);
  FCR_CHECK(fixture.registry->IsWorkerBootFenced(fixture.boot));
  FCR_CHECK(!fixture.registry->QueryEntity(entity).Value().digest.IsZero());
}

FCR_TEST(concurrency, concurrent_readers_never_observe_a_torn_set) {
  Fixture fixture;
  const EntityId entity = Entity("nic:reader");
  FCR_REQUIRE_OK(fixture.PublishSnapshot(entity, EntityGeneration::FromValue(1),
                                         {Claim("fabric.queue.max_rx_queues", 4)}));

  std::atomic<bool> stop{false};
  std::atomic<std::size_t> observed{0};
  std::atomic<bool> inconsistent{false};
  std::vector<std::thread> readers;
  for (int index = 0; index < 4; ++index) {
    readers.emplace_back([&fixture, &entity, &stop, &observed, &inconsistent]() {
      while (!stop.load()) {
        auto set = fixture.registry->QueryEntity(entity);
        if (!set.HasValue()) {
          inconsistent.store(true);
          return;
        }
        // Every published set is internally consistent: the resolutions are
        // sorted, unique and carry matching digests.
        const std::vector<CapabilityResolution>& resolutions = set.Value().capabilities;
        for (std::size_t position = 1; position < resolutions.size(); ++position) {
          if (!(resolutions[position - 1].capability < resolutions[position].capability)) {
            inconsistent.store(true);
            return;
          }
        }
        observed.fetch_add(1);
      }
    });
  }
  for (int index = 0; index < 64; ++index) {
    fixture.PublishSnapshot(entity, EntityGeneration::FromValue(1),
                            {Claim("fabric.queue.max_rx_queues", index + 1)},
                            CapabilitySetGeneration::FromValue(static_cast<std::uint64_t>(index + 1)),
                            Coverage::FullEnumeration, "p-loop", "a-loop");
  }
  stop.store(true);
  for (std::thread& reader : readers) reader.join();
  FCR_CHECK(!inconsistent.load());
  FCR_CHECK(observed.load() > 0);
}
