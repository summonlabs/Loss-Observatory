# Persistence

## Format

One append-only journal per store instance: `<directory>/<instance>.loj`.

```
record := prefix(20) crc(4) payload
prefix := magic(4) type(2) flags(2) length(4) sequence(8)
```

* `magic` is `LOJ1`; anything else stops recovery.
* `length` is a little-endian `uint32`; a length above the configured payload
  bound stops recovery.
* `crc` is CRC-32C over the twenty prefix bytes followed by the payload.
* `sequence` is the write ordinal within the current stream.

The first record must be a manifest. Payloads use a little-endian,
bounds-checked, length-prefixed encoding; every decoder refuses trailing bytes,
out-of-range enumerators, and oversized strings.

| Record | Payload |
| --- | --- |
| `manifest` | format version, semantics revision, product version, creation instant, instance name |
| `session` | engine epoch id, boot instant, incarnation name |
| `topology-entity` | one link, path (with explicit hop identities), queue, or flow binding |
| `source-descriptor` | source identity, name, kind, authority |
| `source-incarnation` | source, epoch, name, activation instant, retired flag |
| `evidence` | one complete evidence item with its provenance header |
| `episode` | one episode record with its evidence and source references |
| `compaction-marker` | written last by a compaction pass |

## Conservative recovery

Recovery reads records in order and stops at the first problem. It never
resynchronises, never scans for a later magic number, and never interprets bytes
it cannot verify.

| Condition | Result |
| --- | --- |
| No journal | Reported as "starting from declared state"; not an error |
| Short header or short payload | `truncated`, recovery stops |
| Magic mismatch | `truncated`, recovery stops |
| Length above the bound | `truncated`, recovery stops |
| CRC mismatch | `checksum_failure` and `truncated`, the record is counted as discarded, recovery stops |
| Manifest version mismatch | `version_mismatch`, nothing is loaded |
| Manifest semantics mismatch | `semantics_mismatch`, nothing is loaded |
| Undecodable entity | The record is counted as discarded and reading continues |

Everything read before the stopping point is restored; everything after it is
reported as discarded. The report carries the record and byte counts so an
operator can see exactly how much of the journal survived.

## What recovery restores, and what it refuses to restore

| Loaded as | Why |
| --- | --- |
| Topology and source registration | They are *declarations*, not observations, so they are restored as declared |
| Source incarnations | Restored with their recorded epochs, but **retired**: they belonged to a previous process |
| Evidence | Restored as history, every item marked `recovered_from_persistence` |
| Episodes | Restored as history; anything that was open arrives sealed as `closed-by-restart` |

The consequence is the guarantee this runtime exists to keep: **a restart does
not revive old loss evidence as current**. Recovered evidence is inadmissible
whatever its timestamps say, an interval derived from a recovered baseline is
itself treated as not current, and an episode cannot resume.

Re-establishing a live source after a restart is an explicit operator action,
expressed in the scenario language as `incarnation source=... epoch=...
active=true`. The runtime never takes that decision on its own.

## Growth and compaction

The journal grows with declarations, evidence, and episode evolution. When it
reaches `max_journal_bytes`, the engine compacts: live state is written to a
temporary file, flushed, and swapped in with a rename, after which the stream is
reopened in append mode.

Compaction is the bound on growth, not a space optimisation: repeated
declaration snapshots and superseded episode records collapse to one record per
live entity. The hardening suite runs eight restart sessions with a 64 KiB limit
and asserts the journal stays inside twice that bound while every session's
conclusions remain correct.

If the swap fails, the original journal is untouched and is reopened, so a
failed compaction never costs data.

## Versioning

Two version numbers matter:

* `kPersistFormatVersion` — the on-disk encoding. A mismatch refuses the load.
* `kSemanticsRevision` — the classification, attribution, and localization
  rules. A mismatch refuses the load, because persisted explanations would not
  be interpretable under different rules.

Both are recorded in the manifest and reported by `locctl version` and by
`full_version_string()`.

## Reading the format without the library

`encode_evidence` and the other encoders are public, and the framing is
described above, so a downstream tool can read a journal without linking the
store. `ExportFormat::Binary` uses a simpler frame for the same payloads:
`length(4) crc(4) payload`.
