# Fabric Capability Registry

Fabric Capability Registry is the authoritative capability-knowledge runtime of the
Distributed Fabric Infrastructure / Fabric OS stack. It answers one question: what
capabilities does this exact fabric entity generation authoritatively support now,
under which provenance and evidence, with what constraints, and when must a capability
claim be rejected, superseded, invalidated, fenced or reduced to UNKNOWN?

It is a vendor-neutral C++20 library with a governance boundary, not a configuration
store and not a telemetry system.

## Systems boundary

Fabric Capability Registry owns capability truth only.

| Not owned here | Runtime that owns it |
| --- | --- |
| Canonical entity identity | Fabric Registry |
| Structural relationships | Fabric Topology |
| Live link operational condition | Link State Fabric |
| Port configuration | Port Fabric |
| Queue runtime state and allocation | Queue Fabric |
| Packet-buffer allocation and pressure | Buffer Fabric |
| Operational congestion control | Network Congestion Fabric |
| Policy and authorization decisions | Policy runtimes |

A SUPPORTED capability says nothing about whether anything is enabled, configured,
active, healthy, available or authorized. Those concepts are deliberately absent.

## Capability schema

Capabilities are declared in a governed schema, never as free-form key/value strings.
Each descriptor fixes a typed value form, unit, bounds, cardinality and optional
enumeration domain. Canonical descriptors cover the protocol, forwarding, port, queue,
buffer-exposure, telemetry, offload, RDMA, congestion, QoS, tunneling, virtualization,
timestamping, optics, device-management and compatibility namespaces. Vendor extensions
live in vendor.<vendor>[.<domain>] namespaces, are registered at run time, are bounded,
and can never shadow a canonical capability.

Typed value forms: boolean, integer, quantity with unit, enumeration, enumeration set,
bitset, numeric set, numeric range, version interval, protocol set, tuple, tuple set,
structured record and bounded opaque vendor payload. Numeric limits are never stored as
strings. Sets are canonical (sorted, de-duplicated) and contradictory values are
rejected at construction and again when decoding.

## Capability states

SUPPORTED, UNSUPPORTED, UNKNOWN, REVALIDATION_REQUIRED and CONFLICTED are first-class.
UNKNOWN means insufficient current evidence. UNSUPPORTED means authoritative evidence of
absence. REVALIDATION_REQUIRED means prior durable evidence exists but is not current
enough to act on. CONFLICTED means current sources disagree in a way the deterministic
rules cannot resolve. Evaluation is fail-closed: only SUPPORTED is actionable support.

## Evidence, provenance and conflict resolution

Evidence is provenance, never authority. Provenance classes are an ordered strength
classification, not a confidence score: direct hardware enumeration, direct driver or
operating system API, authoritative administrative declaration, vendor or firmware
manifest, imported static profile, inferred evidence, synthetic test backend.

Resolution is deterministic: the strongest provenance class present decides; agreement
inside that class resolves; disagreement inside the strongest class is CONFLICTED; if
the strongest class has no current member the capability is REVALIDATION_REQUIRED and
weaker current evidence is never silently used instead; with no claim the capability is
UNKNOWN.

## Generation, authority and fencing

Entity generations are issued by Fabric Registry and only observed here; replacing a
generation fences the previous generation's claims as historical. Capability set,
capability, evidence and source generations advance exactly once per committed semantic
change; exact idempotent replays advance nothing and stale replays are rejected.
Authority is explicit and enumerative with no wildcard. A publisher restart mints a
fresh worker boot identity; a fenced boot can never publish again. A coordinator restart
advances the epoch, fences previously registered boots and turns process-bound evidence
into REVALIDATION_REQUIRED while durable administrative declarations survive.

## Persistence and recovery

The store is a single versioned, integrity-checked file: magic, format version, payload
length, record count, SHA-256 payload digest, header CRC-32, then deterministic
length-prefixed records. Decoding is bounds-checked and rejects unknown record types,
duplicate entities, duplicate capabilities, out-of-range enum values, malformed ranges,
malformed sets and unsupported versions. Replacement is atomic. Recovery is
conservative: a store never turns a live process observation into fresh current truth.

## Compatibility queries

Requirement expressions are bounded and explicit: state equality, minimum, maximum,
membership, range containment, version containment, protocol and enumeration membership,
ALL_OF, ANY_OF and NOT, with bounded depth and node count and no embedded scripting.

## REAL, SYNTHETIC, UNSUPPORTED

REAL: host capability discovery reads genuinely host-visible sources (adapter
enumeration, interface property tables, bounded PnP device properties) and real
process and network behaviour (real coordinator and publisher processes over framed
loopback TCP, real TerminateProcess kills).

SYNTHETIC: switch, router, SmartNIC, DPU, optical, high-speed port, RDMA, telemetry-rich
and reduced-capability models are labelled synthetic-test-backend and are never
presented as physical proof.

UNSUPPORTED: physical switch ASIC capability, NDIS offload OID enumeration and PCI
configuration space reads are not implemented; host discovery reports those as
NOT_REPORTED (UNKNOWN), never as UNSUPPORTED.

## Build

    cmake -S . -B build -G "Visual Studio 17 2022" -A x64
    cmake --build build --config Release --parallel

Options: FCR_BUILD_SHARED, FCR_BUILD_TESTS, FCR_BUILD_TOOLS, FCR_BUILD_EXAMPLES,
FCR_BUILD_BENCHMARKS, FCR_WARNINGS_AS_ERRORS, FCR_ENABLE_ANALYZE, FCR_ENABLE_ASAN.
MSVC builds use /W4 /permissive- with /WX and no warning is globally suppressed.
FCR_ENABLE_ASAN=ON fails configure when the toolchain has no AddressSanitizer runtime
instead of pretending to instrument.

## Test

    ctest --test-dir build -C Release --output-on-failure

Suites: ids, value, schema, publication, authority, query, requirement, persistence,
concurrency, property, adversarial, discovery, synthetic, wire, distributed,
coordinator_restart, cli. No test uses a timeout or watchdog: a hanging test is a defect.

## Install and consume

    cmake --install build --config Release --prefix <prefix>
    cmake -S tests/consumer -B consumer-build -DCMAKE_PREFIX_PATH=<prefix>
    cmake --build consumer-build --config Release

The package exports SummonSoftwareLabs::FabricCapabilityRegistry through
find_package(FabricCapabilityRegistry CONFIG REQUIRED).

## Tools

fcrctl is a deterministic inspection CLI (REAL discovery, synthetic profiles, store
import, entities, capability/evidence/explanation inspection, snapshots, diffs, bounded
compatibility queries, store inspection). fcr_coordinator is a real coordinator process
and fcr_publisher a real publisher worker process.

## Genuine limitations

* REAL discovery is implemented for Windows hosts; other platforms report
  discovery-unavailable instead of fabricating data.
* Switch ASIC capability, NDIS offload OIDs and PCI configuration space are not read.
* Retired generation identifier lists are in-memory only; digests and counts are durable.
* Superseded evidence lineage is not persisted; stale-replay protection across a restart
  is carried by persisted source-generation floors.
* AddressSanitizer is not installed in the reference build environment; /analyze and the
  runtime checks are the available substitutes.
* This revision has NOT passed its full validation suite; see the status report
  accompanying the commit. It must not be treated as a closed 1.0.0 release.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
