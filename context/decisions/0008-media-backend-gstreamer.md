---
type: Decision
title: Media backend — gstreamer (v1)
decision_status: accepted
tags: [decision, architecture, backend, gstreamer, phase-2]
generated:
  by: claude/opus-5
  at: 2026-07-17T20:30:00Z
fork: 5
---

# Context

Fork 5. Layer 4 of the architecture ([`0005`](./0005-klv-core-data-model.md)). The
project was originally "configurable gstreamer or ffmpeg"; deliberation
narrowed it to a **single backend for v1**. Both backends can extract + insert
(including real-time); the user prioritizes **real-time insertion flow control
+ Jetson multimedia alignment**, and accepts that gstreamer insertion requires
a PMT-rewrite component (the cost ffmpeg would have avoided natively).
ffmpeg is deferred/optional, not built in v1. See
[`gstklvplugin`](../prior-art-gstklvplugin.md) (the reference) and
[`data-samples`](../data-samples.md) (`0x06`+`KLVA` signaling).

# Decision

- **Single backend for v1: gstreamer**, **library-style** (link gstreamer libs,
  drive pipelines programmatically — *not* ship as plugins).
- **`MediaBackend` interface kept** despite one implementation — keeps the KLV
  core backend-agnostic, enables a mock/in-memory backend for unit tests, and
  leaves the door open for ffmpeg later without core changes.
- **Extraction + insertion in v1, including real-time (stream) insertion.** The
  interface's `insert()` takes a **stream sink** (file *or* live socket), not
  just a file.
- **Extraction:** `tsdemux` finds the KLV PID (by the `KLVA` registration
  descriptor, per [`data-samples`](../data-samples.md)) and yields KLV packets
  to the core. Accepts both `0x06` and `0x15` stream types.
- **Insertion (file + real-time):** `appsrc` → `mpegtsmux` → `klvpmtrewrite`
  (in-library element) → `filesink` / `udpsink` / `srtsink`. Real-time via
  `appsrc` — **automatic flow control** (`need-data` / `enough-data`,
  `is-live`, backpressure + clock pacing), so the library's push-KLV API can
  block on pipeline flow (no manual pacing) — the real-time API ergonomic win.
- **v1 real-time mode: asynchronous (`0x06`).** Metadata paces at its own
  cadence via `appsrc` and carries its own Precision Time Stamp (matches our
  samples). **Sync per-frame (`0x15`, the `klvframeinject` pattern,
  frame-accurate) is deferred as a follow-on** — the PMT-rewrite supports both
  signaling modes, so adding sync later is non-breaking.
- **PMT-rewrite: an in-library gstreamer element, registered in-process** (option
  (c)) via `gst_element_register` at library init — internal plumbing used only
  in libmisbklv's own pipelines, **not a shipped plugin**. Stock `mpegtsmux`
  does *not* emit `KLVA`; the element rewrites the PMT after mux to add the
  `KLVA` registration descriptor + correct `stream_type` (`0x06` async / `0x15`
  sync), porting [`gstklvplugin`](../prior-art-gstklvplugin.md)'s `tspmtrewrite`
  approach. On insert, emit `0x06`+`KLVA` (matches our samples). Idiomatic
  gstreamer composition (a pipeline stage) while staying library-style.
- **ffmpeg:** deferred/optional — not built in v1; the interface allows adding
  it later without core changes. Revisit if a consumer can't take a gstreamer
  dependency.

# Alternatives considered

- **Both backends in v1** — rejected: not required (user); double the work;
  gstreamer alone covers both directions.
- **ffmpeg-only (v1)** — rejected by user preference: ffmpeg gets insertion
  signaling natively (no PMT-rewrite) and is a lighter dependency, but lacks
  gstreamer's automatic real-time flow control (`appsrc`) and Jetson multimedia
  alignment — both prioritized for the real-time insertion library API.
- **gstreamer-only, no abstract interface** — rejected: keep the interface for
  a backend-agnostic core + testability + a future-ffmpeg option.
- **Ship gstreamer plugins** (gstklvplugin-style) — rejected: libmisbklv is a
  library ([`0001`](./0001-build-system-and-cpp-standard.md),
  [`0003`](./0003-project-name.md)); library-style is consistent. Plugins remain
  a possible later artifact.
- **PMT-rewrite as in-process byte manipulation (option (a))** — rejected in
  favor of (c): byte-fiddling beside the pipeline is less gstreamer-idiomatic
  than a composable in-pipeline element.
- **Sync per-frame (`0x15`) as the v1 real-time mode** — deferred: more work
  (tapping video frames to pace KLV); async (`0x06`) satisfies "insertion in v1"
  and matches our samples; sync added later, non-breaking.

# Update (2026-07-19)

The `klvpmtrewrite` requirement below is **superseded by
[`0015`](./0015-no-pmt-rewrite.md)**: stock `mpegtsmux` (gstreamer ≥ 1.20)
already emits `stream_type 0x06` + `KLVA`, verified by a byte-exact insertion
round-trip, so no PMT-rewrite element is built. Everything else in this ADR
stands.

# Update (2026-07-31)

The original extraction statement is refined to describe the implementation:
`GstBackend` uses `tsdemux` and extracts `stream_type` 0x06 KLV. The gst-free
whole-buffer `extract_ts_klv` path handles both 0x06 and 0x15 offline; live 0x15
extraction remains deferred. This corrects the current extraction boundary only.

# Consequences

- v1 ships a gstreamer backend doing extract + insert (file + real-time,
  async).
- The **PMT-rewrite is an in-library gstreamer element** (a required v1
  deliverable, porting gstklvplugin's `tspmtrewrite`) — the main insertion
  cost; bounded, with a known reference.
- Project description narrows from "gstreamer or ffmpeg" to "gstreamer (v1);
  ffmpeg optional later" — `CLAUDE.md` / `README` reconciled on accept.
- gstreamer is a heavier runtime dependency (plugin discovery, pipeline
  construction, the gst runtime); the aarch64/Jetson gstreamer sysroot must be
  available for cross-compile ([`0001`](./0001-build-system-and-cpp-standard.md)).
- Real-time insertion API ergonomics: `appsrc` backpressure lets the library's
  push API block on flow control (no manual pacing) — a genuine advantage.
- Sync per-frame (`0x15`) insertion deferred — frame-accurate metadata is a
  follow-on, not v1.

# Assumptions / open questions

- **Resolved:** v1 real-time mode = async (`0x06`); sync per-frame (`0x15`)
  deferred. PMT-rewrite shape = in-library gstreamer element (option (c)).
- **Open (non-blocking):** Jetson gstreamer aarch64 sysroot availability for
  cross-compile — confirm when the cross-build is set up.

# Citations

[1] [`gstklvplugin`](../prior-art-gstklvplugin.md) — `tspmtrewrite`,
    `klvframeinject`, `appsrc`/pipeline patterns (the reference).
[2] [`data-samples`](../data-samples.md) — `0x06`+`KLVA` signaling; KLV PID
    identification by `KLVA` registration descriptor.
[3] [`0005`](./0005-klv-core-data-model.md) — Layer 4 / backend boundary.
[4] [`0001`](./0001-build-system-and-cpp-standard.md) — cross-compile / Jetson.
[5] [`0003`](./0003-project-name.md) — library (not plugin); C ABI deferred.
