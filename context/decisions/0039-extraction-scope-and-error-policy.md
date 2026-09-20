---
type: Decision
title: Extraction scope and error policy (issue #80)
decision_status: accepted
tags: [decision, backend, extraction, error, mpegts, phase-5]
generated:
  by: claude/sonnet-5
  at: 2026-09-20T00:00:00Z
sources:
  - id: libmisbklv-80
    resource: https://github.com/nitsuga/libmisbklv/issues/80
    title: Extraction policy - foreign UL, framing errors, second PID, live 0x15, RP 217 fragmentation, PTS wrap
fork: 35
---

# Context

Review of the extraction paths (issue #80) raised policy questions that the
earlier ADRs left open: what a foreign UL, a framing error, a second KLV PID, a
live `0x15` stream, a fragmented RP 217 cell, and a 33-bit PTS wrap should do.
Its "how does the framer resync" half was fixed independently in PR #84 (see the
amendment in [`0026`](./0026-bounded-live-klv-reassembly.md)); this ADR settles
the policy on top of it and resolves the open questions left in
[`0027`](./0027-high-level-streaming-errors.md) and
[`0016`](./0016-ts-0x15-extraction.md).

# Decision

1. **Foreign UL: fail closed.** An unregistered UL is an error:
   `Message::parse`/`adopt` return `UnknownTag` and `KlvStream` ends with it.
   Deployments that need custom ULs extend the registry.
2. **Framing errors stay terminal.** `extract_ts_klv` and the GStreamer
   extraction stop at the first framing error and return it (ADRs 0026/0027),
   although `KlvFramer` resyncs internally by skipping one byte. Known tradeoff:
   one corrupted length byte ends a live session, and a one-byte skip after a
   `ResourceLimit` rejection can scan inside the rejected payload and emit a
   bogus packet if `06 0E 2B 34` appears there. Accepted and documented in
   `klv_framer.hpp` (`ponytail:`).
3. **Single KLV PID.** `extract_ts_klv` selects the first UL-bearing PID by
   content; the GStreamer backend links only the first `meta/x-klv` pad. A
   second KLV PID is ignored. Multi-PID would need a `pid` on `KlvPacket`.
4. **Live `0x15` is not supported.** `tsdemux` does not expose `stream_type
   0x15` as `meta/x-klv` (finding in [`backend-scope`](../backend-scope.md),
   observed on GStreamer 1.24.2; behavior is version-dependent). Offline
   `extract_ts_klv` reads both `0x06` and `0x15`.
5. **Fragmented RP 217 cells are rejected.** For `stream_id 0xFC`,
   `extract_ts_klv` checks `cell_fragmentation_indication` (the top two bits of
   the third cell-header byte; `0b11` is one complete cell, as in ADR 0016 and
   the fixture generator's `0xDF`). Any other value on the selected PID fails
   with `Error::Unsupported` through the same first-error path as other framing
   failures; packets already delivered stay delivered.
6. **33-bit PTS wrap: limit documented, no code change.** A capture crossing
   the wrap (about every 26.5 h) gets `extract_ts_klv` timestamps about 26.5 h
   off, since the origin is the minimum PTS. Workaround: KLV Item 2 (Precision
   Time Stamp), or split the file. The live path's behavior across the wrap is
   not specified (no explicit handling in `src/gst/`; segment behavior is not
   verified). Upgrade path is in a `ponytail:` comment at `earliest_pts_90k`.
7. **Real-capture coverage stays synthetic-only**, per
   [`0028`](./0028-hermetic-synthetic-fixtures.md).

# Alternatives considered

- **Skip-and-continue / raw-packet surfacing for a foreign UL** — needs an API
  decision on how the skipped packet is exposed. Deferred.
- **Non-terminal opt-in `ExtractOptions` flag for framing errors** — adds API
  surface for a case no deployment has reported. Deferred.
- **Multi-PID extraction** — requires a `pid` on `KlvPacket`. Deferred.
- **Reassemble fragmented RP 217 cells** — no sample needs it; silent
  mis-framing was the risk, so an explicit refusal is enough. Deferred.
- **Unroll PTS across the wrap** — needs a false-positive guard for reordered
  PES; not worth it before a real capture hits it. Deferred.

# Consequences

- Behavior on bad input is explicit and testable: `UnknownTag`, the first
  framing error, or `Unsupported` for a fragmented cell.
- One corrupted length byte ends a live session; callers that must survive it
  restart the stream.
- Live capability is a strict subset of offline: `0x15` is offline-only.

# Assumptions / open questions

- Upgrade paths, only if a deployment hits the ceiling: skip by declared
  length after a rejection, non-terminal extraction as an opt-in, a `pid` on
  `KlvPacket`, PTS unrolling.
- The meaning of the non-`0b11` indication values is not in `references/`;
  every value other than `0b11` is treated as fragmented.

# Citations

[1] [`0016`](./0016-ts-0x15-extraction.md), [`0026`](./0026-bounded-live-klv-reassembly.md),
    [`0027`](./0027-high-level-streaming-errors.md),
    [`0028`](./0028-hermetic-synthetic-fixtures.md).
[2] [`backend-scope`](../backend-scope.md) — the `0x06`-vs-`0x15` finding.
[^libmisbklv-80]: https://github.com/nitsuga/libmisbklv/issues/80
