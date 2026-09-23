# Operations

## Deploying a store

A store is a directory plus an instance name. Every command takes both:

```
locctl run --store C:\var\loss-observatory --instance fabric-a --script scenario.txt
```

Give each writer its own `--store` directory. The journal is a single-writer
append-only file; concurrent writers to one file are outside the supported
model.

## Time

Every decision is made against an explicit instant.

* `--now TIME` freezes the engine clock at an ISO-8601 instant. Use it for
  reproducible analysis and for any run whose inputs are dated in the past.
* `--at TIME` sets the instant a query is evaluated at, and accepts either
  ISO-8601 or a relative offset such as `+1500ms`.
* `--max-age SECONDS` widens or narrows the freshness window. The default is
  30 seconds.

Forgetting `--now` on historical data is the most common operational mistake:
the evidence is real, but it is genuinely stale relative to wall-clock time,
and the runtime will say so.

## Freshness and the age window

Choose `max_age` from how fast the telemetry arrives, not from how long you
would like the answer to stay valid. Too wide and old evidence will be treated
as current; too narrow and a quiet period will read as `stale-evidence`.

The runtime never widens the window on its own, and it never treats a gap in
telemetry as an absence of loss.

## Sequence numbers and epochs

A source's `source_sequence` must increase within an incarnation epoch. Two
readings with the same sequence are a replay and are refused; a reading behind
the high-water mark is admitted as history and flagged out-of-order.

When an agent restarts, activate a new epoch. Reusing an epoch across a restart
is an explicit act (`active=true`) and should be reserved for the case where
the counter state genuinely continued.

## Interpreting results

Read the class together with its reasons.

| Class | What to do |
| --- | --- |
| `confirmed-loss` | Loss is established; read the localization and its ambiguity notes before acting |
| `suspected-loss` | Raise coverage: more offered units, or a method with finer granularity |
| `absent-evidence` | Nothing has been observed for this subject. This is not a clean bill of health |
| `stale-evidence` | The subject has not been observed recently, or only recovered evidence exists |
| `discontinuity` | A reset, wrap, or restart happened. Re-baseline rather than reading it as loss |
| `conflicting-evidence` | Two admissible sources disagree and neither outranks the other. Go and look |
| `incomplete-evidence` | Partial coverage, a missing baseline, or an unpaired endpoint report |
| `unsupported-method` | The deployment declared a method this runtime does not implement, or a precondition is unmet |
| `implausible-evidence` | The source reported something impossible. Fix the source |

## Ambiguity notes

A localization result that could not narrow to a single entity says so
explicitly and lists what remains possible. A `flow-wide` segment on a
multi-hop path means the evidence was flow-scoped: report it as "loss on this
flow", never as "loss on hop 3".

An `unobserved-span` segment means those hops had no evidence at all. It is
never a statement that those hops are healthy.

## Bounded behaviour

When a bound is reached the runtime reports it. Watch for bound notes on
evidence items, evidence per subject, distinct subjects, histogram windows,
history episodes, result sets, and journal size. A bound note in a result means
the answer is drawn from partial data, and it should be quoted alongside the
conclusion.

## Export and import

`op export scope=all format=script-text out=FILE` writes a scenario that
re-parses. Evidence lines carry every identity, so an import reproduces the
same conclusions. Two things to know:

* Derived content — aggregates and episodes — is exported as comments, because
  it is recomputed rather than declared.
* Incarnations recovered from disk are exported as `active=false`. Re-importing
  therefore takes an explicit activation line, which is the same step an
  operator takes after a restart.

## Housekeeping

* `locctl explain --flow ID` is the primary diagnostic: it prints the class, the
  freshness summary, every reason, the confidence terms, the per-source claims,
  the localization, the history, and any bound or rejection notes.
* `locctl history` lists episodes; a closed episode's `state_detail` says why it
  closed.
* Delete a store directory to start over. Nothing else holds state.
