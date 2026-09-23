# Architecture

## Layering

The runtime is a strict pipeline. Each layer consumes only the layer beneath
it, and no layer reaches around another.

| Layer | Headers | Responsibility |
| --- | --- | --- |
| Core | `core/*.hpp` | Status and Result, checked arithmetic, stable identities, time, byte encoding, CRC-32C, bounded containers |
| Model | `model/*.hpp` | Typed identities, granularity lattice, declared topology, source registration and incarnation epochs, measurement-method semantics, generation correlation |
| Evidence | `evidence/*.hpp` | Raw payload types, the provenance header, counter delta derivation, normalisation into observations, the fenced evidence store |
| Analysis | `freshness.hpp`, `classify.hpp`, `conflict.hpp`, `localize.hpp` | Freshness, per-source claims, conflict resolution, the classification ladder, attribution, localization |
| Memory | `episode.hpp`, `aggregate.hpp` | Bounded episode history and bounded time-bucket aggregation |
| Durability | `persist.hpp` | Versioned CRC-checked journal, conservative recovery, compaction |
| Runtime | `engine.hpp`, `report.hpp`, `script.hpp` | The engine, deterministic rendering, and the scenario language |

## Evidence flow

1. **Ingest.** `EvidenceStore::submit` is the only door. It validates the
   structure, checks that the measurement method is implemented and can support
   the claimed granularity, consults the source registry for the incarnation
   epoch, applies the sequence fence, de-duplicates the measurement identity,
   and only then stores the item.
2. **Derivation.** `derive_observations` is a pure function over the canonically
   ordered evidence set. It groups counter readings into series, derives the
   deltas with wrap and reset handling, evaluates probes, sequence gaps, and
   endpoint pairs, and emits `LossObservation` values with an explicit validity
   and discontinuity kind. Because the function sorts its input, arrival order
   cannot change its output.
3. **Freshness.** `assess_freshness` runs at query time against an explicit
   instant. Recovery from persistence, age, future dating, retired
   incarnations, superseded generations, and changed topology revisions are all
   decided here, and only a fresh assessment is admissible.
4. **Classification.** `Classifier::classify` rolls admissible observations into
   per-source claims, runs conflict detection, then walks a fixed rule ladder
   that can only ever narrow the conclusion.
5. **Placement.** `Localizer::localize` attaches conclusions to path entities, or
   refuses to, according to the granularity the evidence supports.
6. **Memory and durability.** Episodes and aggregates record history; the
   journal records everything that must survive a restart.

## Determinism

Deterministic output is a property, not an aspiration:

* every derived observation is a pure function of the canonically ordered
  evidence set, and canonical order is `(observed time, source, source sequence,
  measurement id)`;
* the generation current at an instant is a pure function of the recorded
  generation windows, so arrival order cannot change it;
* seeds and sequence numbers come from the caller, never from a hidden global;
* confidence is an integer sum of named terms;
* every rendered form iterates in a fixed order.

The engine exposes this directly: `proof.determinism_under_concurrent_ingest`
ingests the same evidence set through four workers in three different shuffled
orders and requires byte-identical classifications, and the property suite
permutes derived inputs and requires identical observations.

## What the runtime refuses to do

* **It never infers loss from silence.** No evidence produces
  `absent-evidence`, which asserts nothing.
* **It never names a hop it cannot see.** A flow-scoped observation produces a
  flow-scoped answer and an explicit ambiguity note.
* **It never repairs an ambiguity.** Two equally authoritative disagreeing
  sources produce `conflicting-evidence`, with both claims retained.
* **It never invents a denominator.** A direct drop counter reports magnitude
  with the ratio left undefined rather than dividing by a guess.
* **It never treats a discontinuity as loss.** A counter reset, a wrap, an
  unresolved decrease, an epoch change, a generation change, and an implausible
  jump are all reported as discontinuities, and a discontinuity contributes
  nothing to a loss total.
* **It never promotes recovered evidence.** Anything read back from disk is
  marked recovered and is inadmissible until a live source re-observes it.
* **It never lets a coarse observation become a fine claim.** Flow and path
  observations contribute to the aggregate and to nothing finer.

## Bounds

Every externally driven collection has a limit, and every limit that bites is
reported through a `BoundNote` naming the bound, the limit, what was observed,
and the subject. The limits cover: evidence items, evidence per subject,
distinct subjects, distinct sources, counter series and points per series,
topology entities, hops per path, open episodes, episodes in total, query
results, aggregation windows, aggregation sources, evidence references per
bucket, explanation lines, export items, journal bytes, journal record payloads,
and script lines, bytes, fields, and commands.

Externally derived sizes use checked arithmetic throughout; the fallback for a
size that cannot be represented is an explicit refusal, never truncation.

## Files

```
include/loss_observatory/version.hpp            version and proof-surface facts
include/loss_observatory/core/status.hpp        StatusCode, Status, Result, UpsertOutcome
include/loss_observatory/core/checked.hpp       checked add/sub/mul/div/narrow, saturating sum, basis points
include/loss_observatory/core/hash.hpp          FNV-1a, splitmix64, the Id<Tag> template
include/loss_observatory/core/time.hpp          Timestamp, Duration, Clock, ManualClock
include/loss_observatory/core/bytes.hpp         bounds-checked little-endian writer and reader, CRC-32C
include/loss_observatory/core/bounded.hpp       BoundedQueue, RingHistory, BoundNotes, BoundKind
include/loss_observatory/model/ids.hpp          every identity type, SequenceId, SubjectRef
include/loss_observatory/model/granularity.hpp  the Unknown < Flow < Path < Hop < Link < Queue lattice
include/loss_observatory/model/topology.hpp     declared links, paths, hops, queues, flows
include/loss_observatory/model/source.hpp       source descriptors, incarnations, the sequence fence
include/loss_observatory/model/method.hpp       measurement-method semantics
include/loss_observatory/model/generation.hpp   generation correlation over recorded windows
include/loss_observatory/evidence/evidence.hpp  payloads and the provenance header
include/loss_observatory/evidence/counter.hpp   counter delta derivation and its states
include/loss_observatory/evidence/derive.hpp    normalized LossObservation and the derivation pass
include/loss_observatory/evidence/store.hpp     the fenced, bounded evidence store
include/loss_observatory/freshness.hpp          freshness classes, reasons, policy
include/loss_observatory/classify.hpp           loss classes, reason codes, attribution, the classifier
include/loss_observatory/conflict.hpp           disagreement detection and resolution
include/loss_observatory/localize.hpp           segments, ambiguity, the localizer
include/loss_observatory/episode.hpp            episode records and the tracker
include/loss_observatory/aggregate.hpp          bounded time buckets
include/loss_observatory/persist.hpp            the journal and its encodings
include/loss_observatory/engine.hpp             the runtime
include/loss_observatory/report.hpp             deterministic rendering and export
include/loss_observatory/script.hpp             the scenario language
include/loss_observatory/testkit/testkit.hpp    deterministic generators and invariant checkers
```
