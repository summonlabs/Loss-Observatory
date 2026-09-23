# REAL, SYNTHETIC, and UNSUPPORTED proof surfaces

This document states exactly what has been demonstrated and what has not. No
claim in this repository is stronger than the evidence behind it, and no
hardware, fabric, or telemetry-source integration is claimed anywhere.

## Summary

| Surface | Label |
| --- | --- |
| The library, the CLI, the journal format, the scenario language, the classification, conflict, localization, freshness, episode, aggregation, and bounding logic | **REAL** — implemented, exercised, and proven by this repository's own tests |
| Every input used by every test, example, benchmark, and fixture | **SYNTHETIC** — generated deterministically by this repository |
| Switch, ASIC, NIC, RDMA, InfiniBand, NVLink, multi-host, and vendor telemetry integration | **UNSUPPORTED** — no such integration exists or has been tested |

## REAL

The following are real, first-party implementations with real behaviour, real
concurrency, real persistence, and real failure modes:

* **Typed identity and provenance model.** Distinct C++ types, deterministic
  derivation, de-duplication, and a fence that rejects replays, reordered
  readings, stale epochs, unknown sources, and unknown incarnations.
* **Derivation.** Counter deltas with explicit wrap, reset, unresolved,
  out-of-order, epoch-change, generation-change, binding-change, implausible,
  and missing-baseline states; probe, sequence-gap, and endpoint-pair
  evaluation; and the guarantee that derivation is a pure function of the
  canonically ordered evidence set.
* **Freshness.** Evaluated per query, including recovery from disk, retired
  incarnations, superseded generations, and changed topology revisions.
* **Classification, attribution, and conflict handling.** A fixed rule ladder
  producing one of eleven classes, integer confidence from named terms, and
  authority-based conflict resolution that always retains the losing claim.
* **Localization.** Hop, link, and queue placement with a runtime-checked
  invariant that a claim is never finer than its evidence, and retained
  ambiguity everywhere the evidence cannot decide.
* **Bounded memory.** Evidence, history, aggregation, results, payloads, and
  the journal are all bounded, and every bound that bites is reported.
* **Persistence.** A CRC-32C-checked, versioned, append-only journal with
  conservative recovery, session epochs, and compaction.
* **Concurrency.** A bounded multi-producer queue, worker threads, per-batch
  completion state, real cancellation through `std::stop_token`, and a
  documented ownership model with no lock cycle.
* **The command line tool and the scenario language.** Exercised by
  independent-process end-to-end tests.

## SYNTHETIC

Every number that appears in any test, example, benchmark, or fixture in this
repository was produced by this repository. Specifically:

* `loss_observatory::testkit` generates counter series, scenarios, and
  workloads from a seed. A source created by the testkit is registered with
  `SourceKind::Synthetic`, and evidence produced by the synthetic-injection
  method carries `MethodSemantics::synthetic`, so downstream output can always
  identify it.
* The examples print `proof surface: SYNTHETIC`.
* The benchmark prints `proof surface: SYNTHETIC (generated inputs, no hardware
  involved)`.
* The test scenarios use fabricated node, port, queue, and flow names. They
  describe no real device.

This means the runtime's *behaviour* is proven, while its *inputs* are not
measurements of anything real. The distinction matters: this repository
demonstrates that the runtime reasons correctly about evidence it is given, not
that any particular network exhibits any particular loss.

## UNSUPPORTED

None of the following exists in this repository. Nothing here reads from, talks
to, or models a real forwarding plane:

* No switch, router, or fabric operating-system integration of any kind.
* No ASIC, NPU, or programmable-pipeline counter access.
* No NIC, RDMA, RoCE, InfiniBand, or NVLink interaction.
* No multi-host or multi-process distributed measurement; the only
  cross-process behaviour is a command line tool reading and writing local
  files.
* No vendor telemetry source, no gNMI, no streaming telemetry, no SNMP, no
  proprietary counter API.
* No clock synchronisation implementation. A deployment must declare
  `clock_synchronized` if it has one; the runtime does not create one.
* No active probe sender, no packet capture, no socket, and no network I/O of
  any kind. The library links no networking library.
* No path repair, rerouting, traffic engineering, congestion control, or
  blackhole declaration. These are outside the owned boundary.
* No performance claim about real traffic. The benchmarks measure the runtime's
  own completed work on generated inputs.

## Encoding a real deployment

The intended integration path is for a deployment to translate its own
telemetry into `EvidenceItem` values, or into the scenario language, and then
rely on this runtime for everything downstream of ingest. That translation
layer is where hardware-specific knowledge belongs, and it does not exist here.
Until such a layer exists and is tested, any statement about behaviour on real
hardware is unsupported.

## What would change these labels

An integration becomes REAL when it has been run against the real thing and its
behaviour has been observed. For this repository that would mean, at minimum: a
real counter source feeding real readings, a real probe or sequence tracker,
and a demonstration that the freshness, classification, and localization
conclusions match independently observed truth. None of that has been done.
