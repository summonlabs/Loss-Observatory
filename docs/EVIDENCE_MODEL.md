# Evidence model

Every observation in this runtime answers six questions before it is allowed to
influence anything: *what* was observed, *by which source*, *for which
generation*, *at what observation and receive times*, *under which source
incarnation*, and *with what provenance and freshness*. If any answer is
missing, the observation is either refused at ingest or carries an explicit
validity that keeps it out of loss conclusions.

## Identities

Each identity is a distinct C++ type wrapping a 64-bit value derived
deterministically from canonical text or supplied literally as sixteen
hexadecimal digits. There is no conversion between identity kinds.

| Identity | Names |
| --- | --- |
| `NodeId`, `PortId`, `PriorityId`, `ProtocolId` | fabric endpoints and attributes |
| `LinkId`, `HopId`, `PathId`, `QueueId` | declared topology entities |
| `FlowId` | a flow binding |
| `SourceId`, `EpochId` | a telemetry source and one activation of it |
| `GenerationId`, `RevisionId` | binding generation and topology revision |
| `MeasurementId`, `SequenceId` | one observation, and its position in a source's stream |
| `EpisodeId`, `WindowId`, `ClaimId`, `ProbeId`, `CounterId` | history and evidence sub-identities |

`SubjectRef` is the type-erased reference used by queries. It keeps the
identity class, so a link can never be silently interpreted as a queue, and it
offers checked accessors that fail rather than convert.

## Granularity

```
Unknown < Flow < Path < Hop < Link < Queue
```

Larger means finer. The lattice is the backbone of every "never claim more than
you can see" rule:

* a measurement method declares the finest granularity it can ever support, and
  evidence claiming more is refused at ingest;
* a segment in a localization result is never finer than the evidence behind
  it, and never finer than the caller's cap;
* a flow-scoped or path-scoped observation is never attributed to an individual
  hop.

## Provenance header

Every evidence item carries:

| Field | Meaning |
| --- | --- |
| `id` | measurement identity, de-duplicated at ingest |
| `source`, `epoch` | which source, and which activation of it |
| `generation` | the binding generation the source observed |
| `topology_revision` | the path or binding revision the source observed |
| `source_sequence` | monotonic position within the source epoch |
| `observed_at`, `received_at` | distinct instants; a delayed delivery cannot make old evidence new |
| `method` | how the number was obtained |
| `subject`, `granularity` | what entity, and how precisely |
| `note` | bounded free text |
| `recovered_from_persistence` | set by recovery; makes the item inadmissible |

## Measurement methods

| Method | Finest granularity | Requires | Implemented |
| --- | --- | --- | --- |
| `counter-delta` | queue | — | yes |
| `probe-round-trip` | path | — | yes |
| `probe-one-way` | link | synchronised clocks | **no** |
| `sequence-gap` | flow | stable generation | yes |
| `endpoint-comparison` | flow | stable generation | yes |
| `synthetic-injection` | queue | — | yes, and labelled synthetic |

An unimplemented method never degrades into another one. Evidence that uses it
is either refused at ingest or produces `unsupported-method`.

## Counter semantics

A counter delta is derived from a deterministically ordered series, never from
arrival order. The outcome is one of:

| State | Meaning |
| --- | --- |
| `valid` | monotonic and plausible |
| `wrap-detected` | decreased near the top of a declared width; the wrapped value is reported but **not** accepted as loss by default |
| `reset-detected` | decreased far from the top of the declared range |
| `discontinuity-unresolved` | decreased with no declared width, so neither wrap nor reset is proven |
| `out-of-order` | the second reading is older by sequence or by time |
| `epoch-changed`, `generation-changed` | the readings are not comparable |
| `counter-changed` | identity, scope, or binding changed between readings |
| `implausible-delta` | above the configured plausibility threshold |
| `missing-baseline` | a single reading cannot produce a delta |

Only `valid` contributes to a loss total, and then only according to the
counter's scope:

* **direct loss scopes** (dropped, discarded, error) report the magnitude
  directly with the ratio left undefined, because there is no denominator;
* **throughput scopes** (received, transmitted, byte counters) are recorded as
  valid observations that are simply not loss evidence. A lone throughput
  counter carries no expectation to compare against, and this runtime will not
  invent one.

## Validity versus freshness

These are different questions and are kept separate.

**Validity** is a property of the evidence itself: is the arithmetic sound, is
the payload plausible, does the method apply. It is decided during derivation.

**Freshness** is a property of the evidence *at the moment it is evaluated*:
how old is it, is the citing incarnation still live, has the generation been
superseded, has the topology revision moved on, did it come back from disk. It
is decided at query time and is never stored as a truth.

An observation can be perfectly valid and completely stale. Only fresh and
valid evidence can support a loss conclusion.

## Freshness classes and reasons

| Class | Admissible | Typical reason |
| --- | --- | --- |
| `fresh` | yes | within the age window, live incarnation, current generation and revision |
| `stale` | no | age exceeded, retired or unknown epoch, superseded or unknown generation, changed topology revision, receive before observe |
| `not-current` | no | recovered from persistence during this boot |
| `future-dated` | no | observation instant ahead of local time beyond the skew tolerance |
| `undated` | no | no usable observation instant |
| `unknown` | no | the source is not registered |

## Loss classes

| Class | Asserts |
| --- | --- |
| `absent-evidence` | nothing at all |
| `no-loss-observed` | fresh, adequately covered evidence shows zero loss |
| `confirmed-loss` | fresh evidence directly shows loss |
| `suspected-loss` | something loss-shaped, coverage or magnitude insufficient |
| `discontinuity` | a reset, wrap, or restart occurred; explicitly not loss |
| `stale-evidence` | nothing current; explicitly not an absence of loss |
| `conflicting-evidence` | admissible sources disagree and no source outranks them |
| `incomplete-evidence` | coverage is partial |
| `unsupported-method` | the method is declared but not implemented, or its preconditions are unmet |
| `implausible-evidence` | the evidence is internally impossible |
| `unknown` | the inputs do not determine an answer |

## Attribution

Confidence is an integer in `[0, 100]` accumulated from named terms — fresh
observations, coverage behind a ratio, direct-loss counter observations,
independent source agreement, authority-resolved conflict, stale or recovered
observations, discontinuities, unsupported methods, and bound truncation. Each
term appears in the explanation with its points and its reason. The band is a
fixed function of the score.
