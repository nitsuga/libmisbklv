---
type: Decision
title: No klvpmtrewrite — stock mpegtsmux emits 0x06+KLVA
decision_status: accepted
tags: [decision, backend, gstreamer, insertion, phase-3]
generated:
  by: claude/opus-5
  at: 2026-07-19T02:00:00Z
fork: 13
---

# Context

Fork 13 (backend F-C) was to decide the **form** of the `klvpmtrewrite` element.
ADR [`0008`](./0008-media-backend-gstreamer.md) made that element a required v1
deliverable on the premise that **stock `mpegtsmux` does not emit the `KLVA`
registration descriptor**, so the PMT had to be rewritten after mux to add
`KLVA` + `stream_type 0x06`. That premise was true when
[`gstklvplugin`](../prior-art-gstklvplugin.md) was written.

The B2 insertion spike (gstreamer **1.20.3**) tested it directly:
`appsrc(meta/x-klv) ! mpegtsmux ! filesink`, pushing real KLV packets. The output
already carries **`stream_type 0x06` + `registration_id KLVA`**, and the full
**insert → re-extract round-trip is byte-exact** (via the B1 `GstBackend`).

# Decision

**Do not build `klvpmtrewrite`.** v1 insertion is plain
`appsrc ! mpegtsmux ! sink` — modern `mpegtsmux` (gstreamer ≥ 1.20) signals
`0x06`+KLVA natively. This **supersedes the `klvpmtrewrite` requirement in ADR
[`0008`](./0008-media-backend-gstreamer.md)**; the rest of 0008 (gstreamer,
library-style, `MediaBackend` interface, real-time via `appsrc`) stands.

# Alternatives considered

- **Build `klvpmtrewrite` anyway** (as 0008 specified) — rejected: unnecessary
  complexity (a `GstElement` + `gstreamer-mpegts` PMT surgery) for zero benefit
  on gst ≥ 1.20; verified redundant by the round-trip.
- **Rewrite the PMT via a pad probe** — same: solves a non-problem here.

# Consequences

- **B2 collapses** to a thin `GstInserter` (done): `appsrc ! mpegtsmux ! sink`,
  push blocks on backpressure, `finish()` drains. The single biggest planned
  backend risk is removed.
- **Minimum gstreamer bumps to ≥ 1.20** for insertion signaling. Documented in
  [`backend-scope`](../backend-scope.md). If a target must ship an
  older mpegtsmux that omits KLVA, `klvpmtrewrite` returns as a follow-on (the
  interface — [`0013`](./0013-media-backend-interface.md) — is unaffected).
- The `gstreamer-mpegts-1.0` dev lib is no longer required for v1 insertion (was
  only for the PMT rewrite); keep it noted for a possible future 0x15/sync path.

# Assumptions / open questions

- Verified on 1.20.3 with the `Day Flight` 6-packet stream; assume it holds for
  larger streams (mpegtsmux carries the KLV PES payload verbatim). Confirm on a
  bigger sample if a doubt arises.
- Real-time (`udpsink`/`srtsink`, B4) uses the same mux; signaling is identical.

# Citations

[1] [`0008`](./0008-media-backend-gstreamer.md) — the superseded `klvpmtrewrite`
    requirement; the rest stands.
[2] [`0013`](./0013-media-backend-interface.md) — the interface `GstInserter`
    implements.
[3] [`backend-scope`](../backend-scope.md) — B2 spike evidence.
[4] [`gstklvplugin`](../prior-art-gstklvplugin.md) — the `tspmtrewrite` reference
    that a modern `mpegtsmux` makes unnecessary.
