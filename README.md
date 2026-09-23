# Loss Observatory

A standalone, vendor-neutral packet-loss observation, attribution, and
localization runtime for Fabric OS deployments. Copyright 2026 Summon Software Labs.
Apache License 2.0. No telemetry transmission.

Loss Observatory owns one question and refuses to answer any other: *what does
the evidence actually prove about packet loss, and how precisely can it be
placed?* It does not repair paths, reroute traffic, control congestion, or
declare blackholes. It never treats missing telemetry as loss.

## What it does

| Capability | Implementation |
| --- | --- |
| Typed identities | `FlowId`, `PathId`, `HopId`, `LinkId`, `QueueId`, `SourceId`, `EpochId`, `GenerationId`, `RevisionId`, `MeasurementId`, `EpisodeId`, `SequenceId` — each a distinct type; no conversion between identity kinds exists |
| Evidence | Counter deltas, probe reports, sequence-gap reports, endpoint comparisons; each carries source, incarnation epoch, generation, topology revision, source sequence, observation time, and receive time |
| Measurement semantics | Declared per method: finest supportable granularity, clock requirements, generation stability, synthetic flag, implemented-or-not |
| Loss classes | `absent-evidence`, `no-loss-observed`, `confirmed-loss`, `suspected-loss`, `discontinuity`, `stale-evidence`, `conflicting-evidence`, `incomplete-evidence`, `unsupported-method`, `implausible-evidence`, `unknown` |
| Attribution | Integer confidence score built from named terms, with a band and per-source claims |
| Localization | Hop/link/queue attribution that can never exceed the granularity of the evidence behind it, with retained ambiguity |
| Reset and wrap handling | Counter resets, wraps, unresolved decreases, out-of-order readings, epoch and generation changes, and implausible jumps are explicit discontinuities, never loss |
| Freshness | Re-evaluated at every query: age, clock skew, retired incarnations, superseded generations, changed topology revisions, and recovery from disk |
| Conflict | Admissible sources that disagree produce `conflicting-evidence` unless a strictly higher authority resolves it, and the losing claim is always retained |
| History | Bounded loss episodes with lifecycle states; episodes never resume across a restart |
| Persistence | CRC-32C-checked, versioned, append-only journal with conservative recovery, compaction, and session epochs |
| Tooling | `locctl` CLI plus a deterministic line language for ingest, classify, localize, history, explain, and export |

## Quick start

```powershell
pwsh scripts/build.ps1 -Config Release
pwsh scripts/test.ps1 -Config Release
pwsh scripts/build.ps1 -BuildDir build-asan -Asan
pwsh scripts/test.ps1 -BuildDir build-asan -Asan
pwsh scripts/verify-package.ps1
```

Without the scripts:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## Using the library

```cpp
#include "loss_observatory/engine.hpp"

using namespace loss_observatory;

ManualClock clock{Timestamp::from_unix_seconds(1767225600)};
EngineConfig config{};
config.loss.freshness.max_age = Duration::from_seconds(30);

ObservatoryEngine engine(config, clock);
engine.start();

// Declarations and evidence both arrive through one script language, or
// directly through engine.ingest() with a fully populated EvidenceItem.
auto program = parse_script(scenario_text);
apply_script(engine, program.value());

auto classification = engine.classify_flow(flow_id, clock.now());
auto localization = engine.localize(request);
auto explanation = engine.explain(explain_request);
```

The complete worked examples are `examples/observe_and_explain.cpp`,
`examples/persist_and_recover.cpp`, and `examples/export_and_reparse.cpp`.

### The scenario language

One command per line; `#` starts a comment; every key is validated, so a typo
is an error rather than a silent default.

```
source id=s1 name=leaf-a kind=counter-telemetry authority=primary
incarnation source=s1 epoch=E1 name=boot-1 at=2026-01-01T00:00:00Z active=true
path id=p1 kind=synthetic hops=n1:0->n2:0,n2:0->n3:0
queue id=q1 node=n2 port=0 priority=3 kind=egress-port-queue
flow id=f1 src=n1:0 dst=n3:0 proto=tcp path=p1 gen=7
counter source=s1 epoch=E1 gen=7 seq=1 counter=c1 scope=dropped-packets value=100 bits=32 queue=q1
counter source=s1 epoch=E1 gen=7 seq=2 counter=c1 scope=dropped-packets value=175 bits=32 queue=q1
op classify flow=f1 at=+1s
op localize flow=f1 gran=queue
op export scope=evidence out=evidence.txt
```

## The command line tool

```
locctl version
locctl run      --store DIR --script FILE [--now TIME] [--max-age SECONDS] [--quiet]
locctl classify --store DIR --flow ID [--at TIME] [--now TIME] [--out FILE]
locctl localize --store DIR --flow ID [--gran GRANULARITY] [--out FILE]
locctl history  --store DIR [--flow ID] [--out FILE]
locctl explain  --store DIR --flow ID [--out FILE]
locctl export   --store DIR --out FILE [--scope SCOPE] [--format FORMAT]
```

Each invocation is an independent operating-system process that shares nothing
with any other except the on-disk journal and the canonical line format. That
is exactly how the end-to-end tests exercise restart behaviour.

## Guarantees, and how they are proven

| Guarantee | Evidence |
| --- | --- |
| Stale evidence can never prove current loss | `proof.stale_evidence_never_proves_current_loss`, plus the property suite over seeded scenarios |
| Discontinuities are explicit and are never loss | `proof.discontinuities_are_explicit`, counter unit tests for reset, wrap, unresolved decrease, out-of-order, epoch change, generation change, and implausible jumps |
| Disagreement yields conflict or unknown, never a guess | `proof.disagreement_yields_conflict`, conflict unit tests including equal-authority ties and retained losing claims |
| Localization never exceeds evidence granularity | `proof.localization_never_exceeds_evidence_granularity`, plus the runtime invariant checked inside the localizer |
| A restart does not revive old loss as current | `proof.restart_does_not_revive_old_loss`, eight-session restart hardening test, cross-process end-to-end test |
| Absence of evidence is never positive evidence | `proof.absence_of_evidence_is_not_loss` |
| Deterministic outputs | `proof.determinism_under_concurrent_ingest`, plus order-invariance properties over permuted inputs |
| Bounded behaviour is reported, never hidden | `proof.bounded_aggregation_reports_truncation`, bound note assertions across store, history, aggregation, export, and script limits |
| Persistence integrity | `proof.persistence_integrity_check`, plus a mutation fuzzer over the journal and a random-file recovery test |
| Independent-process behaviour | `proof.independent_process_round_trip` |

All fifteen CTest entries pass in Debug, Release, and AddressSanitizer builds.
No test uses a timeout.

## Building and testing

| Setting | Value |
| --- | --- |
| Language | C++20 |
| Verified toolchain | MSVC 19.44 (Visual Studio 2022 Build Tools), Windows SDK 10.0.26100, CMake 4.3 |
| Warnings | `/W4 /WX /permissive-` for every first-party target; first-party warning count is zero |
| Sanitizers | AddressSanitizer via `-DLO_ENABLE_ASAN=ON`; the whole tree is instrumented uniformly |
| Dependencies | None beyond the C++20 standard library and a threads implementation |

## Installing and consuming

```powershell
cmake --install build --config Release --prefix C:\sdk\loss-observatory
```

```cmake
find_package(LossObservatory 1.0 REQUIRED)
target_link_libraries(my_target PRIVATE LossObservatory::loss_observatory)
```

`examples/downstream` is a complete consumer that is not part of this build
tree and is configured only through `find_package`. `scripts/verify-package.ps1`
installs the package, builds that consumer against the installed tree, and runs
it.

## Benchmarks

```powershell
.\build\bench\Release\lo_bench.exe --quick
```

Each phase runs a fixed number of complete operations and reports the number it
finished, so a partial run cannot be mistaken for a fast one. Inputs are
generated; the benchmark makes no hardware claim.

## Architecture

The runtime is a layered pipeline. Raw evidence enters only through one door,
is normalised exactly once, and everything downstream consumes the normalised
form.

```
script / API
     |
     v
EvidenceStore        validate, identify, fence epochs and sequences, de-duplicate, bound
     |
     v
derive_observations  pure function: canonical order in, normalized observations out
     |               (counter deltas with wrap/reset handling, probes, sequence gaps,
     |                endpoint pairs, each with validity and discontinuity kind)
     v
freshness            evaluated at query time, never stored as truth
     |
     v
claims + conflict    per-source roll-up, deterministic authority-based resolution
     |
     v
classification       fixed rule ladder producing one loss class plus reasons
     |
     +--> localization   hop/link/queue placement bound by evidence granularity
     +--> episodes       bounded history with lifecycle states
     +--> aggregation    bounded time buckets with explicit saturation
     +--> explanation    deterministic text
     +--> export         canonical line format, re-parseable
     |
     v
persistence          CRC-checked append-only journal, conservative recovery
```

See `docs/ARCHITECTURE.md` for the full design, `docs/EVIDENCE_MODEL.md` for
what each field means, `docs/CONCURRENCY.md` for the locking and shutdown
model, `docs/PERSISTENCE.md` for the on-disk format, `docs/OPERATIONS.md` for
deployment guidance, and `docs/REAL_SYNTHETIC_UNSUPPORTED.md` for exactly what
has and has not been demonstrated.

## Explicit boundaries

**Owned.** Observation, attribution, localization, evidence lifecycle,
freshness and generation correlation, conflict handling, bounded aggregation,
episode history, persistence, and deterministic explanation and export.

**Not owned.** Path repair, rerouting, traffic engineering, congestion control,
blackhole declaration, and any inference that a missing measurement is
evidence of loss.

**Not claimed.** No switch, ASIC, RDMA, InfiniBand, NVLink, multi-host, or
telemetry-source integration has been tested. Every test, example, benchmark,
and fixture in this repository uses SYNTHETIC evidence generated by the
repository itself. `docs/REAL_SYNTHETIC_UNSUPPORTED.md` states this precisely
and per capability.

## Repository layout

```
include/loss_observatory/   public headers, one directory per layer
src/                        implementation
tools/locctl/               the inspection and scripting tool
tests/                      unit, integration, invariants, concurrency, e2e
examples/                   worked examples and the downstream consumer
bench/                      completed-work benchmarks
docs/                       architecture, evidence model, boundaries, operations
scripts/                    build, test, and package-verification helpers
```

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
