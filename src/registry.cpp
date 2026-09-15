// Fabric Capability Registry - authoritative capability knowledge runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric/capability/registry.hpp"

#include <algorithm>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

#include "fabric/capability/encoding.hpp"
#include "fabric/capability/limits.hpp"
#include "fabric/capability/version.hpp"
#include "src/internal/persistence_io.hpp"
#include "src/internal/requirement_internal.hpp"

namespace fabric::capability {
namespace {

/// Key of a publishing identity: one publisher inside one worker boot.
struct PublisherKey {
  PublisherId publisher;
  WorkerBootId boot;

  friend bool operator==(const PublisherKey&, const PublisherKey&) = default;
};

struct PublisherKeyHash {
  std::size_t operator()(const PublisherKey& key) const noexcept {
    return key.publisher.Hash() * 1099511628211ull ^ key.boot.Hash();
  }
};

/// Key of an evidence-owning source inside one worker boot.
struct SourceKey {
  SourceId source;
  WorkerBootId boot;

  friend bool operator==(const SourceKey&, const SourceKey&) = default;
  friend bool operator<(const SourceKey& lhs, const SourceKey& rhs) {
    if (lhs.source != rhs.source) return lhs.source < rhs.source;
    return lhs.boot < rhs.boot;
  }
};

std::string DigestHex(const Digest& digest) { return digest.ToString(); }

Digest DigestOfResolutions(const std::vector<CapabilityResolution>& resolutions) {
  std::vector<std::byte> bytes;
  bytes.reserve(resolutions.size() * 64);
  ByteWriter writer(bytes);
  writer.U32(static_cast<std::uint32_t>(resolutions.size()));
  for (const CapabilityResolution& resolution : resolutions) {
    writer.Text(resolution.capability.ToString(), limits::kMaxCapabilityIdLength);
    writer.U8(static_cast<std::uint8_t>(resolution.state));
    writer.Bool(resolution.has_value);
    if (resolution.has_value) resolution.value.Encode(writer);
    writer.U8(ProvenanceRank(resolution.provenance));
    writer.U8(static_cast<std::uint8_t>(resolution.source_class));
    writer.U8(static_cast<std::uint8_t>(resolution.coverage));
    writer.U8(static_cast<std::uint8_t>(resolution.durability));
    // Publisher-local source sequencing is deliberately excluded: the canonical digest
    // must describe capability truth, not the bookkeeping order of one publisher.
    writer.U64(resolution.evidence_generation.Value());
    writer.U64(resolution.capability_generation.Value());
    // The winning evidence identifier is publisher-assigned bookkeeping: it identifies an
    // observation, not the capability truth, so it is excluded from the canonical digest.
    writer.U32(static_cast<std::uint32_t>(resolution.evidence_count));
    writer.U32(static_cast<std::uint32_t>(resolution.current_evidence_count));
    writer.U32(static_cast<std::uint32_t>(resolution.outranked_evidence_count));
    writer.U32(static_cast<std::uint32_t>(resolution.conflicting_evidence_count));
    writer.Bool(resolution.has_non_current_evidence);
  }
  return ComputeDigest(bytes);
}

/// Deterministic winner selection inside one provenance class: strongest
/// provenance first, then the newest evidence generation, then the lowest
/// source identifier and finally the lowest worker boot identifier, so that
/// the choice never depends on arrival order or map iteration order.
const EvidenceRecord* PickDeterministic(const std::vector<const EvidenceRecord*>& records) {
  const EvidenceRecord* best = nullptr;
  for (const EvidenceRecord* record : records) {
    if (best == nullptr) {
      best = record;
      continue;
    }
    if (record->evidence_generation.Value() > best->evidence_generation.Value()) {
      best = record;
      continue;
    }
    if (record->evidence_generation.Value() < best->evidence_generation.Value()) continue;
    if (record->source != best->source) {
      if (record->source < best->source) best = record;
      continue;
    }
    if (record->worker_boot < best->worker_boot) best = record;
  }
  return best;
}

bool SameClaim(const EvidenceRecord& lhs, const EvidenceRecord& rhs) {
  if (lhs.state != rhs.state) return false;
  if (lhs.value.IsAbsent() != rhs.value.IsAbsent()) return false;
  if (lhs.value.IsAbsent()) return true;
  return lhs.value == rhs.value;
}

ExplanationStep MakeStep(std::string code, std::string summary) {
  ExplanationStep step;
  step.code = std::move(code);
  step.summary = std::move(summary);
  return step;
}

void AddField(ExplanationStep& step, std::string name, std::string value) {
  if (step.fields.size() >= limits::kMaxTextFieldsPerExplanation) return;
  step.fields.emplace_back(std::move(name), std::move(value));
}

/// Builds a bounded printable reason token, falling back to an unset token
/// when the text cannot be represented.
ReasonToken MakeReason(std::string_view text) {
  auto parsed = ReasonToken::Parse(text);
  return parsed.HasValue() ? parsed.Value() : ReasonToken{};
}

}  // namespace

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

struct CapabilityEntry {
  CapabilityId id;
  CapabilityGeneration generation;
  std::vector<EvidencePtr> evidence;
  CapabilityResolution resolution;
};

struct RetainedGeneration {
  EntityGenerationSummary summary;
  std::vector<CapabilityId> capability_ids;
};

struct EntityState {
  EntityId id;
  EntityGeneration current_generation;
  RegistryGeneration touched_at;
  bool has_set = false;
  EntityGeneration bound_generation;
  CapabilitySetGeneration set_generation;
  bool invalidated = false;
  ReasonToken invalidation_reason;
  std::map<std::uint32_t, CapabilityEntry> capabilities;
  std::map<SourceKey, SourceGeneration> source_high_water;
  std::deque<RetainedGeneration> history;
  std::shared_ptr<const EntityCapabilitySet> set;
};

struct ReverseEntry {
  std::vector<std::uint32_t> supported;
  std::vector<std::uint32_t> unsupported;
  std::vector<std::uint32_t> unknown;
  std::vector<std::uint32_t> revalidation_required;
  std::vector<std::uint32_t> conflicted;

  std::vector<std::uint32_t>* Bucket(CapabilityState state) {
    switch (state) {
      case CapabilityState::Supported:
        return &supported;
      case CapabilityState::Unsupported:
        return &unsupported;
      case CapabilityState::Unknown:
        return &unknown;
      case CapabilityState::RevalidationRequired:
        return &revalidation_required;
      case CapabilityState::Conflicted:
        return &conflicted;
    }
    return &unknown;
  }
};

struct ReplayRecord {
  MutationAttemptId attempt;
  PublicationId publication;
  std::string content_digest;
  PublicationStatus status = PublicationStatus::Committed;
  ErrorCode code = ErrorCode::Ok;
};

struct PublisherRuntime {
  PublisherRegistration registration;
  bool active = false;
  std::deque<ReplayRecord> replay;
};

struct JournalEntry {
  PublicationId publication;
  MutationAttemptId attempt;
  EntityId entity;
  EntityGeneration entity_generation;
  PublicationStatus status = PublicationStatus::Rejected;
  ErrorCode code = ErrorCode::Ok;
  std::string message;
  CapabilitySetGeneration previous_generation;
  CapabilitySetGeneration new_generation;
  RegistryGeneration registry_generation;
  Explanation explanation;
};

void InsertSorted(std::vector<std::uint32_t>& values, std::uint32_t value) {
  const auto position = std::lower_bound(values.begin(), values.end(), value);
  if (position != values.end() && *position == value) return;
  values.insert(position, value);
}

void EraseSorted(std::vector<std::uint32_t>& values, std::uint32_t value) {
  const auto position = std::lower_bound(values.begin(), values.end(), value);
  if (position == values.end() || *position != value) return;
  values.erase(position);
}

// ---------------------------------------------------------------------------
// Registry implementation
// ---------------------------------------------------------------------------

class CapabilityRegistry::Impl {
 public:
  Impl(Options options, CapabilitySchema& schema)
      : options_(std::move(options)), schema_(schema), epoch_(options_.initial_epoch) {
    if (!options_.initial_epoch.IsSet()) {
      options_.initial_epoch = CoordinatorEpoch::FromValue(1);
      epoch_ = options_.initial_epoch;
    }
  }

  // --- helper types -------------------------------------------------------

  CapabilityEntry* FindCapability(EntityState& entity, std::uint32_t key) {
    const auto found = entity.capabilities.find(key);
    return found == entity.capabilities.end() ? nullptr : &found->second;
  }

  EntityState* FindEntity(const EntityId& id) {
    const auto found = entity_keys_.find(id);
    if (found == entity_keys_.end()) return nullptr;
    return &entities_[found->second];
  }

  const EntityState* FindEntity(const EntityId& id) const {
    const auto found = entity_keys_.find(id);
    if (found == entity_keys_.end()) return nullptr;
    return &entities_[found->second];
  }

  std::uint32_t CapabilityKey(const CapabilityId& id) {
    const auto found = capability_keys_.find(id);
    if (found != capability_keys_.end()) return found->second;
    const std::uint32_t key = static_cast<std::uint32_t>(capability_ids_.size());
    capability_ids_.push_back(id);
    capability_keys_.emplace(id, key);
    return key;
  }

  ReverseEntry& ReverseFor(std::uint32_t key) { return reverse_[key]; }

  void RemoveFromReverse(std::uint32_t entity_key, std::uint32_t capability_key,
                         CapabilityState state) {
    const auto found = reverse_.find(capability_key);
    if (found == reverse_.end()) return;
    EraseSorted(*found->second.Bucket(state), entity_key);
  }

  void AddToReverse(std::uint32_t entity_key, std::uint32_t capability_key, CapabilityState state) {
    InsertSorted(*ReverseFor(capability_key).Bucket(state), entity_key);
  }

  void UpdateRevalidationIndex(const EntityId& entity, const CapabilityId& capability,
                               CapabilityState previous, bool had_previous,
                               CapabilityState current) {
    const auto key = std::make_pair(entity, capability);
    const bool was = had_previous && previous == CapabilityState::RevalidationRequired;
    const bool is = current == CapabilityState::RevalidationRequired;
    if (was && !is) {
      revalidation_.erase(key);
    } else if (!was && is) {
      revalidation_.insert(key);
    }
  }

  void IndexEvidence(const EvidenceRecord& record) {
    publisher_evidence_[record.publisher].insert(record.id);
    source_evidence_[record.source].insert(record.id);
  }

  void DeindexEvidence(const EvidenceRecord& record) {
    const auto publisher = publisher_evidence_.find(record.publisher);
    if (publisher != publisher_evidence_.end()) {
      publisher->second.erase(record.id);
      if (publisher->second.empty()) publisher_evidence_.erase(publisher);
    }
    const auto source = source_evidence_.find(record.source);
    if (source != source_evidence_.end()) {
      source->second.erase(record.id);
      if (source->second.empty()) source_evidence_.erase(source);
    }
  }

  static void TouchNamespace(
      std::unordered_map<CapabilityNamespaceId, std::set<CapabilityId>>& index,
      const CapabilityId& capability) {
    index[capability.Namespace()].insert(capability);
  }

  // --- resolution ---------------------------------------------------------

  void ResolveCapability(EntityState& entity, std::uint32_t key, CapabilityEntry& entry) {
    std::vector<const EvidenceRecord*> current;
    std::vector<const EvidenceRecord*> non_current;
    std::vector<const EvidenceRecord*> retired;
    for (const EvidencePtr& record : entry.evidence) {
      switch (record->currentness) {
        case EvidenceCurrentness::Current:
          current.push_back(record.get());
          break;
        case EvidenceCurrentness::RevalidationRequired:
        case EvidenceCurrentness::Fenced:
          non_current.push_back(record.get());
          break;
        case EvidenceCurrentness::Superseded:
        case EvidenceCurrentness::EntityGenerationSuperseded:
          retired.push_back(record.get());
          break;
      }
    }

    CapabilityResolution resolution;
    resolution.capability = entry.id;
    resolution.record_id = MakeCapabilityRecordId(entity.id, entity.bound_generation, entry.id);
    resolution.evidence_count = current.size() + non_current.size() + retired.size();
    resolution.current_evidence_count = current.size();
    resolution.capability_generation = entry.generation;
    resolution.has_non_current_evidence = !non_current.empty();

    std::vector<const EvidenceRecord*> pool;
    pool.reserve(current.size() + non_current.size());
    pool.insert(pool.end(), current.begin(), current.end());
    pool.insert(pool.end(), non_current.begin(), non_current.end());

    if (pool.empty()) {
      resolution.state = CapabilityState::Unknown;
      resolution.conflicting_evidence_count = 0;
      FinishResolution(entity, key, entry, resolution);
      return;
    }

    std::uint8_t strongest_rank = kProvenanceClassCount;
    for (const EvidenceRecord* record : pool) {
      strongest_rank = std::min(strongest_rank, ProvenanceRank(record->provenance));
    }
    std::vector<const EvidenceRecord*> strongest;
    std::size_t outranked = 0;
    for (const EvidenceRecord* record : pool) {
      if (ProvenanceRank(record->provenance) == strongest_rank) {
        strongest.push_back(record);
      } else {
        ++outranked;
      }
    }
    resolution.outranked_evidence_count = outranked;

    std::vector<const EvidenceRecord*> strongest_current;
    for (const EvidenceRecord* record : strongest) {
      if (IsCurrent(record->currentness)) strongest_current.push_back(record);
    }

    if (strongest_current.empty()) {
      const EvidenceRecord* winner = PickDeterministic(strongest);
      resolution.state = CapabilityState::RevalidationRequired;
      resolution.winning_evidence = winner->id;
      resolution.provenance = winner->provenance;
      resolution.source_class = winner->source_class;
      resolution.coverage = winner->coverage;
      resolution.durability = winner->durability;
      resolution.evidence_generation = winner->evidence_generation;
      resolution.source_generation = winner->source_generation;
      resolution.value = winner->value;
      resolution.has_value = !winner->value.IsAbsent();
      resolution.value_digest = resolution.has_value ? DigestOf(resolution.value) : Digest{};
      FinishResolution(entity, key, entry, resolution);
      return;
    }

    const EvidenceRecord* winner = PickDeterministic(strongest_current);
    bool agree = true;
    for (const EvidenceRecord* record : strongest_current) {
      if (!SameClaim(*record, *winner)) {
        agree = false;
        break;
      }
    }

    resolution.winning_evidence = winner->id;
    resolution.provenance = winner->provenance;
    resolution.source_class = winner->source_class;
    resolution.coverage = winner->coverage;
    resolution.durability = winner->durability;
    resolution.evidence_generation = winner->evidence_generation;
    resolution.source_generation = winner->source_generation;

    if (agree) {
      resolution.state = winner->state;
      resolution.value = winner->value;
      resolution.has_value = !winner->value.IsAbsent();
      resolution.value_digest = resolution.has_value ? DigestOf(resolution.value) : Digest{};
      FinishResolution(entity, key, entry, resolution);
      return;
    }

    resolution.state = CapabilityState::Conflicted;
    for (const EvidenceRecord* record : strongest_current) {
      bool already = false;
      for (const EvidenceRecord* seen : strongest_current) {
        if (seen == record) break;
        if (!SameClaim(*seen, *record)) continue;
        already = true;
        break;
      }
      if (already) continue;
      if (resolution.conflicting_evidence.size() < limits::kMaxEvidencePerCapability) {
        resolution.conflicting_evidence.push_back(record->id);
      }
    }
    resolution.conflicting_evidence_count = resolution.conflicting_evidence.size();
    FinishResolution(entity, key, entry, resolution);
  }

  void FinishResolution(EntityState& entity, std::uint32_t key, CapabilityEntry& entry,
                        CapabilityResolution& resolution) {
    const CapabilityState previous = entry.resolution.state;
    const bool had = entry.resolution.capability.IsSet();
    entry.resolution = std::move(resolution);
    const std::uint32_t entity_key = entity_keys_[entity.id];
    if (had && previous != entry.resolution.state) {
      RemoveFromReverse(entity_key, key, previous);
      AddToReverse(entity_key, key, entry.resolution.state);
    } else if (!had) {
      AddToReverse(entity_key, key, entry.resolution.state);
    }
    UpdateRevalidationIndex(entity.id, entry.id, previous, had, entry.resolution.state);
    TouchNamespace(namespace_index_, entry.id);
  }

  // --- materialisation ----------------------------------------------------

  std::shared_ptr<EntityCapabilitySet> Materialize(const EntityState& entity) const {
    auto set = std::make_shared<EntityCapabilitySet>();
    set->entity = entity.id;
    set->entity_generation = entity.bound_generation;
    set->set_generation = entity.set_generation;
    set->registry_generation = registry_generation_;
    set->invalidated = entity.invalidated;
    set->invalidation_reason = entity.invalidation_reason;
    set->capabilities.reserve(entity.capabilities.size());
    for (const auto& entry : entity.capabilities) {
      set->capabilities.push_back(entry.second.resolution);
    }
    std::sort(set->capabilities.begin(), set->capabilities.end(),
              [](const CapabilityResolution& lhs, const CapabilityResolution& rhs) {
                return lhs.capability < rhs.capability;
              });
    set->digest = DigestOfResolutions(set->capabilities);
    return set;
  }

  /// Rebuilds the immutable set of one entity. Returns true when the semantic
  /// digest changed.
  bool RefreshEntity(EntityState& entity) {
    const Digest before = entity.set != nullptr ? entity.set->digest : Digest{};
    const bool had_set = entity.set != nullptr;
    auto next = Materialize(entity);
    next->set_generation = entity.set_generation;
    next->registry_generation = registry_generation_;
    const bool changed = !had_set || !(before == next->digest);
    entity.set = std::move(next);
    entity.touched_at = registry_generation_;
    return changed;
  }

  // --- explanations -------------------------------------------------------

  Explanation ExplainResolution(const EntityState* entity, const CapabilityEntry* entry) const {
    Explanation explanation;
    if (entity == nullptr) {
      explanation.code = "capability.entity-unknown";
      explanation.summary = "the entity is not known to this registry";
      return explanation;
    }
    if (entry == nullptr) {
      explanation.code = "capability.no-record";
      explanation.summary = "no capability record exists for this entity generation";
      return explanation;
    }
    const CapabilityResolution& resolution = entry->resolution;
    explanation.code = std::string("capability.") + std::string(CapabilityStateName(resolution.state));
    explanation.summary = std::string("resolved ") + entry->id.ToString() + " as " +
                          std::string(CapabilityStateName(resolution.state)) + " for " +
                          entity->id.ToString() + " generation " +
                          entity->bound_generation.ToString();

    ExplanationStep criteria =
        MakeStep("resolution.criteria",
                 "strongest provenance class wins; agreement within the class resolves, "
                 "disagreement is CONFLICTED, absence of current evidence in the strongest "
                 "class is REVALIDATION_REQUIRED, and no claim at all is UNKNOWN");
    AddField(criteria, "evidence", std::to_string(resolution.evidence_count));
    AddField(criteria, "current_evidence", std::to_string(resolution.current_evidence_count));
    AddField(criteria, "outranked_evidence", std::to_string(resolution.outranked_evidence_count));
    AddField(criteria, "conflicting_evidence", std::to_string(resolution.conflicting_evidence_count));
    explanation.steps.push_back(std::move(criteria));

    if (resolution.winning_evidence.IsSet()) {
      ExplanationStep winner = MakeStep("resolution.winner", "winning evidence");
      AddField(winner, "evidence", resolution.winning_evidence.Value());
      AddField(winner, "provenance", std::string(ProvenanceClassName(resolution.provenance)));
      AddField(winner, "source_class", std::string(EvidenceSourceClassName(resolution.source_class)));
      AddField(winner, "coverage", std::string(CoverageName(resolution.coverage)));
      AddField(winner, "durability", std::string(DurabilityClassName(resolution.durability)));
      AddField(winner, "evidence_generation", resolution.evidence_generation.ToString());
      AddField(winner, "source_generation", resolution.source_generation.ToString());
      explanation.steps.push_back(std::move(winner));
    }

    std::size_t suppressed = 0;
    for (const EvidencePtr& record : entry->evidence) {
      if (!IsCurrent(record->currentness)) {
        ++suppressed;
        if (suppressed > 8) continue;
        ExplanationStep step = MakeStep("evidence.non-current",
                                        "evidence retained but not current");
        AddField(step, "evidence", record->id.Value());
        AddField(step, "currentness", std::string(EvidenceCurrentnessName(record->currentness)));
        AddField(step, "source", record->source.Value());
        AddField(step, "provenance", std::string(ProvenanceClassName(record->provenance)));
        if (record->reason.IsSet()) AddField(step, "reason", record->reason.Value());
        explanation.steps.push_back(std::move(step));
      }
    }
    if (suppressed > 8) {
      ExplanationStep step = MakeStep("evidence.non-current-truncated",
                                      "further non current evidence omitted");
      AddField(step, "omitted", std::to_string(suppressed - 8));
      explanation.steps.push_back(std::move(step));
    }
    return explanation;
  }

  void RecordJournal(const JournalEntry& entry) {
    if (journal_.size() >= options_.max_rejection_journal_entries) {
      const PublicationId oldest = journal_order_.front();
      journal_order_.pop_front();
      journal_.erase(oldest);
    }
    journal_order_.push_back(entry.publication);
    journal_[entry.publication] = entry;
    attempt_journal_[entry.attempt] = entry.publication;
  }

  void RegisterReplay(const PublisherKey& key, const ReplayRecord& record) {
    PublisherRuntime& runtime = publishers_[key];
    if (runtime.replay.size() >= options_.max_replay_entries_per_publisher) {
      runtime.replay.pop_front();
    }
    runtime.replay.push_back(record);
  }

  // --- data ---------------------------------------------------------------

  Options options_;
  mutable std::shared_mutex mutex_;
  CapabilitySchema& schema_;

  RegistryGeneration registry_generation_;
  CoordinatorEpoch epoch_;

  std::vector<AuthorityGrant> authorities_;
  std::unordered_map<AuthorityScopeId, std::size_t> authority_index_;

  std::unordered_map<PublisherKey, PublisherRuntime, PublisherKeyHash> publishers_;
  std::unordered_set<WorkerBootId> fenced_boots_;
  std::deque<FenceRecord> fences_;

  std::unordered_map<EntityId, std::uint32_t> entity_keys_;
  std::deque<EntityState> entities_;

  std::unordered_map<CapabilityId, std::uint32_t> capability_keys_;
  std::vector<CapabilityId> capability_ids_;
  std::unordered_map<std::uint32_t, ReverseEntry> reverse_;
  std::unordered_map<CapabilityNamespaceId, std::set<CapabilityId>> namespace_index_;
  std::unordered_map<PublisherId, std::set<EvidenceId>> publisher_evidence_;
  std::unordered_map<SourceId, std::set<EvidenceId>> source_evidence_;
  std::set<std::pair<EntityId, CapabilityId>> revalidation_;

  std::uint64_t arrival_sequence_ = 0;

  std::unordered_map<PublicationId, JournalEntry> journal_;
  std::unordered_map<MutationAttemptId, PublicationId> attempt_journal_;
  std::deque<PublicationId> journal_order_;

  // --- validation helpers -------------------------------------------------

  static bool NamespaceAllowed(const AuthorityGrant& grant, const CapabilityNamespaceId& ns) {
    for (const CapabilityNamespaceId& allowed : grant.namespaces) {
      if (allowed == ns) return true;
    }
    return false;
  }

  Status ValidateClaimShape(const CapabilityDescriptor& descriptor, const CapabilityClaim& claim) {
    const bool needs_value =
        descriptor.kind != ValueKind::Boolean &&
        (claim.state == CapabilityState::Supported || claim.state == CapabilityState::Unsupported);
    if (claim.value.IsAbsent()) {
      if (needs_value) {
        return Status::Failure(
            ErrorCode::ValueTypeMismatch,
            "a typed capability claim requires a value for this descriptor",
            descriptor.id.ToString() + " expects " + std::string(ValueKindName(descriptor.kind)));
      }
      return Status::Success();
    }
    if (claim.state == CapabilityState::Unknown) {
      return Status::Failure(ErrorCode::ValueContradiction,
                             "an UNKNOWN claim must not carry a value",
                             descriptor.id.ToString());
    }
    return ValidateValueAgainstDescriptor(descriptor, claim.value);
  }

  Digest RequestContentDigest(const PublicationRequest& request, const AuthorityGrant& grant) {
    std::vector<std::byte> bytes;
    bytes.reserve(256 + request.claims.size() * 64);
    ByteWriter writer(bytes);
    writer.U8(static_cast<std::uint8_t>(request.mode));
    writer.Text(request.entity.ToString(), limits::kMaxEntityIdLength);
    writer.U64(request.entity_generation.Value());
    // The expected capability set generation is a precondition, not mutation content: a
    // duplicate of the same attempt that refreshed its precondition is still the same attempt
    // and is answered from the replay cache instead of being applied again.
    writer.U8(static_cast<std::uint8_t>(request.coverage));
    writer.Text(request.authority.scope.Value(), limits::kMaxAuthorityScopeIdLength);
    writer.Text(request.authority.source.Value(), limits::kMaxSourceIdLength);
    writer.U64(request.authority.source_generation.Value());
    writer.Text(request.reason.Value(), limits::kMaxTextLength);
    writer.U8(static_cast<std::uint8_t>(grant.exclusive ? 1 : 0));
    writer.U32(static_cast<std::uint32_t>(request.claims.size()));
    for (const CapabilityClaim& claim : request.claims) {
      writer.Text(claim.capability.ToString(), limits::kMaxCapabilityIdLength);
      writer.U8(static_cast<std::uint8_t>(claim.state));
      claim.value.Encode(writer);
      writer.U8(ProvenanceRank(claim.provenance));
      writer.U8(static_cast<std::uint8_t>(claim.source_class));
      writer.U8(static_cast<std::uint8_t>(claim.durability));
      writer.U8(static_cast<std::uint8_t>(claim.coverage));
      writer.Text(claim.firmware_version.Value(), limits::kMaxVersionTokenLength);
      writer.Text(claim.driver_version.Value(), limits::kMaxVersionTokenLength);
      writer.Text(claim.reason.Value(), limits::kMaxTextLength);
    }
    writer.U32(static_cast<std::uint32_t>(request.edits.size()));
    for (const IncrementalEdit& edit : request.edits) {
      writer.U8(static_cast<std::uint8_t>(edit.operation));
      writer.Text(edit.capability.ToString(), limits::kMaxCapabilityIdLength);
      writer.U8(static_cast<std::uint8_t>(edit.claim.state));
      edit.claim.value.Encode(writer);
      writer.U8(ProvenanceRank(edit.claim.provenance));
      writer.U8(static_cast<std::uint8_t>(edit.claim.source_class));
      writer.U8(static_cast<std::uint8_t>(edit.claim.durability));
      writer.U8(static_cast<std::uint8_t>(edit.claim.coverage));
    }
    return fabric::capability::ComputeDigest(bytes);
  }

  Explanation RejectionExplanation(ErrorCode code, const std::string& message,
                                   const std::string& detail,
                                   const PublicationRequest& request) const {
    Explanation explanation;
    explanation.code = std::string("publication.rejected.") + std::string(ErrorCodeName(code));
    explanation.summary = message;
    ExplanationStep step = MakeStep("publication.request", "rejected publication request");
    AddField(step, "publication", request.publication.Value());
    AddField(step, "attempt", request.attempt.Value());
    AddField(step, "mode", std::string(PublicationModeName(request.mode)));
    AddField(step, "entity", request.entity.ToString());
    AddField(step, "entity_generation", request.entity_generation.ToString());
    AddField(step, "expected_set_generation", request.expected_set_generation.ToString());
    AddField(step, "publisher", request.authority.publisher.Value());
    AddField(step, "worker_boot", request.authority.worker_boot.Value());
    AddField(step, "scope", request.authority.scope.Value());
    AddField(step, "claims", std::to_string(request.claims.size()));
    AddField(step, "edits", std::to_string(request.edits.size()));
    explanation.steps.push_back(std::move(step));
    if (!detail.empty()) {
      ExplanationStep failure = MakeStep("publication.failure", detail);
      AddField(failure, "error_code", std::string(ErrorCodeName(code)));
      explanation.steps.push_back(std::move(failure));
    }
    return explanation;
  }

  // --- evidence mutation --------------------------------------------------

  EvidenceGeneration NextEvidenceGeneration(const CapabilityEntry& entry,
                                            const SourceKey& key) const {
    EvidenceGeneration highest;
    for (const EvidencePtr& record : entry.evidence) {
      if (record->source == key.source && record->worker_boot == key.boot) {
        if (record->evidence_generation.Value() >= highest.Value()) {
          highest = record->evidence_generation;
        }
      }
    }
    auto next = highest.Next();
    return next.HasValue() ? next.Value() : EvidenceGeneration::FromValue(UINT64_MAX);
  }

  std::size_t LiveEvidenceCount(const CapabilityEntry& entry) const {
    std::size_t count = 0;
    for (const EvidencePtr& record : entry.evidence) {
      if (IsCurrent(record->currentness) ||
          record->currentness == EvidenceCurrentness::RevalidationRequired ||
          record->currentness == EvidenceCurrentness::Fenced) {
        ++count;
      }
    }
    return count;
  }

  void TrimEvidence(CapabilityEntry& entry) {
    while (entry.evidence.size() > options_.max_evidence_per_capability) {
      std::size_t victim = entry.evidence.size();
      for (std::size_t index = 0; index < entry.evidence.size(); ++index) {
        const EvidenceCurrentness currentness = entry.evidence[index]->currentness;
        if (currentness == EvidenceCurrentness::Superseded ||
            currentness == EvidenceCurrentness::EntityGenerationSuperseded) {
          victim = index;
          break;
        }
      }
      if (victim == entry.evidence.size()) break;
      entry.evidence.erase(entry.evidence.begin() + static_cast<std::ptrdiff_t>(victim));
    }
  }

  /// Marks every live claim of one publishing identity as superseded. Used by
  /// full snapshots (the publisher replaces its own complete surface) and by
  /// an exclusive authoritative snapshot (which also supersedes other
  /// identities).
  void SupersedeIdentity(EntityState& entity, const SourceKey& source_key,
                         const ReasonToken& reason) {
    for (auto& pair : entity.capabilities) {
      CapabilityEntry& entry = pair.second;
      bool touched = false;
      for (EvidencePtr& record : entry.evidence) {
        if (record->source != source_key.source || record->worker_boot != source_key.boot) continue;
        if (record->currentness == EvidenceCurrentness::Superseded ||
            record->currentness == EvidenceCurrentness::EntityGenerationSuperseded) {
          continue;
        }
        auto copy = std::make_shared<EvidenceRecord>(*record);
        copy->currentness = EvidenceCurrentness::Superseded;
        copy->reason = reason;
        DeindexEvidence(*record);
        record = std::move(copy);
        touched = true;
      }
      if (touched) {
        const auto next = entry.generation.Next();
        if (next.HasValue()) entry.generation = next.Value();
        ResolveCapability(entity, pair.first, entry);
      }
    }
  }

  /// Marks every claim owned by any identity other than the given one as
  /// superseded. Only an exclusive authority may do this. Durable
  /// administrative declarations survive an exclusive snapshot.
  void SupersedeOtherIdentities(EntityState& entity, const PublisherKey& keep,
                                const ReasonToken& reason) {
    for (auto& pair : entity.capabilities) {
      CapabilityEntry& entry = pair.second;
      bool touched = false;
      for (EvidencePtr& record : entry.evidence) {
        if (record->publisher == keep.publisher && record->worker_boot == keep.boot) continue;
        if (record->durability == DurabilityClass::Durable) continue;
        if (record->currentness == EvidenceCurrentness::Superseded ||
            record->currentness == EvidenceCurrentness::EntityGenerationSuperseded) {
          continue;
        }
        auto copy = std::make_shared<EvidenceRecord>(*record);
        copy->currentness = EvidenceCurrentness::Superseded;
        copy->reason = reason;
        DeindexEvidence(*record);
        record = std::move(copy);
        touched = true;
      }
      if (touched) {
        const auto next = entry.generation.Next();
        if (next.HasValue()) entry.generation = next.Value();
        ResolveCapability(entity, pair.first, entry);
      }
    }
  }

  Status ApplyClaim(EntityState& entity, const CapabilityClaim& claim,
                    const PublicationRequest& request, Coverage coverage,
                    const ReasonToken& supersede_reason) {
    const std::uint32_t key = CapabilityKey(claim.capability);
    const bool is_new = entity.capabilities.find(key) == entity.capabilities.end();
    if (is_new && entity.capabilities.size() >= options_.max_capabilities_per_entity) {
      return Status::Failure(ErrorCode::ClaimCountExceeded,
                             "entity capability limit reached",
                             std::to_string(options_.max_capabilities_per_entity));
    }
    CapabilityEntry& entry = entity.capabilities[key];
    if (is_new) {
      entry.id = claim.capability;
    }

    const SourceKey source_key{request.authority.source, request.authority.worker_boot};
    const EvidenceGeneration generation = NextEvidenceGeneration(entry, source_key);

    for (EvidencePtr& record : entry.evidence) {
      if (record->source != source_key.source || record->worker_boot != source_key.boot) continue;
      if (record->currentness == EvidenceCurrentness::Superseded ||
          record->currentness == EvidenceCurrentness::EntityGenerationSuperseded) {
        continue;
      }
      auto copy = std::make_shared<EvidenceRecord>(*record);
      copy->currentness = EvidenceCurrentness::Superseded;
      copy->reason = supersede_reason;
      DeindexEvidence(*record);
      record = std::move(copy);
    }

    TrimEvidence(entry);
    if (LiveEvidenceCount(entry) >= options_.max_evidence_per_capability) {
      return Status::Failure(ErrorCode::EvidenceLimitExceeded,
                             "capability evidence limit reached",
                             claim.capability.ToString());
    }

    auto record = std::make_shared<EvidenceRecord>();
    record->capability = claim.capability;
    record->entity = entity.id;
    record->entity_generation = entity.bound_generation;
    record->source = request.authority.source;
    record->publisher = request.authority.publisher;
    record->scope = request.authority.scope;
    record->worker_boot = request.authority.worker_boot;
    record->epoch = epoch_;
    record->source_generation = request.authority.source_generation;
    record->evidence_generation = generation;
    record->attempt = request.attempt;
    record->publication = request.publication;
    record->provenance = claim.provenance;
    record->source_class = claim.source_class;
    record->durability = claim.durability;
    record->coverage = coverage;
    record->state = claim.state;
    record->value = claim.value;
    record->firmware_version = claim.firmware_version;
    record->driver_version = claim.driver_version;
    record->reason = claim.reason;
    record->currentness = EvidenceCurrentness::Current;
    record->accepted_generation = registry_generation_;
    record->arrival_sequence = ++arrival_sequence_;
    record->id = MakeEvidenceId(request.authority.publisher, request.authority.worker_boot,
                                request.attempt, entity.id, entity.bound_generation,
                                claim.capability, generation.Value());

    IndexEvidence(*record);
    entry.evidence.push_back(std::move(record));
    const auto next = entry.generation.Next();
    if (!next.HasValue()) {
      return Status::Failure(ErrorCode::ArithmeticOverflow, "capability generation exhausted",
                             claim.capability.ToString());
    }
    entry.generation = next.Value();
    ResolveCapability(entity, key, entry);
    return Status::Success();
  }

  Status WithdrawIdentityClaim(EntityState& entity, const CapabilityId& capability,
                               const PublicationRequest& request, const ReasonToken& reason) {
    const auto found = capability_keys_.find(capability);
    if (found == capability_keys_.end()) return Status::Success();
    const auto entry_it = entity.capabilities.find(found->second);
    if (entry_it == entity.capabilities.end()) return Status::Success();
    CapabilityEntry& entry = entry_it->second;
    bool touched = false;
    for (EvidencePtr& record : entry.evidence) {
      if (record->source != request.authority.source ||
          record->worker_boot != request.authority.worker_boot) {
        continue;
      }
      if (record->currentness == EvidenceCurrentness::Superseded ||
          record->currentness == EvidenceCurrentness::EntityGenerationSuperseded) {
        continue;
      }
      auto copy = std::make_shared<EvidenceRecord>(*record);
      copy->currentness = EvidenceCurrentness::Superseded;
      copy->reason = reason;
      DeindexEvidence(*record);
      record = std::move(copy);
      touched = true;
    }
    if (!touched) return Status::Success();
    const auto next = entry.generation.Next();
    if (!next.HasValue()) {
      return Status::Failure(ErrorCode::ArithmeticOverflow, "capability generation exhausted",
                             capability.ToString());
    }
    entry.generation = next.Value();
    ResolveCapability(entity, found->second, entry);
    return Status::Success();
  }

  /// Marks evidence non current without withdrawing the claim: used by worker
  /// boot fencing, source invalidation, entity invalidation and coordinator
  /// epoch advance.
  void MarkNonCurrent(EntityState& entity, CapabilityEntry& entry,
                      const EvidenceCurrentness currentness, const ReasonToken& reason,
                      const WorkerBootId* boot, const SourceId* source, bool include_durable) {
    bool touched = false;
    for (EvidencePtr& record : entry.evidence) {
      if (boot != nullptr && record->worker_boot != *boot) continue;
      if (source != nullptr && record->source != *source) continue;
      if (!include_durable && record->durability == DurabilityClass::Durable) continue;
      if (!IsCurrent(record->currentness)) continue;
      auto copy = std::make_shared<EvidenceRecord>(*record);
      copy->currentness = currentness;
      copy->reason = reason;
      DeindexEvidence(*record);
      record = std::move(copy);
      touched = true;
    }
    if (!touched) return;
    const auto next = entry.generation.Next();
    if (next.HasValue()) entry.generation = next.Value();
    ResolveCapability(entity, CapabilityKey(entry.id), entry);
  }

  // --- diffs --------------------------------------------------------------

  static CapabilityDiff BuildDiff(const EntityId& entity, const EntityCapabilitySet* before,
                                  const EntityCapabilitySet* after) {
    CapabilityDiff diff;
    diff.entity = entity;
    if (before != nullptr) {
      diff.before_entity_generation = before->entity_generation;
      diff.before_set_generation = before->set_generation;
      diff.before_digest = before->digest;
    }
    if (after != nullptr) {
      diff.after_entity_generation = after->entity_generation;
      diff.after_set_generation = after->set_generation;
      diff.after_digest = after->digest;
    }
    if (before != nullptr && after != nullptr &&
        before->entity_generation != after->entity_generation) {
      CapabilityDiffEntry entry;
      entry.kind = CapabilityDiffKind::EntityGenerationChanged;
      entry.detail = "entity generation advanced from " + before->entity_generation.ToString() +
                     " to " + after->entity_generation.ToString();
      diff.entries.push_back(std::move(entry));
    }
    if (before != nullptr && after != nullptr && before->invalidated != after->invalidated) {
      CapabilityDiffEntry entry;
      entry.kind = CapabilityDiffKind::EntityInvalidated;
      entry.detail = after->invalidated ? "entity generation invalidated"
                                        : "entity generation re-established";
      diff.entries.push_back(std::move(entry));
    }

    const auto find = [](const EntityCapabilitySet* set,
                         const CapabilityId& capability) -> const CapabilityResolution* {
      if (set == nullptr) return nullptr;
      return set->Find(capability);
    };

    std::set<CapabilityId> capabilities;
    if (before != nullptr) {
      for (const CapabilityResolution& resolution : before->capabilities) {
        capabilities.insert(resolution.capability);
      }
    }
    if (after != nullptr) {
      for (const CapabilityResolution& resolution : after->capabilities) {
        capabilities.insert(resolution.capability);
      }
    }

    for (const CapabilityId& capability : capabilities) {
      const CapabilityResolution* lhs = find(before, capability);
      const CapabilityResolution* rhs = find(after, capability);
      CapabilityDiffEntry entry;
      entry.capability = capability;
      if (lhs == nullptr && rhs != nullptr) {
        entry.kind = CapabilityDiffKind::CapabilityAdded;
        entry.after_state = rhs->state;
        entry.after_value = rhs->value;
        entry.has_after_value = rhs->has_value;
        entry.after_provenance = rhs->provenance;
        entry.after_generation = rhs->capability_generation;
        entry.detail = "capability record added";
        diff.entries.push_back(std::move(entry));
        continue;
      }
      if (lhs != nullptr && rhs == nullptr) {
        entry.kind = CapabilityDiffKind::CapabilityRemoved;
        entry.before_state = lhs->state;
        entry.before_value = lhs->value;
        entry.has_before_value = lhs->has_value;
        entry.before_provenance = lhs->provenance;
        entry.before_generation = lhs->capability_generation;
        entry.detail = "capability record removed";
        diff.entries.push_back(std::move(entry));
        continue;
      }
      if (lhs == nullptr || rhs == nullptr) continue;
      entry.before_state = lhs->state;
      entry.after_state = rhs->state;
      entry.before_value = lhs->value;
      entry.has_before_value = lhs->has_value;
      entry.after_value = rhs->value;
      entry.has_after_value = rhs->has_value;
      entry.before_provenance = lhs->provenance;
      entry.after_provenance = rhs->provenance;
      entry.before_generation = lhs->capability_generation;
      entry.after_generation = rhs->capability_generation;
      if (lhs->state != rhs->state) {
        entry.kind = CapabilityDiffKind::StateChanged;
        entry.detail = std::string(CapabilityStateName(lhs->state)) + " -> " +
                       std::string(CapabilityStateName(rhs->state));
        diff.entries.push_back(std::move(entry));
        continue;
      }
      if (lhs->has_value != rhs->has_value || !(lhs->value == rhs->value)) {
        entry.kind = CapabilityDiffKind::ValueChanged;
        entry.detail = "value changed";
        diff.entries.push_back(std::move(entry));
        continue;
      }
      if (lhs->provenance != rhs->provenance || lhs->coverage != rhs->coverage ||
          lhs->durability != rhs->durability) {
        entry.kind = CapabilityDiffKind::ProvenanceChanged;
        entry.detail = std::string(ProvenanceClassName(lhs->provenance)) + " -> " +
                       std::string(ProvenanceClassName(rhs->provenance));
        diff.entries.push_back(std::move(entry));
        continue;
      }
      if (lhs->current_evidence_count != rhs->current_evidence_count) {
        entry.kind = CapabilityDiffKind::CurrentnessChanged;
        entry.detail = "current evidence count changed";
        diff.entries.push_back(std::move(entry));
        continue;
      }
      if (lhs->evidence_generation != rhs->evidence_generation ||
          !(lhs->winning_evidence == rhs->winning_evidence)) {
        entry.kind = CapabilityDiffKind::EvidenceSuperseded;
        entry.detail = "evidence refreshed";
        diff.entries.push_back(std::move(entry));
        continue;
      }
      if (lhs->capability_generation != rhs->capability_generation) {
        entry.kind = CapabilityDiffKind::GenerationAdvanced;
        entry.detail = "capability generation advanced";
        diff.entries.push_back(std::move(entry));
      }
    }
    std::sort(diff.entries.begin(), diff.entries.end(),
              [](const CapabilityDiffEntry& lhs, const CapabilityDiffEntry& rhs) {
                if (!(lhs.capability == rhs.capability)) return lhs.capability < rhs.capability;
                return static_cast<std::uint8_t>(lhs.kind) < static_cast<std::uint8_t>(rhs.kind);
              });
    return diff;
  }


  // --- generation retirement ----------------------------------------------

  void RetireGeneration(EntityState& entity, EntityGeneration next_generation,
                        const ReasonToken& reason) {
    if (entity.has_set && entity.set != nullptr) {
      RetainedGeneration retained;
      retained.summary.generation = entity.bound_generation;
      retained.summary.set_generation = entity.set_generation;
      retained.summary.digest = entity.set->digest;
      retained.summary.capability_count = entity.set->capabilities.size();
      retained.summary.supported_count = entity.set->CountOf(CapabilityState::Supported);
      retained.summary.retired_at = registry_generation_;
      retained.summary.reason = reason;
      for (const CapabilityResolution& resolution : entity.set->capabilities) {
        if (retained.capability_ids.size() >= limits::kMaxRetainedCapabilityIds) break;
        retained.capability_ids.push_back(resolution.capability);
      }
      entity.history.push_front(std::move(retained));
      while (entity.history.size() > options_.max_entity_generations_retained) {
        entity.history.pop_back();
      }
    }
    for (auto& pair : entity.capabilities) {
      CapabilityEntry& entry = pair.second;
      for (EvidencePtr& record : entry.evidence) {
        if (!IsCurrent(record->currentness)) continue;
        DeindexEvidence(*record);
      }
    }
    entity.capabilities.clear();
    entity.source_high_water.clear();
    entity.set.reset();
    entity.has_set = false;
    entity.set_generation = CapabilitySetGeneration{};
    entity.bound_generation = next_generation;
    entity.current_generation = next_generation;
    entity.invalidated = false;
    entity.invalidation_reason = ReasonToken{};
  }

  // --- publication --------------------------------------------------------

  Outcome<PublicationResult> Publish(const PublicationRequest& request) {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    PublicationResult result;
    result.publication = request.publication;
    result.attempt = request.attempt;
    result.entity = request.entity;
    result.entity_generation = request.entity_generation;
    result.registry_generation = registry_generation_;

    const auto reject = [&](ErrorCode code, std::string message,
                            std::string detail) -> Outcome<PublicationResult> {
      result.status = PublicationStatus::Rejected;
      result.code = code;
      result.message = std::move(message);
      result.explanation = RejectionExplanation(code, result.message, detail, request);
      result.registry_generation = registry_generation_;
      if (request.publication.IsSet() && request.attempt.IsSet()) {
        JournalEntry journal;
        journal.publication = request.publication;
        journal.attempt = request.attempt;
        journal.entity = request.entity;
        journal.entity_generation = request.entity_generation;
        journal.status = PublicationStatus::Rejected;
        journal.code = code;
        journal.message = result.message;
        journal.registry_generation = registry_generation_;
        journal.explanation = result.explanation;
        RecordJournal(journal);
      }
      return result;
    };

    // 1. request shape
    if (!request.publication.IsSet()) {
      return reject(ErrorCode::InvalidArgument, "publication identifier is required", {});
    }
    if (!request.attempt.IsSet()) {
      return reject(ErrorCode::InvalidArgument, "mutation attempt identifier is required", {});
    }
    if (!request.entity.IsSet()) {
      return reject(ErrorCode::InvalidArgument, "entity identifier is required", {});
    }
    if (!request.entity_generation.IsSet()) {
      return reject(ErrorCode::InvalidArgument, "entity generation is required", {});
    }
    if (!request.authority.publisher.IsSet() || !request.authority.worker_boot.IsSet() ||
        !request.authority.scope.IsSet() || !request.authority.source.IsSet()) {
      return reject(ErrorCode::InvalidArgument,
                    "publisher, worker boot, authority scope and source are required", {});
    }
    if (request.mode == PublicationMode::Incremental && !request.claims.empty()) {
      return reject(ErrorCode::InvalidArgument,
                    "an incremental publication carries edits, not claims", {});
    }
    if (request.mode != PublicationMode::Incremental && !request.edits.empty()) {
      return reject(ErrorCode::InvalidArgument,
                    "a snapshot publication carries claims, not edits", {});
    }
    if (request.mode == PublicationMode::PartialObservation && request.claims.empty()) {
      return reject(ErrorCode::EmptyCollection,
                    "a partial observation must contain at least one claim", {});
    }
    if (request.mode == PublicationMode::Incremental && request.edits.empty()) {
      return reject(ErrorCode::EmptyCollection,
                    "an incremental publication must contain at least one edit", {});
    }
    if (request.claims.size() > limits::kMaxClaimsPerPublication) {
      return reject(ErrorCode::TooManyItems, "publication exceeds the claim bound",
                    "claims=" + std::to_string(request.claims.size()));
    }
    if (request.edits.size() > limits::kMaxClaimsPerPublication) {
      return reject(ErrorCode::TooManyItems, "publication exceeds the edit bound",
                    "edits=" + std::to_string(request.edits.size()));
    }

    // 2. coordinator epoch
    if (!request.epoch.IsSet() || request.epoch.Value() < epoch_.Value()) {
      return reject(ErrorCode::CoordinatorEpochStale,
                    "the publication carries a stale coordinator epoch",
                    "request=" + request.epoch.ToString() + " current=" + epoch_.ToString());
    }
    if (request.epoch.Value() > epoch_.Value()) {
      return reject(ErrorCode::CoordinatorEpochUnknown,
                    "the publication carries an unknown coordinator epoch",
                    "request=" + request.epoch.ToString() + " current=" + epoch_.ToString());
    }

    // 3. worker boot fencing and publisher registration
    if (fenced_boots_.find(request.authority.worker_boot) != fenced_boots_.end()) {
      return reject(ErrorCode::WorkerBootFenced,
                    "the worker boot identity is permanently fenced",
                    "worker_boot=" + request.authority.worker_boot.Value());
    }
    const PublisherKey publisher_key{request.authority.publisher, request.authority.worker_boot};
    const auto publisher_it = publishers_.find(publisher_key);
    if (publisher_it == publishers_.end()) {
      return reject(ErrorCode::UnauthorizedPublisher,
                    "the publishing instance is not registered with this coordinator",
                    "publisher=" + request.authority.publisher.Value());
    }
    PublisherRuntime& runtime = publisher_it->second;
    if (!runtime.active) {
      return reject(ErrorCode::WorkerBootStale,
                    "the publishing instance registration is no longer active", {});
    }
    if (!(runtime.registration.scope == request.authority.scope)) {
      return reject(ErrorCode::AuthorityScopeViolation,
                    "the publication scope does not match the registered scope",
                    "registered=" + runtime.registration.scope.Value() + " request=" +
                        request.authority.scope.Value());
    }
    if (!(runtime.registration.epoch == epoch_)) {
      return reject(ErrorCode::CoordinatorEpochStale,
                    "the publishing instance was registered under an older coordinator epoch", {});
    }

    // 4. authority grant
    const auto grant_it = authority_index_.find(request.authority.scope);
    if (grant_it == authority_index_.end()) {
      return reject(ErrorCode::UnknownAuthorityScope, "the authority scope is not declared",
                    request.authority.scope.Value());
    }
    const AuthorityGrant& grant = authorities_[grant_it->second];
    if (!AllowsMode(grant.modes, request.mode)) {
      return reject(ErrorCode::PublicationModeNotAuthorized,
                    "the authority scope may not publish in this mode",
                    "mode=" + std::string(PublicationModeName(request.mode)));
    }
    if (std::find(grant.entity_kinds.begin(), grant.entity_kinds.end(), request.entity.Kind()) ==
        grant.entity_kinds.end()) {
      return reject(ErrorCode::AuthorityScopeViolation,
                    "the authority scope may not publish for this entity class",
                    std::string(FabricEntityKindName(request.entity.Kind())));
    }
    if (request.claims.size() > grant.max_claims_per_publication) {
      return reject(ErrorCode::ClaimCountExceeded,
                    "the publication exceeds the claim bound of the authority scope",
                    std::to_string(grant.max_claims_per_publication));
    }

    // 5. per claim authority and schema validation (no mutation yet)
    std::set<CapabilityId> seen;
    const auto check_claim = [&](const CapabilityClaim& claim, bool require_capability) -> Status {
      if (!claim.capability.IsSet()) {
        return Status::Failure(ErrorCode::InvalidArgument,
                               "claim capability identifier is required");
      }
      if (!NamespaceAllowed(grant, claim.capability.Namespace())) {
        return Status::Failure(ErrorCode::AuthorityScopeViolation,
                               "the authority scope does not cover this capability namespace",
                               claim.capability.Namespace().Value());
      }
      if (require_capability && !seen.insert(claim.capability).second) {
        return Status::Failure(ErrorCode::DuplicateClaim,
                               "the publication declares one capability more than once",
                               claim.capability.ToString());
      }
      const auto descriptor = schema_.Find(claim.capability);
      if (!descriptor) return descriptor.GetError();
      if (ProvenanceRank(claim.provenance) < ProvenanceRank(grant.strongest_provenance)) {
        return Status::Failure(
            ErrorCode::ProvenanceNotAuthorized,
            "the authority scope may not claim provenance stronger than it holds",
            std::string(ProvenanceClassName(claim.provenance)));
      }
      if (ProvenanceRank(claim.provenance) <
          ProvenanceRank(StrongestProvenanceForSource(claim.source_class))) {
        return Status::Failure(
            ErrorCode::ProvenanceNotAuthorized,
            "the declared evidence source class cannot support the declared provenance",
            std::string(EvidenceSourceClassName(claim.source_class)) + " -> " +
                std::string(ProvenanceClassName(claim.provenance)));
      }
      if (claim.durability == DurabilityClass::Durable) {
        if (!grant.may_publish_durable) {
          return Status::Failure(ErrorCode::DurablePublicationNotAuthorized,
                                 "the authority scope may not publish durable declarations");
        }
        if (IsLiveProcessObservation(claim.provenance)) {
          return Status::Failure(
              ErrorCode::DurablePublicationNotAuthorized,
              "a live hardware, driver, operating system or agent observation may never be "
              "declared durable",
              std::string(ProvenanceClassName(claim.provenance)));
        }
      }
      return ValidateClaimShape(*descriptor.Value(), claim);
    };
    for (const CapabilityClaim& claim : request.claims) {
      auto valid = check_claim(claim, true);
      if (!valid) return reject(valid.Code(), valid.GetError().message, valid.GetError().detail);
    }
    for (const IncrementalEdit& edit : request.edits) {
      if (!edit.capability.IsSet()) {
        return reject(ErrorCode::InvalidArgument, "edit capability identifier is required", {});
      }
      if (!NamespaceAllowed(grant, edit.capability.Namespace())) {
        return reject(ErrorCode::AuthorityScopeViolation,
                      "the authority scope does not cover this capability namespace",
                      edit.capability.Namespace().Value());
      }
      const auto descriptor = schema_.Find(edit.capability);
      if (!descriptor) {
        return reject(descriptor.GetError().code, descriptor.GetError().message,
                      descriptor.GetError().detail);
      }
      if (edit.operation == IncrementalOperation::WithdrawClaim ||
          edit.operation == IncrementalOperation::SupersedeSourceEvidence) {
        continue;
      }
      CapabilityClaim claim = edit.claim;
      claim.capability = edit.capability;
      switch (edit.operation) {
        case IncrementalOperation::MarkUnsupported:
          claim.state = CapabilityState::Unsupported;
          break;
        case IncrementalOperation::MarkUnknown:
          claim.state = CapabilityState::Unknown;
          break;
        case IncrementalOperation::MarkRevalidationRequired:
          claim.state = CapabilityState::RevalidationRequired;
          break;
        default:
          break;
      }
      if (ProvenanceRank(claim.provenance) < ProvenanceRank(grant.strongest_provenance) ||
          ProvenanceRank(claim.provenance) <
              ProvenanceRank(StrongestProvenanceForSource(claim.source_class))) {
        return reject(ErrorCode::ProvenanceNotAuthorized,
                      "the declared provenance is not authorized for this scope or source",
                      std::string(ProvenanceClassName(claim.provenance)));
      }
      if (claim.durability == DurabilityClass::Durable) {
        if (!grant.may_publish_durable || IsLiveProcessObservation(claim.provenance)) {
          return reject(ErrorCode::DurablePublicationNotAuthorized,
                        "a durable declaration requires an authorized durable scope and a "
                        "provenance that is not a live process observation",
                        std::string(ProvenanceClassName(claim.provenance)));
        }
      }
      auto valid = ValidateClaimShape(*descriptor.Value(), claim);
      if (!valid) return reject(valid.Code(), valid.GetError().message, valid.GetError().detail);
    }

    // 6. entity identity and generation
    EntityState* entity = FindEntity(request.entity);
    if (entity == nullptr) {
      if (entities_.size() >= options_.max_entities) {
        return Outcome<PublicationResult>::Failure(ErrorCode::TooManyItems,
                                                   "entity limit reached",
                                                   std::to_string(options_.max_entities));
      }
      entity_keys_.emplace(request.entity, static_cast<std::uint32_t>(entities_.size()));
      entities_.emplace_back();
      entity = &entities_.back();
      entity->id = request.entity;
      entity->current_generation = request.entity_generation;
      entity->bound_generation = request.entity_generation;
      entity->touched_at = registry_generation_;
    }
    if (request.entity_generation.Value() < entity->current_generation.Value()) {
      return reject(ErrorCode::EntityGenerationStale,
                    "the publication targets a superseded entity generation",
                    "request=" + request.entity_generation.ToString() + " current=" +
                        entity->current_generation.ToString());
    }
    if (request.entity_generation.Value() > entity->current_generation.Value()) {
      RetireGeneration(*entity, request.entity_generation,
                       MakeReason("superseded-by-fabric-registry"));
      const auto advanced = registry_generation_.Next();
      if (!advanced.HasValue()) {
        return Outcome<PublicationResult>::Failure(ErrorCode::ArithmeticOverflow,
                                                   "registry generation exhausted");
      }
      registry_generation_ = advanced.Value();
      entity->touched_at = registry_generation_;
    }


    // 7. duplicate attempt classification: an exact replay is answered from the replay
    // cache and an already superseded attempt is rejected as a stale replay, both before any
    // precondition check, so a replay is never misreported as a precondition failure.
    const std::string content_digest = DigestHex(RequestContentDigest(request, grant));
    if (!runtime.replay.empty()) {
      const ReplayRecord& latest = runtime.replay.back();
      if (latest.attempt == request.attempt) {
        if (latest.content_digest != content_digest) {
          return reject(ErrorCode::DuplicateAttemptConflict,
                        "the mutation attempt identifier was reused with different content",
                        "attempt=" + request.attempt.Value());
        }
        result.status = PublicationStatus::IdempotentReplay;
        result.code = ErrorCode::Ok;
        result.message = "exact replay of an already committed attempt";
        result.previous_set_generation = entity->set_generation;
        result.new_set_generation = entity->set_generation;
        result.registry_generation = registry_generation_;
        result.set_digest = entity->set != nullptr ? entity->set->digest : Digest{};
        result.explanation.code = "publication.idempotent-replay";
        result.explanation.summary = "the attempt was already applied; no generation advanced";
        ExplanationStep step = MakeStep("publication.replay", "exact idempotent replay detected");
        AddField(step, "attempt", request.attempt.Value());
        AddField(step, "publication", latest.publication.Value());
        AddField(step, "content_digest", content_digest);
        AddField(step, "set_generation", entity->set_generation.ToString());
        result.explanation.steps.push_back(std::move(step));
        JournalEntry journal;
        journal.publication = request.publication;
        journal.attempt = request.attempt;
        journal.entity = request.entity;
        journal.entity_generation = request.entity_generation;
        journal.status = PublicationStatus::IdempotentReplay;
        journal.code = ErrorCode::Ok;
        journal.message = result.message;
        journal.previous_generation = entity->set_generation;
        journal.new_generation = entity->set_generation;
        journal.registry_generation = registry_generation_;
        journal.explanation = result.explanation;
        RecordJournal(journal);
        return result;
      }
      for (const ReplayRecord& record : runtime.replay) {
        if (record.attempt == request.attempt) {
          return reject(ErrorCode::StaleReplay,
                        "the mutation attempt was already accepted and later superseded",
                        "attempt=" + request.attempt.Value() + " superseded_by=" +
                            latest.attempt.Value());
        }
      }
    }

    // 8. source generation must not go backwards for this publishing identity
    const SourceKey source_key{request.authority.source, request.authority.worker_boot};
    {
      const auto floor = entity->source_high_water.find(source_key);
      if (floor != entity->source_high_water.end() &&
          request.authority.source_generation.Value() < floor->second.Value()) {
        return reject(ErrorCode::SourceGenerationStale,
                      "the publication carries a stale source generation",
                      "request=" + request.authority.source_generation.ToString() + " floor=" +
                          floor->second.ToString());
      }
    }

    // 9. expected capability set generation
    if (entity->has_set) {
      if (request.expected_set_generation.Value() < entity->set_generation.Value()) {
        return reject(ErrorCode::CapabilitySetGenerationStale,
                      "the publication expects a stale capability set generation",
                      "expected=" + request.expected_set_generation.ToString() + " current=" +
                          entity->set_generation.ToString());
      }
      if (request.expected_set_generation.Value() > entity->set_generation.Value()) {
        return reject(ErrorCode::CapabilitySetGenerationMismatch,
                      "the publication expects a capability set generation that does not exist",
                      "expected=" + request.expected_set_generation.ToString() + " current=" +
                          entity->set_generation.ToString());
      }
    } else if (request.expected_set_generation.Value() != 0) {
      return reject(ErrorCode::CapabilitySetGenerationMismatch,
                    "no capability set exists for this entity generation yet",
                    "expected=" + request.expected_set_generation.ToString());
    }

    // 10. apply
    const std::shared_ptr<const EntityCapabilitySet> before_set = entity->set;
    const ReasonToken supersede_reason = request.reason.IsSet() ? request.reason : ReasonToken{};
    std::size_t applied = 0;
    std::size_t withdrawn = 0;

    if (request.mode == PublicationMode::FullSnapshot) {
      if (grant.exclusive) {
        SupersedeOtherIdentities(*entity, publisher_key, supersede_reason);
      }
      SupersedeIdentity(*entity, source_key, supersede_reason);
      for (const CapabilityClaim& claim : request.claims) {
        auto status =
            ApplyClaim(*entity, claim, request, request.coverage, supersede_reason);
        if (!status) {
          return reject(status.Code(), status.GetError().message, status.GetError().detail);
        }
        ++applied;
      }
      if (grant.exclusive && request.coverage == Coverage::FullEnumeration) {
        std::set<CapabilityId> mentioned;
        for (const CapabilityClaim& claim : request.claims) mentioned.insert(claim.capability);
        for (const CapabilityDescriptor& descriptor : schema_.Descriptors()) {
          if (!descriptor.absence_is_negative || !descriptor.id.IsSet()) continue;
          if (mentioned.find(descriptor.id) != mentioned.end()) continue;
          if (!NamespaceAllowed(grant, descriptor.id.Namespace())) continue;
          CapabilityClaim absence;
          absence.capability = descriptor.id;
          absence.state = CapabilityState::Unsupported;
          absence.provenance = request.claims.empty() ? grant.strongest_provenance
                                                      : request.claims.front().provenance;
          absence.source_class = request.claims.empty() ? EvidenceSourceClass::HardwareEnumeration
                                                        : request.claims.front().source_class;
          absence.coverage = Coverage::FullEnumeration;
          absence.reason = supersede_reason;
          auto status = ApplyClaim(*entity, absence, request, Coverage::FullEnumeration,
                                   supersede_reason);
          if (!status) {
            return reject(status.Code(), status.GetError().message, status.GetError().detail);
          }
          ++applied;
        }
      }
    } else if (request.mode == PublicationMode::PartialObservation) {
      for (const CapabilityClaim& claim : request.claims) {
        auto status =
            ApplyClaim(*entity, claim, request, request.coverage, supersede_reason);
        if (!status) {
          return reject(status.Code(), status.GetError().message, status.GetError().detail);
        }
        ++applied;
      }
    } else {
      bool superseded_source = false;
      for (const IncrementalEdit& edit : request.edits) {
        switch (edit.operation) {
          case IncrementalOperation::WithdrawClaim: {
            auto status =
                WithdrawIdentityClaim(*entity, edit.capability, request, supersede_reason);
            if (!status) {
              return reject(status.Code(), status.GetError().message, status.GetError().detail);
            }
            ++withdrawn;
            break;
          }
          case IncrementalOperation::SupersedeSourceEvidence: {
            if (!superseded_source) {
              SupersedeIdentity(*entity, source_key, supersede_reason);
              superseded_source = true;
            }
            ++withdrawn;
            break;
          }
          default: {
            CapabilityClaim claim = edit.claim;
            claim.capability = edit.capability;
            switch (edit.operation) {
              case IncrementalOperation::MarkUnsupported:
                claim.state = CapabilityState::Unsupported;
                break;
              case IncrementalOperation::MarkUnknown:
                claim.state = CapabilityState::Unknown;
                break;
              case IncrementalOperation::MarkRevalidationRequired:
                claim.state = CapabilityState::RevalidationRequired;
                break;
              default:
                break;
            }
            auto status =
                ApplyClaim(*entity, claim, request, claim.coverage, supersede_reason);
            if (!status) {
              return reject(status.Code(), status.GetError().message, status.GetError().detail);
            }
            ++applied;
            break;
          }
        }
      }
    }

    // 11. commit: one capability set generation per semantic change
    const Digest previous_digest = before_set != nullptr ? before_set->digest : Digest{};
    const bool had_set = before_set != nullptr;
    auto provisional = Materialize(*entity);
    const bool semantic_change = !had_set || !(provisional->digest == previous_digest);
    if (semantic_change) {
      const auto next = entity->set_generation.Next();
      if (!next.HasValue()) {
        return Outcome<PublicationResult>::Failure(ErrorCode::ArithmeticOverflow,
                                                   "capability set generation exhausted");
      }
      entity->set_generation = next.Value();
      const auto registry_next = registry_generation_.Next();
      if (!registry_next.HasValue()) {
        return Outcome<PublicationResult>::Failure(ErrorCode::ArithmeticOverflow,
                                                   "registry generation exhausted");
      }
      registry_generation_ = registry_next.Value();
    }
    entity->has_set = true;
    entity->bound_generation = entity->current_generation;
    entity->invalidated = false;
    entity->invalidation_reason = ReasonToken{};
    entity->set = Materialize(*entity);
    entity->touched_at = registry_generation_;

    result.status = PublicationStatus::Committed;
    result.code = ErrorCode::Ok;
    result.message = semantic_change ? "committed" : "committed without a semantic change";
    result.previous_set_generation =
        before_set != nullptr ? before_set->set_generation : CapabilitySetGeneration{};
    result.new_set_generation = entity->set_generation;
    result.registry_generation = registry_generation_;
    result.claims_applied = applied;
    result.claims_withdrawn = withdrawn;
    result.set_digest = entity->set->digest;
    result.diff = BuildDiff(entity->id, before_set.get(), entity->set.get());
    for (const CapabilityDiffEntry& entry : result.diff.entries) {
      switch (entry.kind) {
        case CapabilityDiffKind::CapabilityAdded:
          ++result.capabilities_added;
          break;
        case CapabilityDiffKind::CapabilityRemoved:
          ++result.capabilities_removed;
          break;
        default:
          ++result.capabilities_changed;
          break;
      }
    }
    result.explanation.code = semantic_change ? "publication.committed"
                                              : "publication.committed-no-semantic-change";
    result.explanation.summary = semantic_change
                                     ? "capability set generation advanced once"
                                     : "the publication produced the same resolved capability set";
    {
      ExplanationStep step = MakeStep("publication.commit", "committed atomically");
      AddField(step, "mode", std::string(PublicationModeName(request.mode)));
      AddField(step, "claims_applied", std::to_string(applied));
      AddField(step, "claims_withdrawn", std::to_string(withdrawn));
      AddField(step, "previous_set_generation", result.previous_set_generation.ToString());
      AddField(step, "set_generation", result.new_set_generation.ToString());
      AddField(step, "registry_generation", registry_generation_.ToString());
      AddField(step, "set_digest", result.set_digest.ToString());
      AddField(step, "semantic_change", semantic_change ? "true" : "false");
      result.explanation.steps.push_back(std::move(step));
    }

    ReplayRecord replay;
    replay.attempt = request.attempt;
    replay.publication = request.publication;
    replay.content_digest = content_digest;
    replay.status = PublicationStatus::Committed;
    RegisterReplay(publisher_key, replay);

    SourceGeneration& high_water = entity->source_high_water[source_key];
    if (request.authority.source_generation.Value() > high_water.Value()) {
      high_water = request.authority.source_generation;
    }

    JournalEntry journal;
    journal.publication = request.publication;
    journal.attempt = request.attempt;
    journal.entity = request.entity;
    journal.entity_generation = request.entity_generation;
    journal.status = PublicationStatus::Committed;
    journal.code = ErrorCode::Ok;
    journal.message = result.message;
    journal.previous_generation = result.previous_set_generation;
    journal.new_generation = result.new_set_generation;
    journal.registry_generation = registry_generation_;
    journal.explanation = result.explanation;
    RecordJournal(journal);
    return result;
  }
};  // class CapabilityRegistry::Impl

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

CapabilityRegistry::CapabilityRegistry(Options options)
    : impl_(std::make_unique<Impl>(std::move(options), schema_)) {}

CapabilityRegistry::~CapabilityRegistry() = default;

Outcome<void> CapabilityRegistry::RegisterVendorDescriptor(CapabilityDescriptor descriptor) {
  return schema_.RegisterVendorDescriptor(std::move(descriptor));
}

CoordinatorEpoch CapabilityRegistry::CurrentEpoch() const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex_);
  return impl_->epoch_;
}

RegistryGeneration CapabilityRegistry::Generation() const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex_);
  return impl_->registry_generation_;
}

Outcome<CoordinatorEpoch> CapabilityRegistry::AdvanceCoordinatorEpoch(ReasonToken reason) {
  Impl& impl = *impl_;
  std::unique_lock<std::shared_mutex> lock(impl.mutex_);
  auto next = impl.epoch_.Next();
  if (!next.HasValue()) {
    return Outcome<CoordinatorEpoch>::Failure(ErrorCode::ArithmeticOverflow,
                                              "coordinator epoch exhausted");
  }
  impl.epoch_ = next.Value();
  const ReasonToken why = reason.IsSet() ? reason : MakeReason("coordinator-epoch-advanced");

  std::vector<WorkerBootId> boots;
  boots.reserve(impl.publishers_.size());
  for (const auto& entry : impl.publishers_) boots.push_back(entry.first.boot);
  impl.publishers_.clear();

  for (WorkerBootId& boot : boots) {
    if (impl.fenced_boots_.find(boot) != impl.fenced_boots_.end()) continue;
    if (impl.fences_.size() >= impl.options_.max_fenced_worker_boots) break;
    impl.fenced_boots_.insert(boot);
    FenceRecord record;
    record.worker_boot = boot;
    record.reason = why;
    record.fenced_at = impl.registry_generation_;
    impl.fences_.push_back(record);
  }

  for (EntityState& entity : impl.entities_) {
    for (auto& pair : entity.capabilities) {
      impl.MarkNonCurrent(entity, pair.second, EvidenceCurrentness::RevalidationRequired, why,
                          nullptr, nullptr, false);
    }
    impl.RefreshEntity(entity);
  }
  auto registry_next = impl.registry_generation_.Next();
  if (!registry_next.HasValue()) {
    return Outcome<CoordinatorEpoch>::Failure(ErrorCode::ArithmeticOverflow,
                                              "registry generation exhausted");
  }
  impl.registry_generation_ = registry_next.Value();
  return impl.epoch_;
}

Outcome<void> CapabilityRegistry::DeclareAuthority(const AuthorityGrant& grant) {
  Impl& impl = *impl_;
  std::unique_lock<std::shared_mutex> lock(impl.mutex_);
  if (!grant.scope.IsSet()) {
    return Status::Failure(ErrorCode::InvalidArgument, "authority scope identifier is required");
  }
  if (grant.entity_kinds.empty()) {
    return Status::Failure(ErrorCode::InvalidArgument,
                           "an authority scope must name at least one entity class",
                           grant.scope.Value());
  }
  if (grant.entity_kinds.size() > limits::kMaxAuthorityEntityKinds) {
    return Status::Failure(ErrorCode::TooManyItems,
                           "too many entity classes in the authority scope", grant.scope.Value());
  }
  if (grant.namespaces.empty()) {
    return Status::Failure(ErrorCode::InvalidArgument,
                           "an authority scope must name at least one capability namespace; there "
                           "is no wildcard authority",
                           grant.scope.Value());
  }
  if (grant.namespaces.size() > limits::kMaxAuthorityNamespaces) {
    return Status::Failure(ErrorCode::TooManyItems,
                           "too many capability namespaces in the authority scope",
                           grant.scope.Value());
  }
  if (grant.modes == 0) {
    return Status::Failure(ErrorCode::InvalidArgument,
                           "an authority scope must allow at least one publication mode",
                           grant.scope.Value());
  }
  if (grant.max_claims_per_publication == 0 ||
      grant.max_claims_per_publication > limits::kMaxClaimsPerPublication) {
    return Status::Failure(ErrorCode::InvalidArgument, "authority claim bound is out of range",
                           grant.scope.Value());
  }
  for (const CapabilityNamespaceId& ns : grant.namespaces) {
    if (!ns.IsSet()) {
      return Status::Failure(ErrorCode::UnknownNamespace,
                             "authority scope names an empty capability namespace",
                             grant.scope.Value());
    }
  }
  if (grant.allowed_publishers.size() > 4096) {
    return Status::Failure(ErrorCode::TooManyItems, "authority publisher allowlist is too large",
                           grant.scope.Value());
  }
  const auto existing = impl.authority_index_.find(grant.scope);
  if (existing != impl.authority_index_.end()) {
    impl.authorities_[existing->second] = grant;
    return Status::Success();
  }
  if (impl.authorities_.size() >= impl.options_.max_authority_scopes) {
    return Status::Failure(ErrorCode::TooManyItems, "authority scope limit reached");
  }
  impl.authority_index_.emplace(grant.scope, impl.authorities_.size());
  impl.authorities_.push_back(grant);
  return Status::Success();
}

Outcome<AuthorityGrant> CapabilityRegistry::Authority(const AuthorityScopeId& scope) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex_);
  const auto found = impl_->authority_index_.find(scope);
  if (found == impl_->authority_index_.end()) {
    return Outcome<AuthorityGrant>::Failure(ErrorCode::UnknownAuthorityScope,
                                            "authority scope is not declared", scope.Value());
  }
  return impl_->authorities_[found->second];
}

std::vector<AuthorityGrant> CapabilityRegistry::Authorities() const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex_);
  std::vector<AuthorityGrant> out = impl_->authorities_;
  std::sort(out.begin(), out.end(), [](const AuthorityGrant& lhs, const AuthorityGrant& rhs) {
    return lhs.scope < rhs.scope;
  });
  return out;
}

Outcome<PublisherRegistration> CapabilityRegistry::RegisterPublisher(
    const PublisherId& publisher, const AuthorityScopeId& scope, const WorkerBootId& worker_boot) {
  Impl& impl = *impl_;
  std::unique_lock<std::shared_mutex> lock(impl.mutex_);
  if (!publisher.IsSet() || !scope.IsSet() || !worker_boot.IsSet()) {
    return Outcome<PublisherRegistration>::Failure(
        ErrorCode::InvalidArgument, "publisher, scope and worker boot identifiers are required");
  }
  if (impl.fenced_boots_.find(worker_boot) != impl.fenced_boots_.end()) {
    return Outcome<PublisherRegistration>::Failure(
        ErrorCode::WorkerBootFenced, "the worker boot identity is permanently fenced",
        worker_boot.Value());
  }
  const auto grant_it = impl.authority_index_.find(scope);
  if (grant_it == impl.authority_index_.end()) {
    return Outcome<PublisherRegistration>::Failure(ErrorCode::UnknownAuthorityScope,
                                                   "authority scope is not declared", scope.Value());
  }
  const AuthorityGrant& grant = impl.authorities_[grant_it->second];
  if (!grant.allowed_publishers.empty() &&
      std::find(grant.allowed_publishers.begin(), grant.allowed_publishers.end(), publisher) ==
          grant.allowed_publishers.end()) {
    return Outcome<PublisherRegistration>::Failure(
        ErrorCode::UnauthorizedPublisher,
        "the publisher is not on the allowlist of this authority scope", publisher.Value());
  }
  const PublisherKey key{publisher, worker_boot};
  const auto existing = impl.publishers_.find(key);
  if (existing == impl.publishers_.end() &&
      impl.publishers_.size() >= impl.options_.max_publishers) {
    return Outcome<PublisherRegistration>::Failure(ErrorCode::TooManyItems,
                                                   "publisher registration limit reached");
  }
  PublisherRuntime& runtime = impl.publishers_[key];
  runtime.registration.publisher = publisher;
  runtime.registration.scope = scope;
  runtime.registration.worker_boot = worker_boot;
  runtime.registration.epoch = impl.epoch_;
  runtime.registration.accepted_generation = impl.registry_generation_;
  runtime.active = true;
  runtime.replay.clear();
  return runtime.registration;
}

Outcome<void> CapabilityRegistry::RetirePublisher(const PublisherId& publisher,
                                                  const WorkerBootId& worker_boot) {
  Impl& impl = *impl_;
  std::unique_lock<std::shared_mutex> lock(impl.mutex_);
  const auto found = impl.publishers_.find(PublisherKey{publisher, worker_boot});
  if (found == impl.publishers_.end()) {
    return Status::Failure(ErrorCode::UnknownPublisher, "publisher instance is not registered",
                           publisher.Value());
  }
  found->second.active = false;
  return Status::Success();
}

Outcome<void> CapabilityRegistry::FenceWorkerBoot(const WorkerBootId& worker_boot,
                                                  ReasonToken reason) {
  Impl& impl = *impl_;
  std::unique_lock<std::shared_mutex> lock(impl.mutex_);
  if (!worker_boot.IsSet()) {
    return Status::Failure(ErrorCode::InvalidArgument, "worker boot identifier is required");
  }
  if (impl.fenced_boots_.find(worker_boot) != impl.fenced_boots_.end()) {
    return Status::Success();
  }
  if (impl.fences_.size() >= impl.options_.max_fenced_worker_boots) {
    bool evicted = false;
    for (auto it = impl.fences_.begin(); it != impl.fences_.end(); ++it) {
      bool owns_evidence = false;
      for (const EntityState& entity : impl.entities_) {
        for (const auto& pair : entity.capabilities) {
          for (const EvidencePtr& record : pair.second.evidence) {
            if (record->worker_boot == it->worker_boot && IsCurrent(record->currentness)) {
              owns_evidence = true;
              break;
            }
          }
          if (owns_evidence) break;
        }
        if (owns_evidence) break;
      }
      if (!owns_evidence) {
        impl.fenced_boots_.erase(it->worker_boot);
        impl.fences_.erase(it);
        evicted = true;
        break;
      }
    }
    if (!evicted) {
      return Status::Failure(
          ErrorCode::TooManyItems,
          "worker boot fence retention is at capacity and no evictable fence remains; refusing to "
          "forget a fence that still protects live evidence",
          std::to_string(impl.options_.max_fenced_worker_boots));
    }
  }
  impl.fenced_boots_.insert(worker_boot);
  const ReasonToken why = reason.IsSet() ? reason : MakeReason("worker-boot-fenced");
  FenceRecord record;
  record.worker_boot = worker_boot;
  record.reason = why;
  record.fenced_at = impl.registry_generation_;
  for (const auto& entry : impl.publishers_) {
    if (entry.first.boot == worker_boot) record.publisher = entry.first.publisher;
  }
  impl.fences_.push_back(record);
  impl.publishers_.erase(PublisherKey{record.publisher, worker_boot});

  for (EntityState& entity : impl.entities_) {
    for (auto& pair : entity.capabilities) {
      impl.MarkNonCurrent(entity, pair.second, EvidenceCurrentness::Fenced, why, &worker_boot,
                          nullptr, false);
    }
    impl.RefreshEntity(entity);
  }
  auto next = impl.registry_generation_.Next();
  if (!next.HasValue()) {
    return Status::Failure(ErrorCode::ArithmeticOverflow, "registry generation exhausted");
  }
  impl.registry_generation_ = next.Value();
  return Status::Success();
}

bool CapabilityRegistry::IsWorkerBootFenced(const WorkerBootId& worker_boot) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex_);
  return impl_->fenced_boots_.find(worker_boot) != impl_->fenced_boots_.end();
}

std::vector<FenceRecord> CapabilityRegistry::Fences() const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex_);
  std::vector<FenceRecord> out(impl_->fences_.begin(), impl_->fences_.end());
  std::sort(out.begin(), out.end(), [](const FenceRecord& lhs, const FenceRecord& rhs) {
    return lhs.worker_boot < rhs.worker_boot;
  });
  return out;
}

std::optional<PublisherRegistration> CapabilityRegistry::FindPublisher(
    const PublisherId& publisher, const WorkerBootId& worker_boot) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex_);
  const auto found = impl_->publishers_.find(PublisherKey{publisher, worker_boot});
  if (found == impl_->publishers_.end() || !found->second.active) return std::nullopt;
  return found->second.registration;
}

Outcome<EntityGeneration> CapabilityRegistry::ObserveEntityGeneration(const EntityId& entity,
                                                                     EntityGeneration generation,
                                                                     ReasonToken reason) {
  Impl& impl = *impl_;
  std::unique_lock<std::shared_mutex> lock(impl.mutex_);
  if (!entity.IsSet() || !generation.IsSet()) {
    return Outcome<EntityGeneration>::Failure(ErrorCode::InvalidArgument,
                                              "entity and entity generation are required");
  }
  EntityState* record = impl.FindEntity(entity);
  if (record == nullptr) {
    if (impl.entities_.size() >= impl.options_.max_entities) {
      return Outcome<EntityGeneration>::Failure(ErrorCode::TooManyItems, "entity limit reached");
    }
    impl.entity_keys_.emplace(entity, static_cast<std::uint32_t>(impl.entities_.size()));
    impl.entities_.emplace_back();
    record = &impl.entities_.back();
    record->id = entity;
    record->current_generation = generation;
    record->bound_generation = generation;
    record->touched_at = impl.registry_generation_;
    return generation;
  }
  if (!record->current_generation.IsSet()) {
    record->current_generation = generation;
    record->bound_generation = generation;
    return generation;
  }
  if (generation.Value() < record->current_generation.Value()) {
    return Outcome<EntityGeneration>::Failure(
        ErrorCode::EntityGenerationStale, "the observed entity generation is stale",
        "observed=" + generation.ToString() + " current=" + record->current_generation.ToString());
  }
  if (generation.Value() == record->current_generation.Value()) {
    return generation;
  }
  impl.RetireGeneration(*record, generation,
                        reason.IsSet() ? reason : MakeReason("superseded-by-fabric-registry"));
  auto next = impl.registry_generation_.Next();
  if (!next.HasValue()) {
    return Outcome<EntityGeneration>::Failure(ErrorCode::ArithmeticOverflow,
                                              "registry generation exhausted");
  }
  impl.registry_generation_ = next.Value();
  record->touched_at = impl.registry_generation_;
  return generation;
}

Outcome<void> CapabilityRegistry::InvalidateEntity(const EntityId& entity,
                                                   EntityGeneration generation,
                                                   ReasonToken reason) {
  Impl& impl = *impl_;
  std::unique_lock<std::shared_mutex> lock(impl.mutex_);
  EntityState* record = impl.FindEntity(entity);
  if (record == nullptr) {
    return Status::Failure(ErrorCode::UnknownEntity, "the entity is not known to this registry",
                           entity.ToString());
  }
  if (!generation.IsSet() || !(generation == record->current_generation)) {
    return Status::Failure(ErrorCode::UnknownEntityGeneration,
                           "only the current entity generation can be invalidated",
                           generation.ToString());
  }
  const ReasonToken why = reason.IsSet() ? reason : MakeReason("entity-invalidated");
  bool changed = false;
  for (auto& pair : record->capabilities) {
    const std::size_t before = pair.second.evidence.size();
    impl.MarkNonCurrent(*record, pair.second, EvidenceCurrentness::RevalidationRequired, why,
                        nullptr, nullptr, true);
    if (pair.second.evidence.size() != before || !pair.second.resolution.Actionable()) {
      changed = true;
    }
  }
  if (!record->invalidated) {
    record->invalidated = true;
    record->invalidation_reason = why;
    changed = true;
  }
  if (changed) {
    impl.RefreshEntity(*record);
    auto next = impl.registry_generation_.Next();
    if (!next.HasValue()) {
      return Status::Failure(ErrorCode::ArithmeticOverflow, "registry generation exhausted");
    }
    impl.registry_generation_ = next.Value();
    record->touched_at = impl.registry_generation_;
  }
  return Status::Success();
}

Outcome<std::size_t> CapabilityRegistry::InvalidateSource(const SourceId& source,
                                                          ReasonToken reason) {
  Impl& impl = *impl_;
  std::unique_lock<std::shared_mutex> lock(impl.mutex_);
  if (!source.IsSet()) {
    return Outcome<std::size_t>::Failure(ErrorCode::InvalidArgument,
                                         "source identifier is required");
  }
  const ReasonToken why = reason.IsSet() ? reason : MakeReason("source-invalidated");
  std::size_t invalidated = 0;
  bool any = false;
  for (EntityState& entity : impl.entities_) {
    bool entity_changed = false;
    for (auto& pair : entity.capabilities) {
      CapabilityEntry& entry = pair.second;
      for (const EvidencePtr& record : entry.evidence) {
        if (record->source == source && IsCurrent(record->currentness)) {
          ++invalidated;
        }
      }
      impl.MarkNonCurrent(entity, entry, EvidenceCurrentness::RevalidationRequired, why, nullptr,
                          &source, true);
      entity_changed = true;
    }
    if (entity_changed) {
      impl.RefreshEntity(entity);
      any = true;
    }
  }
  if (any) {
    auto next = impl.registry_generation_.Next();
    if (!next.HasValue()) {
      return Outcome<std::size_t>::Failure(ErrorCode::ArithmeticOverflow,
                                           "registry generation exhausted");
    }
    impl.registry_generation_ = next.Value();
  }
  return invalidated;
}

Outcome<PublicationResult> CapabilityRegistry::Publish(const PublicationRequest& request) {
  return impl_->Publish(request);
}

Outcome<std::vector<PublicationResult>> CapabilityRegistry::PublishBatch(
    std::span<const PublicationRequest> requests) {
  if (requests.size() > limits::kMaxPublicationsPerBatch) {
    return Outcome<std::vector<PublicationResult>>::Failure(
        ErrorCode::TooManyItems, "publication batch exceeds the bound",
        std::to_string(requests.size()));
  }
  std::vector<PublicationResult> results;
  results.reserve(requests.size());
  for (const PublicationRequest& request : requests) {
    auto result = Publish(request);
    if (!result.HasValue()) return result.GetError();
    results.push_back(std::move(result.Value()));
  }
  return results;
}

// ---------------------------------------------------------------------------
// Public API: queries, snapshots, diffs, explanations, statistics, persistence
// ---------------------------------------------------------------------------

Outcome<CapabilityQueryResult> CapabilityRegistry::Query(const EntityId& entity,
                                                         const CapabilityId& capability) const {
  Impl& impl = *impl_;
  std::shared_lock<std::shared_mutex> lock(impl.mutex_);
  CapabilityQueryResult result;
  result.entity = entity;
  result.capability = capability;
  result.registry_generation = impl.registry_generation_;
  result.fails_closed = true;

  const EntityState* record = impl.FindEntity(entity);
  if (record == nullptr) {
    result.state = CapabilityState::Unknown;
    result.actionable = false;
    result.explanation.code = "capability.entity-unknown";
    result.explanation.summary = "the entity is not known to this registry";
    ExplanationStep step = MakeStep("capability.entity-unknown",
                                    "no capability truth is held for this entity");
    AddField(step, "entity", entity.ToString());
    result.explanation.steps.push_back(std::move(step));
    return result;
  }
  result.entity_known = true;
  result.entity_generation = record->current_generation;
  result.set_generation = record->set_generation;
  if (record->invalidated) {
    ExplanationStep step = MakeStep("capability.entity-invalidated",
                                    "the entity generation was invalidated");
    AddField(step, "reason", record->invalidation_reason.IsSet()
                                 ? record->invalidation_reason.Value()
                                 : std::string("unspecified"));
    result.explanation.steps.push_back(std::move(step));
  }

  const auto key_it = impl.capability_keys_.find(capability);
  const CapabilityEntry* entry = nullptr;
  if (key_it != impl.capability_keys_.end()) {
    const auto found = record->capabilities.find(key_it->second);
    if (found != record->capabilities.end()) entry = &found->second;
  }
  if (entry == nullptr || !entry->resolution.capability.IsSet()) {
    result.state = CapabilityState::Unknown;
    result.actionable = false;
    result.explanation.code = "capability.no-record";
    result.explanation.summary = "no capability claim exists for this entity generation";
    ExplanationStep step = MakeStep("capability.no-record",
                                    "the capability has never been claimed for this entity");
    AddField(step, "capability", capability.ToString());
    AddField(step, "entity_generation", record->current_generation.ToString());
    result.explanation.steps.push_back(std::move(step));
    return result;
  }

  const CapabilityResolution& resolution = entry->resolution;
  result.record_exists = true;
  result.state = resolution.state;
  result.value = resolution.value;
  result.has_value = resolution.has_value;
  result.capability_generation = resolution.capability_generation;
  result.evidence_generation = resolution.evidence_generation;
  result.source_generation = resolution.source_generation;
  result.winning_evidence = resolution.winning_evidence;
  result.provenance = resolution.provenance;
  result.source_class = resolution.source_class;
  result.coverage = resolution.coverage;
  result.durability = resolution.durability;
  result.actionable = resolution.Actionable();
  result.fails_closed = resolution.FailsClosedState();
  result.evidence_count = resolution.evidence_count;
  result.current_evidence_count = resolution.current_evidence_count;
  result.outranked_evidence_count = resolution.outranked_evidence_count;
  result.conflicting_evidence_count = resolution.conflicting_evidence_count;
  result.explanation = impl.ExplainResolution(record, entry);

  for (const EvidencePtr& evidence : entry->evidence) {
    EvidenceSummary summary;
    summary.id = evidence->id;
    summary.source = evidence->source;
    summary.publisher = evidence->publisher;
    summary.scope = evidence->scope;
    summary.worker_boot = evidence->worker_boot;
    summary.provenance = evidence->provenance;
    summary.source_class = evidence->source_class;
    summary.durability = evidence->durability;
    summary.coverage = evidence->coverage;
    summary.state = evidence->state;
    summary.value = evidence->value;
    summary.has_value = !evidence->value.IsAbsent();
    summary.currentness = evidence->currentness;
    summary.evidence_generation = evidence->evidence_generation;
    summary.source_generation = evidence->source_generation;
    summary.epoch = evidence->epoch;
    summary.firmware_version = evidence->firmware_version;
    summary.driver_version = evidence->driver_version;
    summary.reason = evidence->reason;
    summary.winning = evidence->id == resolution.winning_evidence;
    summary.outranked = ProvenanceRank(evidence->provenance) > ProvenanceRank(resolution.provenance);
    for (const EvidenceId& conflicting : resolution.conflicting_evidence) {
      if (conflicting == evidence->id) summary.conflicting = true;
    }
    result.evidence.push_back(std::move(summary));
  }
  std::sort(result.evidence.begin(), result.evidence.end(),
            [](const EvidenceSummary& lhs, const EvidenceSummary& rhs) {
              if (lhs.provenance != rhs.provenance) {
                return ProvenanceRank(lhs.provenance) < ProvenanceRank(rhs.provenance);
              }
              if (lhs.source != rhs.source) return lhs.source < rhs.source;
              return lhs.id < rhs.id;
            });
  return result;
}

Outcome<CapabilityQueryResult> CapabilityRegistry::Query(const EntityId& entity,
                                                         EntityGeneration generation,
                                                         const CapabilityId& capability) const {
  {
    std::shared_lock<std::shared_mutex> lock(impl_->mutex_);
    const EntityState* record = impl_->FindEntity(entity);
    if (record == nullptr) {
      return Outcome<CapabilityQueryResult>::Failure(ErrorCode::UnknownEntity,
                                                     "the entity is not known to this registry",
                                                     entity.ToString());
    }
    if (!(generation == record->current_generation)) {
      return Outcome<CapabilityQueryResult>::Failure(
          ErrorCode::UnknownEntityGeneration,
          "capability truth is only served for the current entity generation",
          "requested=" + generation.ToString() + " current=" +
              record->current_generation.ToString());
    }
  }
  return Query(entity, capability);
}

Outcome<EntityCapabilitySet> CapabilityRegistry::QueryEntity(const EntityId& entity) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex_);
  const EntityState* record = impl_->FindEntity(entity);
  if (record == nullptr) {
    return Outcome<EntityCapabilitySet>::Failure(ErrorCode::UnknownEntity,
                                                 "the entity is not known to this registry",
                                                 entity.ToString());
  }
  if (record->set == nullptr) {
    EntityCapabilitySet empty;
    empty.entity = entity;
    empty.entity_generation = record->current_generation;
    empty.invalidated = record->invalidated;
    empty.invalidation_reason = record->invalidation_reason;
    empty.registry_generation = impl_->registry_generation_;
    return empty;
  }
  return *record->set;
}

Outcome<EntityCapabilitySet> CapabilityRegistry::QueryEntity(const EntityId& entity,
                                                             EntityGeneration generation) const {
  {
    std::shared_lock<std::shared_mutex> lock(impl_->mutex_);
    const EntityState* record = impl_->FindEntity(entity);
    if (record == nullptr) {
      return Outcome<EntityCapabilitySet>::Failure(ErrorCode::UnknownEntity,
                                                   "the entity is not known to this registry",
                                                   entity.ToString());
    }
    if (!(generation == record->current_generation)) {
      return Outcome<EntityCapabilitySet>::Failure(
          ErrorCode::UnknownEntityGeneration,
          "capability truth is only served for the current entity generation",
          "requested=" + generation.ToString() + " current=" +
              record->current_generation.ToString());
    }
  }
  return QueryEntity(entity);
}

Outcome<EntityRecordView> CapabilityRegistry::EntityRecord(const EntityId& entity) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex_);
  const EntityState* record = impl_->FindEntity(entity);
  if (record == nullptr) {
    return Outcome<EntityRecordView>::Failure(ErrorCode::UnknownEntity,
                                              "the entity is not known to this registry",
                                              entity.ToString());
  }
  EntityRecordView view;
  view.entity = entity;
  view.current_generation = record->current_generation;
  view.invalidated = record->invalidated;
  view.invalidation_reason = record->invalidation_reason;
  if (record->set != nullptr) {
    view.has_current_set = true;
    view.current_set = *record->set;
  }
  for (const RetainedGeneration& retained : record->history) {
    view.history.push_back(retained.summary);
  }
  return view;
}

Outcome<RetiredGenerationView> CapabilityRegistry::RetiredGeneration(
    const EntityId& entity, EntityGeneration generation) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex_);
  const EntityState* record = impl_->FindEntity(entity);
  if (record == nullptr) {
    return Outcome<RetiredGenerationView>::Failure(ErrorCode::UnknownEntity,
                                                   "the entity is not known to this registry",
                                                   entity.ToString());
  }
  if (generation == record->current_generation) {
    return Outcome<RetiredGenerationView>::Failure(
        ErrorCode::InvalidArgument, "the requested generation is the current generation",
        generation.ToString());
  }
  for (const RetainedGeneration& retained : record->history) {
    if (!(retained.summary.generation == generation)) continue;
    RetiredGenerationView view;
    view.entity = entity;
    view.generation = retained.summary.generation;
    view.set_generation = retained.summary.set_generation;
    view.digest = retained.summary.digest;
    view.capabilities = retained.capability_ids;
    view.supported_known = true;
    view.supported_count = retained.summary.supported_count;
    return view;
  }
  return Outcome<RetiredGenerationView>::Failure(
      ErrorCode::UnknownEntityGeneration, "the entity generation is not retained as history",
      generation.ToString());
}

std::vector<EntityId> CapabilityRegistry::Entities() const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex_);
  std::vector<EntityId> out;
  out.reserve(impl_->entities_.size());
  for (const EntityState& record : impl_->entities_) out.push_back(record.id);
  std::sort(out.begin(), out.end());
  return out;
}

namespace {

std::vector<EntityId> EntitiesFromKeys(const std::deque<EntityState>& entities,
                                       const std::vector<std::uint32_t>& keys) {
  std::vector<EntityId> out;
  out.reserve(keys.size());
  for (const std::uint32_t key : keys) {
    if (key < entities.size()) out.push_back(entities[key].id);
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

std::vector<EntityId> CapabilityRegistry::EntitiesSupporting(const CapabilityId& capability) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex_);
  const auto key = impl_->capability_keys_.find(capability);
  if (key == impl_->capability_keys_.end()) return {};
  const auto entry = impl_->reverse_.find(key->second);
  if (entry == impl_->reverse_.end()) return {};
  return EntitiesFromKeys(impl_->entities_, entry->second.supported);
}

std::vector<EntityId> CapabilityRegistry::EntitiesNotSupporting(
    const CapabilityId& capability) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex_);
  const auto key = impl_->capability_keys_.find(capability);
  if (key == impl_->capability_keys_.end()) return {};
  const auto entry = impl_->reverse_.find(key->second);
  if (entry == impl_->reverse_.end()) return {};
  return EntitiesFromKeys(impl_->entities_, entry->second.unsupported);
}

std::vector<EntityId> CapabilityRegistry::EntitiesWithState(CapabilityState state) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex_);
  std::vector<std::uint32_t> merged;
  for (const auto& entry : impl_->reverse_) {
    const ReverseEntry& reverse = entry.second;
    const std::vector<std::uint32_t>* bucket = nullptr;
    switch (state) {
      case CapabilityState::Supported:
        bucket = &reverse.supported;
        break;
      case CapabilityState::Unsupported:
        bucket = &reverse.unsupported;
        break;
      case CapabilityState::Unknown:
        bucket = &reverse.unknown;
        break;
      case CapabilityState::RevalidationRequired:
        bucket = &reverse.revalidation_required;
        break;
      case CapabilityState::Conflicted:
        bucket = &reverse.conflicted;
        break;
    }
    if (bucket == nullptr) continue;
    merged.insert(merged.end(), bucket->begin(), bucket->end());
  }
  std::sort(merged.begin(), merged.end());
  merged.erase(std::unique(merged.begin(), merged.end()), merged.end());
  return EntitiesFromKeys(impl_->entities_, merged);
}

std::vector<CapabilityId> CapabilityRegistry::CapabilitiesInNamespace(
    const CapabilityNamespaceId& ns) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex_);
  const auto found = impl_->namespace_index_.find(ns);
  if (found == impl_->namespace_index_.end()) return {};
  return std::vector<CapabilityId>(found->second.begin(), found->second.end());
}

std::vector<std::pair<EntityId, CapabilityId>> CapabilityRegistry::CapabilitiesRequiringRevalidation()
    const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex_);
  return std::vector<std::pair<EntityId, CapabilityId>>(impl_->revalidation_.begin(),
                                                        impl_->revalidation_.end());
}

std::vector<EvidenceSummary> CapabilityRegistry::EvidenceFor(const EntityId& entity,
                                                             const CapabilityId& capability) const {
  auto query = Query(entity, capability);
  if (!query.HasValue()) return {};
  return query.Value().evidence;
}

RequirementEvaluation CapabilityRegistry::Evaluate(const EntityId& entity,
                                                   const Requirement& requirement) const {
  return EvaluateRequirementAgainst(*this, entity, requirement);
}

Outcome<CapabilitySnapshot> CapabilityRegistry::CreateSnapshot(const SnapshotScope& scope) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex_);
  CapabilitySnapshot snapshot;
  snapshot.registry_generation = impl_->registry_generation_;
  snapshot.epoch = impl_->epoch_;
  snapshot.scope = scope;

  std::vector<const EntityState*> selected;
  if (scope.all_entities) {
    selected.reserve(impl_->entities_.size());
    for (const EntityState& record : impl_->entities_) selected.push_back(&record);
  } else {
    for (const EntityId& id : scope.entities) {
      const EntityState* record = impl_->FindEntity(id);
      if (record != nullptr) selected.push_back(record);
    }
  }
  std::sort(selected.begin(), selected.end(),
            [](const EntityState* lhs, const EntityState* rhs) { return lhs->id < rhs->id; });
  if (selected.size() > limits::kMaxSnapshotEntities) {
    return Outcome<CapabilitySnapshot>::Failure(ErrorCode::TooManyItems,
                                                "snapshot scope exceeds the entity bound");
  }

  for (const EntityState* record : selected) {
    if (!scope.entity_kinds.empty() &&
        std::find(scope.entity_kinds.begin(), scope.entity_kinds.end(), record->id.Kind()) ==
            scope.entity_kinds.end()) {
      continue;
    }
    if (!scope.namespaces.empty()) {
      bool match = false;
      for (const CapabilityNamespaceId& ns : scope.namespaces) {
        const auto found = impl_->namespace_index_.find(ns);
        if (found == impl_->namespace_index_.end()) continue;
        for (const CapabilityResolution& resolution : record->set != nullptr
                                                          ? record->set->capabilities
                                                          : std::vector<CapabilityResolution>{}) {
          if (found->second.find(resolution.capability) != found->second.end()) {
            match = true;
            break;
          }
        }
        if (match) break;
      }
      if (!match) continue;
    }
    SnapshotEntityEntry entry;
    entry.entity = record->id;
    entry.entity_generation = record->current_generation;
    entry.set_generation = record->set_generation;
    entry.set_digest = record->set != nullptr ? record->set->digest : Digest{};
    entry.set = record->set;
    snapshot.entities.push_back(std::move(entry));
  }

  std::vector<std::byte> bytes;
  bytes.reserve(snapshot.entities.size() * 96 + 64);
  ByteWriter writer(bytes);
  writer.U64(snapshot.registry_generation.Value());
  writer.U64(snapshot.epoch.Value());
  writer.Text(scope.ToText(), 4096);
  writer.U32(static_cast<std::uint32_t>(snapshot.entities.size()));
  for (const SnapshotEntityEntry& entry : snapshot.entities) {
    writer.Text(entry.entity.ToString(), limits::kMaxEntityIdLength);
    writer.U64(entry.entity_generation.Value());
    writer.U64(entry.set_generation.Value());
    for (const std::byte value : entry.set_digest.Bytes()) writer.U8(static_cast<std::uint8_t>(value));
  }
  snapshot.digest = fabric::capability::ComputeDigest(bytes);
  auto id = SnapshotId::Parse(snapshot.digest.ShortString());
  snapshot.id = id.HasValue() ? id.Value() : SnapshotId{};
  return snapshot;
}

SnapshotCurrentness CapabilityRegistry::CheckSnapshot(const CapabilitySnapshot& snapshot) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex_);
  SnapshotCurrentness currentness;
  currentness.current = true;
  if (!(snapshot.registry_generation == impl_->registry_generation_)) {
    currentness.current = false;
    currentness.reasons.push_back("registry generation advanced from " +
                                  snapshot.registry_generation.ToString() + " to " +
                                  impl_->registry_generation_.ToString());
  }
  if (!(snapshot.epoch == impl_->epoch_)) {
    currentness.current = false;
    currentness.reasons.push_back("coordinator epoch advanced from " + snapshot.epoch.ToString() +
                                  " to " + impl_->epoch_.ToString());
  }
  for (const SnapshotEntityEntry& entry : snapshot.entities) {
    const EntityState* record = impl_->FindEntity(entry.entity);
    if (record == nullptr) {
      currentness.current = false;
      currentness.reasons.push_back("entity " + entry.entity.ToString() + " disappeared");
      continue;
    }
    if (!(record->current_generation == entry.entity_generation)) {
      currentness.current = false;
      currentness.reasons.push_back("entity " + entry.entity.ToString() +
                                    " advanced to generation " +
                                    record->current_generation.ToString());
      continue;
    }
    if (!(record->set_generation == entry.set_generation)) {
      currentness.current = false;
      currentness.reasons.push_back("capability set of " + entry.entity.ToString() +
                                    " advanced to generation " +
                                    record->set_generation.ToString());
      continue;
    }
    const Digest digest = record->set != nullptr ? record->set->digest : Digest{};
    if (!(digest == entry.set_digest)) {
      currentness.current = false;
      currentness.reasons.push_back("capability digest of " + entry.entity.ToString() +
                                    " changed");
    }
  }
  return currentness;
}

Outcome<CapabilityDiff> CapabilityRegistry::DiffAgainstSnapshot(
    const CapabilitySnapshot& snapshot) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex_);
  CapabilityDiff diff;
  diff.before_entity_generation = EntityGeneration{};
  if (snapshot.entities.size() > limits::kMaxDiffEntries) {
    return Outcome<CapabilityDiff>::Failure(ErrorCode::TooManyItems,
                                            "snapshot is too large to diff");
  }
  std::vector<EntityId> snapshot_entities;
  snapshot_entities.reserve(snapshot.entities.size());
  for (const SnapshotEntityEntry& entry : snapshot.entities) {
    snapshot_entities.push_back(entry.entity);
    const EntityState* record = impl_->FindEntity(entry.entity);
    const EntityCapabilitySet* current = record != nullptr ? record->set.get() : nullptr;
    CapabilityDiff entity_diff = Impl::BuildDiff(entry.entity, entry.set.get(), current);
    diff.entries.insert(diff.entries.end(),
                        std::make_move_iterator(entity_diff.entries.begin()),
                        std::make_move_iterator(entity_diff.entries.end()));
  }
  std::sort(snapshot_entities.begin(), snapshot_entities.end());
  for (const EntityState& record : impl_->entities_) {
    if (std::binary_search(snapshot_entities.begin(), snapshot_entities.end(), record.id)) continue;
    CapabilityDiff entity_diff = Impl::BuildDiff(record.id, nullptr, record.set.get());
    diff.entries.insert(diff.entries.end(),
                        std::make_move_iterator(entity_diff.entries.begin()),
                        std::make_move_iterator(entity_diff.entries.end()));
  }
  std::sort(diff.entries.begin(), diff.entries.end(),
            [](const CapabilityDiffEntry& lhs, const CapabilityDiffEntry& rhs) {
              if (!(lhs.capability == rhs.capability)) return lhs.capability < rhs.capability;
              return static_cast<std::uint8_t>(lhs.kind) < static_cast<std::uint8_t>(rhs.kind);
            });
  return diff;
}

Outcome<CapabilityDiff> CapabilityRegistry::DiffEntityAgainstSnapshot(
    const CapabilitySnapshot& snapshot, const EntityId& entity) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex_);
  const SnapshotEntityEntry* entry = snapshot.Find(entity);
  const EntityState* record = impl_->FindEntity(entity);
  if (entry == nullptr && record == nullptr) {
    return Outcome<CapabilityDiff>::Failure(ErrorCode::NotFound,
                                            "the entity is neither in the snapshot nor in the "
                                            "registry",
                                            entity.ToString());
  }
  return Impl::BuildDiff(entity, entry != nullptr ? entry->set.get() : nullptr,
                         record != nullptr ? record->set.get() : nullptr);
}

Explanation CapabilityRegistry::ExplainCapability(const EntityId& entity,
                                                  const CapabilityId& capability) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex_);
  const EntityState* record = impl_->FindEntity(entity);
  const CapabilityEntry* entry = nullptr;
  if (record != nullptr) {
    const auto key = impl_->capability_keys_.find(capability);
    if (key != impl_->capability_keys_.end()) {
      const auto found = record->capabilities.find(key->second);
      if (found != record->capabilities.end()) entry = &found->second;
    }
  }
  return impl_->ExplainResolution(record, entry);
}

Outcome<Explanation> CapabilityRegistry::ExplainPublication(const PublicationId& publication) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex_);
  const auto found = impl_->journal_.find(publication);
  if (found == impl_->journal_.end()) {
    return Outcome<Explanation>::Failure(ErrorCode::NotFound,
                                         "no publication with this identifier is retained",
                                         publication.Value());
  }
  Explanation explanation = found->second.explanation;
  ExplanationStep step = MakeStep("publication.journal", "retained publication outcome");
  AddField(step, "publication", found->second.publication.Value());
  AddField(step, "attempt", found->second.attempt.Value());
  AddField(step, "status", std::string(PublicationStatusName(found->second.status)));
  AddField(step, "code", std::string(ErrorCodeName(found->second.code)));
  AddField(step, "message", found->second.message);
  AddField(step, "entity", found->second.entity.ToString());
  AddField(step, "entity_generation", found->second.entity_generation.ToString());
  AddField(step, "previous_set_generation", found->second.previous_generation.ToString());
  AddField(step, "set_generation", found->second.new_generation.ToString());
  AddField(step, "registry_generation", found->second.registry_generation.ToString());
  explanation.steps.push_back(std::move(step));
  return explanation;
}

Outcome<Explanation> CapabilityRegistry::ExplainReplay(const MutationAttemptId& attempt) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex_);
  const auto found = impl_->attempt_journal_.find(attempt);
  if (found == impl_->attempt_journal_.end()) {
    return Outcome<Explanation>::Failure(
        ErrorCode::NotFound, "no retained publication used this mutation attempt identifier",
        attempt.Value());
  }
  return ExplainPublication(found->second);
}

Outcome<Explanation> CapabilityRegistry::ExplainGeneration(const EntityId& entity,
                                                           CapabilitySetGeneration generation) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex_);
  for (const PublicationId& id : impl_->journal_order_) {
    const auto found = impl_->journal_.find(id);
    if (found == impl_->journal_.end()) continue;
    if (!(found->second.entity == entity)) continue;
    if (!(found->second.new_generation == generation)) continue;
    Explanation explanation = found->second.explanation;
    ExplanationStep step = MakeStep("generation.advanced", "capability set generation history");
    AddField(step, "entity", entity.ToString());
    AddField(step, "previous_set_generation", found->second.previous_generation.ToString());
    AddField(step, "set_generation", found->second.new_generation.ToString());
    AddField(step, "attempt", found->second.attempt.Value());
    AddField(step, "publisher_status", std::string(PublicationStatusName(found->second.status)));
    explanation.steps.push_back(std::move(step));
    return explanation;
  }
  return Outcome<Explanation>::Failure(
      ErrorCode::NotFound,
      "the capability set generation was not advanced by a retained publication",
      "entity=" + entity.ToString() + " generation=" + generation.ToString());
}

Digest CapabilityRegistry::ComputeDigest() const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex_);
  std::vector<const EntityState*> ordered;
  ordered.reserve(impl_->entities_.size());
  for (const EntityState& record : impl_->entities_) ordered.push_back(&record);
  std::sort(ordered.begin(), ordered.end(),
            [](const EntityState* lhs, const EntityState* rhs) { return lhs->id < rhs->id; });

  std::vector<std::byte> bytes;
  bytes.reserve(ordered.size() * 96 + 128);
  ByteWriter writer(bytes);
  writer.Text("fabric-capability-registry", 64);
  writer.U32(kFormatVersion);
  writer.U64(impl_->epoch_.Value());
  writer.U32(static_cast<std::uint32_t>(ordered.size()));
  for (const EntityState* record : ordered) {
    writer.Text(record->id.ToString(), limits::kMaxEntityIdLength);
    writer.U64(record->current_generation.Value());
    writer.U64(record->set_generation.Value());
    writer.Bool(record->invalidated);
    const Digest digest = record->set != nullptr ? record->set->digest : Digest{};
    for (const std::byte value : digest.Bytes()) writer.U8(static_cast<std::uint8_t>(value));
    writer.U32(static_cast<std::uint32_t>(record->history.size()));
    for (const RetainedGeneration& retained : record->history) {
      writer.U64(retained.summary.generation.Value());
      writer.U64(retained.summary.set_generation.Value());
      for (const std::byte value : retained.summary.digest.Bytes()) {
        writer.U8(static_cast<std::uint8_t>(value));
      }
      writer.U64(retained.summary.capability_count);
      writer.U64(retained.summary.supported_count);
    }
  }
  std::vector<WorkerBootId> boots(impl_->fenced_boots_.begin(), impl_->fenced_boots_.end());
  std::sort(boots.begin(), boots.end());
  writer.U32(static_cast<std::uint32_t>(boots.size()));
  for (const WorkerBootId& boot : boots) writer.Text(boot.Value(), 64);
  return fabric::capability::ComputeDigest(bytes);
}

RegistryStatistics CapabilityRegistry::Statistics() const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex_);
  RegistryStatistics stats;
  stats.entities = impl_->entities_.size();
  stats.authority_scopes = impl_->authorities_.size();
  stats.registered_publishers = impl_->publishers_.size();
  stats.fenced_worker_boots = impl_->fenced_boots_.size();
  stats.vendor_descriptors = schema_.Size() - CanonicalSchema().Size();
  stats.registry_generation = impl_->registry_generation_;
  stats.epoch = impl_->epoch_;
  stats.rejection_journal_entries = impl_->journal_.size();
  for (const auto& entry : impl_->publishers_) {
    stats.replay_entries += entry.second.replay.size();
  }
  for (const EntityState& record : impl_->entities_) {
    stats.retired_generations += record.history.size();
    stats.capability_records += record.capabilities.size();
    for (const auto& pair : record.capabilities) {
      stats.evidence_records += pair.second.evidence.size();
      switch (pair.second.resolution.state) {
        case CapabilityState::Supported:
          ++stats.supported;
          break;
        case CapabilityState::Unsupported:
          ++stats.unsupported;
          break;
        case CapabilityState::Unknown:
          ++stats.unknown;
          break;
        case CapabilityState::RevalidationRequired:
          ++stats.revalidation_required;
          break;
        case CapabilityState::Conflicted:
          ++stats.conflicted;
          break;
      }
    }
  }
  return stats;
}

Outcome<SaveReport> CapabilityRegistry::Save(const PersistenceConfig& config) const {
  internal::StorePayload payload;
  {
    std::shared_lock<std::shared_mutex> lock(impl_->mutex_);
    payload.epoch = impl_->epoch_;
    payload.registry_generation = impl_->registry_generation_;
    payload.authorities = impl_->authorities_;
    std::sort(payload.authorities.begin(), payload.authorities.end(),
              [](const AuthorityGrant& lhs, const AuthorityGrant& rhs) {
                return lhs.scope < rhs.scope;
              });
    for (const auto& entry : impl_->publishers_) {
      if (!entry.second.active) continue;
      internal::StoredRegistration stored;
      stored.publisher = entry.first.publisher;
      stored.worker_boot = entry.first.boot;
      stored.scope = entry.second.registration.scope;
      payload.registrations.push_back(std::move(stored));
    }
    std::sort(payload.registrations.begin(), payload.registrations.end(),
              [](const internal::StoredRegistration& lhs, const internal::StoredRegistration& rhs) {
                if (!(lhs.publisher == rhs.publisher)) return lhs.publisher < rhs.publisher;
                return lhs.worker_boot < rhs.worker_boot;
              });
    for (const FenceRecord& fence : impl_->fences_) {
      internal::StoredFence stored;
      stored.worker_boot = fence.worker_boot;
      stored.publisher = fence.publisher;
      stored.reason = fence.reason;
      payload.fences.push_back(std::move(stored));
    }
    std::sort(payload.fences.begin(), payload.fences.end(),
              [](const internal::StoredFence& lhs, const internal::StoredFence& rhs) {
                return lhs.worker_boot < rhs.worker_boot;
              });

    std::vector<const EntityState*> ordered;
    ordered.reserve(impl_->entities_.size());
    for (const EntityState& record : impl_->entities_) ordered.push_back(&record);
    std::sort(ordered.begin(), ordered.end(),
              [](const EntityState* lhs, const EntityState* rhs) { return lhs->id < rhs->id; });

    for (const EntityState* record : ordered) {
      internal::StoredEntity stored;
      stored.id = record->id;
      stored.current_generation = record->current_generation;
      stored.bound_generation = record->bound_generation;
      stored.has_set = record->has_set;
      stored.set_generation = record->set_generation;
      stored.invalidated = record->invalidated;
      stored.invalidation_reason = record->invalidation_reason;
      for (const auto& pair : record->capabilities) {
        internal::StoredCapability capability;
        capability.id = pair.second.id;
        capability.generation = pair.second.generation;
        stored.capabilities.push_back(std::move(capability));
        for (const EvidencePtr& evidence : pair.second.evidence) {
          if (!IsCurrent(evidence->currentness) &&
              evidence->currentness != EvidenceCurrentness::RevalidationRequired &&
              evidence->currentness != EvidenceCurrentness::Fenced) {
            continue;
          }
          internal::StoredClaim claim;
          claim.capability = pair.second.id;
          claim.id = evidence->id;
          claim.source = evidence->source;
          claim.publisher = evidence->publisher;
          claim.scope = evidence->scope;
          claim.worker_boot = evidence->worker_boot;
          claim.epoch = evidence->epoch;
          claim.source_generation = evidence->source_generation;
          claim.evidence_generation = evidence->evidence_generation;
          claim.attempt = evidence->attempt;
          claim.publication = evidence->publication;
          claim.provenance = evidence->provenance;
          claim.source_class = evidence->source_class;
          claim.durability = evidence->durability;
          claim.coverage = evidence->coverage;
          claim.state = evidence->state;
          claim.value = evidence->value;
          claim.firmware_version = evidence->firmware_version;
          claim.driver_version = evidence->driver_version;
          claim.reason = evidence->reason;
          claim.currentness = evidence->currentness;
          stored.claims.push_back(std::move(claim));
        }
      }
      std::sort(stored.capabilities.begin(), stored.capabilities.end(),
                [](const internal::StoredCapability& lhs, const internal::StoredCapability& rhs) {
                  return lhs.id < rhs.id;
                });
      std::sort(stored.claims.begin(), stored.claims.end(),
                [](const internal::StoredClaim& lhs, const internal::StoredClaim& rhs) {
                  if (!(lhs.capability == rhs.capability)) return lhs.capability < rhs.capability;
                  if (!(lhs.source == rhs.source)) return lhs.source < rhs.source;
                  return lhs.evidence_generation.Value() < rhs.evidence_generation.Value();
                });
      for (const auto& floor : record->source_high_water) {
        internal::StoredSourceFloor stored_floor;
        stored_floor.source = floor.first.source;
        stored_floor.worker_boot = floor.first.boot;
        stored_floor.generation = floor.second;
        stored.source_floors.push_back(std::move(stored_floor));
      }
      for (const RetainedGeneration& retained : record->history) {
        internal::StoredGeneration generation;
        generation.generation = retained.summary.generation;
        generation.set_generation = retained.summary.set_generation;
        generation.digest = retained.summary.digest;
        generation.capability_count = retained.summary.capability_count;
        generation.supported_count = retained.summary.supported_count;
        generation.retired_at = retained.summary.retired_at;
        generation.reason = retained.summary.reason;
        stored.history.push_back(std::move(generation));
      }
      payload.entities.push_back(std::move(stored));
    }
  }
  return internal::WriteStore(config, payload);
}

Outcome<LoadReport> CapabilityRegistry::Load(const PersistenceConfig& config) {
  internal::StorePayload payload;
  auto read = internal::ReadStore(config, payload);
  if (!read.HasValue()) return read.GetError();
  LoadReport report = read.Value();

  Impl& impl = *impl_;
  std::unique_lock<std::shared_mutex> lock(impl.mutex_);
  impl.entities_.clear();
  impl.entity_keys_.clear();
  impl.capability_keys_.clear();
  impl.capability_ids_.clear();
  impl.reverse_.clear();
  impl.namespace_index_.clear();
  impl.publisher_evidence_.clear();
  impl.source_evidence_.clear();
  impl.revalidation_.clear();
  impl.publishers_.clear();
  impl.fenced_boots_.clear();
  impl.fences_.clear();
  impl.authorities_.clear();
  impl.authority_index_.clear();
  impl.journal_.clear();
  impl.journal_order_.clear();
  impl.attempt_journal_.clear();
  impl.arrival_sequence_ = 0;

  impl.epoch_ = payload.epoch.IsSet() ? payload.epoch : CoordinatorEpoch::FromValue(1);
  impl.registry_generation_ = payload.registry_generation;

  for (const AuthorityGrant& grant : payload.authorities) {
    if (!grant.scope.IsSet()) continue;
    impl.authority_index_[grant.scope] = impl.authorities_.size();
    impl.authorities_.push_back(grant);
    ++report.authority_grants;
  }
  for (const internal::StoredFence& stored : payload.fences) {
    if (!stored.worker_boot.IsSet()) continue;
    impl.fenced_boots_.insert(stored.worker_boot);
    FenceRecord record;
    record.worker_boot = stored.worker_boot;
    record.publisher = stored.publisher;
    record.reason = stored.reason;
    record.fenced_at = impl.registry_generation_;
    impl.fences_.push_back(std::move(record));
    ++report.fenced_worker_boots;
  }

  for (const internal::StoredRegistration& stored : payload.registrations) {
    if (!stored.publisher.IsSet() || !stored.worker_boot.IsSet()) continue;
    if (impl.fenced_boots_.find(stored.worker_boot) != impl.fenced_boots_.end()) continue;
    // Restored without authority: the next coordinator epoch advance fences it, so live
    // publication authority never silently survives a restart.
    PublisherRuntime& runtime = impl.publishers_[PublisherKey{stored.publisher,
                                                              stored.worker_boot}];
    runtime.registration.publisher = stored.publisher;
    runtime.registration.scope = stored.scope;
    runtime.registration.worker_boot = stored.worker_boot;
    runtime.registration.epoch = impl.epoch_;
    runtime.registration.accepted_generation = impl.registry_generation_;
    runtime.active = false;
  }

  for (internal::StoredEntity& stored : payload.entities) {
    if (!stored.id.IsSet()) continue;
    impl.entity_keys_.emplace(stored.id, static_cast<std::uint32_t>(impl.entities_.size()));
    impl.entities_.emplace_back();
    EntityState& entity = impl.entities_.back();
    entity.id = stored.id;
    entity.current_generation = stored.current_generation;
    entity.bound_generation = stored.bound_generation.IsSet() ? stored.bound_generation
                                                              : stored.current_generation;
    entity.has_set = stored.has_set;
    entity.set_generation = stored.set_generation;
    entity.invalidated = stored.invalidated;
    entity.invalidation_reason = stored.invalidation_reason;
    entity.touched_at = impl.registry_generation_;

    for (const internal::StoredCapability& capability : stored.capabilities) {
      if (!capability.id.IsSet()) continue;
      const std::uint32_t key = impl.CapabilityKey(capability.id);
      CapabilityEntry& entry = entity.capabilities[key];
      entry.id = capability.id;
      entry.generation = capability.generation;
      Impl::TouchNamespace(impl.namespace_index_, capability.id);
    }
    for (internal::StoredClaim& claim : stored.claims) {
      if (!claim.capability.IsSet()) continue;
      const std::uint32_t key = impl.CapabilityKey(claim.capability);
      CapabilityEntry& entry = entity.capabilities[key];
      entry.id = claim.capability;
      auto record = std::make_shared<EvidenceRecord>();
      record->id = claim.id;
      record->capability = claim.capability;
      record->entity = entity.id;
      record->entity_generation = entity.bound_generation;
      record->source = claim.source;
      record->publisher = claim.publisher;
      record->scope = claim.scope;
      record->worker_boot = claim.worker_boot;
      record->epoch = claim.epoch;
      record->source_generation = claim.source_generation;
      record->evidence_generation = claim.evidence_generation;
      record->attempt = claim.attempt;
      record->publication = claim.publication;
      record->provenance = claim.provenance;
      record->source_class = claim.source_class;
      record->durability = claim.durability;
      record->coverage = claim.coverage;
      record->state = claim.state;
      record->value = claim.value;
      record->firmware_version = claim.firmware_version;
      record->driver_version = claim.driver_version;
      record->reason = claim.reason;
      if (claim.currentness == EvidenceCurrentness::Fenced) {
        record->currentness = EvidenceCurrentness::Fenced;
      } else if (claim.durability == DurabilityClass::Durable) {
        record->currentness = EvidenceCurrentness::Current;
        ++report.durable_evidence_retained;
      } else {
        record->currentness = EvidenceCurrentness::RevalidationRequired;
        ++report.evidence_revalidation_required;
      }
      record->accepted_generation = impl.registry_generation_;
      record->arrival_sequence = ++impl.arrival_sequence_;
      impl.IndexEvidence(*record);
      entry.evidence.push_back(std::move(record));
      ++report.evidence_loaded;
    }
    for (const internal::StoredSourceFloor& floor : stored.source_floors) {
      if (!floor.source.IsSet()) continue;
      entity.source_high_water[SourceKey{floor.source, floor.worker_boot}] = floor.generation;
    }
    for (const internal::StoredGeneration& generation : stored.history) {
      if (!generation.generation.IsSet()) continue;
      RetainedGeneration retained;
      retained.summary.generation = generation.generation;
      retained.summary.set_generation = generation.set_generation;
      retained.summary.digest = generation.digest;
      retained.summary.capability_count = generation.capability_count;
      retained.summary.supported_count = generation.supported_count;
      retained.summary.retired_at = generation.retired_at;
      retained.summary.reason = generation.reason;
      entity.history.push_back(std::move(retained));
      ++report.generations_recovered;
    }
    for (auto& pair : entity.capabilities) {
      impl.ResolveCapability(entity, pair.first, pair.second);
    }
    entity.set = impl.Materialize(entity);
    ++report.entities_loaded;
  }
  report.epoch = impl.epoch_;
  report.registry_generation = impl.registry_generation_;
  return report;
}


}  // namespace fabric::capability