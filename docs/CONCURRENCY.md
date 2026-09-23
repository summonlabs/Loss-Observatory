# Concurrency, ownership, and shutdown

## Ownership model

The runtime is built from leaf subsystems that each own exactly one mutex. No
subsystem acquires another subsystem's mutex, and no subsystem calls out to
user code while holding one. The engine's own mutex protects only its private
bookkeeping.

| Subsystem | Lock | Guards |
| --- | --- | --- |
| `TopologyRegistry` | `std::shared_mutex` | declared links, paths, queues, flows, nodes, revision |
| `SourceRegistry` | `std::shared_mutex` | source descriptors, incarnations, per-epoch sequence fences |
| `GenerationRegistry` | `std::shared_mutex` | generation windows per entity |
| `EvidenceStore` | `std::mutex` | subject buckets, seen measurement identities, insertion order, statistics |
| `EpisodeTracker` | `std::mutex` | episodes and the open-episode index |
| `BoundedAggregator` | `std::mutex` | time buckets |
| `PersistenceStore` | `std::mutex` | the journal stream and statistics |
| `ObservatoryEngine` | `std::mutex` | aggregated-evidence bookkeeping and the batch waiter registry |
| `BoundedQueue` | `std::mutex` | the queue, its bound, and its statistics |
| per-batch state | `std::mutex` | one batch's completion slots |

Every atomic member is a counter that carries no domain state.

## Lock-reentrancy audit

A subsystem that holds its lock must never call a public method of itself,
because every public method takes the lock. The audit below covers every place
where a locked region calls other code.

| Locked region | Calls | Permitted because |
| --- | --- | --- |
| `PersistenceStore::open_session` | `flush_locked` | the locked variant does not re-take the mutex |
| `PersistenceStore::compact` | `write_record_locked`, stream operations | none of these take the mutex |
| `PersistenceStore::flush` | `flush_locked` | same |
| `EvidenceStore::submit` / `restore` | `record_bound_locked` | private, lock-free helper |
| `EvidenceStore::snapshot` | `canonical_order` | pure function over a local copy |
| `EpisodeTracker::observe` | `evict_locked`, `saturating_add` | private, lock-free helpers |
| `BoundedAggregator::add` | `checked::ratio_basis_points` | pure |
| `ObservatoryEngine::record_aggregates` | `BoundedAggregator::add` | leaf subsystem, never calls back |
| `ObservatoryEngine::record_episode` | `EpisodeTracker::observe`, `find`, `PersistenceStore::append_episode` | leaf subsystems, no callback into the engine |

No path exists in which a subsystem's lock is held while a second subsystem's
lock is acquired and vice versa, so there is no lock cycle. The one nesting
that does occur is engine bookkeeping followed by a single leaf subsystem, and
that order is never reversed.

Consequently there is no reentrancy: no public method of a locked subsystem is
reachable from inside its own critical section.

This audit found one real defect during hardening: `PersistenceStore::open_session`
held the mutex and called `flush()`, which re-took it. The fix was to split the
locked and unlocked flush paths.

## Workers, queues, and cancellation

* The engine runs `worker_count` threads. `worker_count == 0` means ingest
  runs synchronously on the calling thread, which is the default for tests,
  replay, and any use that wants strict ordering.
* Submissions enter a `BoundedQueue` with a configured capacity. A full queue
  blocks the producer until space is available, the queue closes, or the stop
  token fires; it never drops silently.
* Each `ingest_batch` call allocates its own completion state, so several
  callers may have batches in flight without sharing buffers. Workers locate
  the caller's slots through a shared pointer carried in the work item.
* Cancellation uses `std::stop_token`. A blocked producer or consumer wakes and
  returns; it does not wait for work that will never arrive.
* `stop()` performs a graceful drain: it stops accepting new work, lets workers
  finish what is queued, joins them, then requests cancellation. Any slot that
  never ran is filled in with an explicit cancelled outcome, so a caller can
  never mistake "not processed" for "processed".
* `request_stop()` is the abrupt path: it requests cancellation immediately and
  closes the queue.

## Query paths under concurrency

Every query derives a fresh snapshot of the evidence store. Derivation sorts
its input canonically, so the *result* never depends on arrival order or on how
many items were in flight. Classification, localization, explanation, history,
and export may therefore run concurrently with ingestion, and the concurrency
suite exercises exactly that: four reader threads running classification,
localization, explanation, and subject classification while batches are
ingested, with a requirement that no query fails.

## What is not synchronised, and why that is safe

The sequence fence is intentionally stateful and order-dependent: a source's
high-water mark advances as evidence arrives. Two submissions from the same
source arriving in different orders can therefore produce different *fence
decisions* (one may be `reordered` instead of `accepted`). What cannot differ
is the *conclusion*, because both are admitted to the store and derivation
sorts them canonically. The hardening and concurrency suites assert exactly
this: shuffled arrival produces byte-identical classifications.

## Clock ownership

The engine borrows a `const Clock&` and never reads a hidden global clock. The
caller must keep the clock alive for the engine's lifetime; this is documented
at the constructor. Tests that need determinism use `ManualClock`.
