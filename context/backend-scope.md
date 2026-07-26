---
type: Component
title: gstreamer media backend — scope & design
description: Environment findings, component breakdown, and the B0–B4 phased plan for the gstreamer MPEG-TS backend.
tags: [component, backend, gstreamer, mpegts, phase-3]
timestamp: 2026-07-19T06:00:00Z
---

# gstreamer backend — scope

> **Status: implemented.** B0–B4 complete and all scoping forks (F-A–F-D)
> resolved — see the [decided register](./decisions/index.md). This is no longer
> a forward plan; it's retained for the environment findings and the design
> rationale that shaped the backend (`misbklv-gst`).

Scoping for the media backend (ADR [`0008`](./decisions/0008-media-backend-gstreamer.md)):
the layer between the KLV core (parse/build) and MPEG-TS I/O. Library-style
gstreamer (link libs, drive pipelines; not shipped plugins). This doc grounds
that shape in the real environment and records the component breakdown, the
design forks (now resolved), and the phased plan. Empirical probes done
2026-07-19.

## Environment (verified)

- **gstreamer 1.20.3**; `gstreamer-app-1.0` (appsrc/appsink) present; all pipeline
  elements present (`tsdemux`, `tsparse`, `mpegtsmux`, `appsrc/appsink`,
  `filesrc/sink`, `udpsink`, `srtsink`, `souphttpsrc`).
- **`gstreamer-mpegts-1.0` 1.20.3 installed** (`libgstreamer-plugins-bad1.0-dev`)
  — the library for PMT / descriptor manipulation, thought to be needed for a
  KLVA PMT-rewrite. **In the end it wasn't used** — the rewrite proved
  unnecessary (ADR [`0015`](./decisions/0015-no-pmt-rewrite.md)); the built
  backend links only `gstreamer-1.0` + `gstreamer-app-1.0`.
- **python-gi + Gst** available — used for the probes; handy for further spikes.

## Key finding — extraction is two regimes

Probed `tsdemux` pad output across all `data/` samples (via python-gi):

| Signaling | Samples | stock `tsdemux` → `meta/x-klv`? |
|---|---|---|
| `stream_type 0x06` + KLVA reg. descriptor | Day Flight, Night Flight IR, falls | **yes** — `private_… → meta/x-klv, parsed=true` |
| `stream_type 0x15` (metadata) | Cheyenne, klv_metadata_test_sync | **no** — video/audio pads only; KLV silently dropped |

So `tsdemux` recognizes the KLVA registration and exposes **0x06** KLV as
`meta/x-klv` (extraction "just works": `tsdemux ! meta/x-klv ! appsink`), but
does **not** surface **0x15** metadata streams. This revises the earlier
assumption that both are equivalent, and confirms ADR 0008's "accept both on
extract" is a real requirement, not hypothetical. 0x15 ties to the deferred
sync-KLV mode (its samples are the `*_sync` file).

## Components to build

1. **`MediaBackend` interface** — core-facing, gstreamer-agnostic (mock + future
   ffmpeg fit). Extraction (pull KLV units from a source) + insertion (push KLV
   units to a sink, real-time flow-controlled). Shape is **fork F-A**.
2. **`GstBackend` — extraction** — `filesrc|udpsrc|srtsrc|souphttpsrc ! tsdemux !
   meta/x-klv ! appsink`; appsink buffers → `parse_packet`. PTS carried per unit.
   0x06 works today; 0x15 is **fork F-B**.
3. **`GstBackend` — insertion** — `appsrc(meta/x-klv) ! mpegtsmux ! klvpmtrewrite
   ! filesink|udpsink|srtsink`. Real-time via appsrc backpressure (need-data/
   enough-data, is-live).
4. ~~**`klvpmtrewrite` element**~~ — **not built** (fork F-C → ADR
   [`0015`](./decisions/0015-no-pmt-rewrite.md)): stock `mpegtsmux` (gst ≥ 1.20)
   already emits `stream_type 0x06` + the KLVA registration descriptor, so no
   in-library PMT rewrite is needed. (Was to port
   [`gstklvplugin`](./prior-art-gstklvplugin.md)'s `tspmtrewrite`.)
5. **Mock backend** — in-memory `MediaBackend` for testing the core↔backend
   contract without gstreamer.
6. **CMake optional dependency** — `option(MISBKLV_GSTREAMER)`; core stays
   dependency-free; backend built only when gstreamer is found. **Fork F-D**.

## Design forks — all resolved

Opened here during scoping, each resolved by an ADR (see the
[register](./decisions/index.md)); kept as a record of what each weighed.

- **F-A — `MediaBackend` interface** → ADR
  [`0013`](./decisions/0013-media-backend-interface.md): a blocking push-callback
  `extract` + an `Inserter`. The backend owns a **reassembly buffer** (B0 showed
  appsink yields sub-packet fragments), runs `parse_packet`, and yields per-packet
  borrowed spans (ADR 0011 read-borrows boundary); PES PTS unreliable → prefer KLV
  Item 2. Later extended with a `std::stop_token` for cancellation (ADR
  [`0019`](./decisions/0019-extract-cancellation.md)).
- **F-B — 0x15 extraction** → ADR
  [`0016`](./decisions/0016-ts-0x15-extraction.md): a gst-free core demuxer
  (`extract_ts_klv`) handles both 0x06 and 0x15 (stock `tsdemux` drops 0x15).
- **F-C — `klvpmtrewrite` form** → ADR
  [`0015`](./decisions/0015-no-pmt-rewrite.md): **not needed** — stock `mpegtsmux`
  already emits `0x06`+KLVA.
- **F-D — optional-dependency build** → ADR
  [`0014`](./decisions/0014-backend-optional-dependency.md): `option(MISBKLV_GSTREAMER)`
  + a separate `misbklv-gst` target, installable as a `find_package(misbklv
  COMPONENTS gst)` component.

## B0 spike results (2026-07-19, python-gi)

`tsdemux ! meta/x-klv ! appsink` on `Day Flight.mpg` (0x06):

- **Extraction is byte-identical to the ffmpeg-extracted `.klv`** (977 B) — so
  gstreamer feeds the core exactly what our round-trip tests already accept.
  The core↔gst path is de-risked.
- **appsink buffers are byte-fragments, NOT packet-aligned** — 203 buffers for a
  6-packet stream. So the backend must **reassemble** appsink buffers into a byte
  stream and run `parse_packet` to recover packet boundaries; it cannot assume
  one buffer = one KLV packet. (Shapes F-A.)
- **PES PTS came back invalid** (`CLOCK_TIME_NONE`). Correlation should rely on
  the KLV's own Item 2 Precision Time Stamp, not PES PTS — consistent with
  [`0009`](./decisions/0009-st0604-deferred.md). (Confirm on more
  samples; may be a demux-config detail.)

## Phased plan

- **B0 — extraction spike**: done (above).
- **B1 — extraction (0x06) + interface + mock**: land `MediaBackend` (F-A),
  `GstBackend` extraction, the mock backend, and the optional-dep CMake (F-D).
  Test: extract from `Day Flight.mpg` → core, vs the committed `.klv`.
- **B2 — insertion** (done): `appsrc ! mpegtsmux ! filesink`. The B2 spike showed
  stock `mpegtsmux` (gst 1.20.3) already emits `0x06`+KLVA, so **`klvpmtrewrite`
  is not needed** (ADR [`0015`](./decisions/0015-no-pmt-rewrite.md), F-C
  resolved). `GstInserter` + `gst_insert_test`: insert→re-extract byte-exact.
- **B3 — 0x15 extraction** (done): gst-free `extract_ts_klv` in the core handles
  both 0x06 and 0x15 (metadata AU cells), byte-exact vs ffmpeg (ADR
  [`0016`](./decisions/0016-ts-0x15-extraction.md), F-B resolved). Bonus:
  file extraction needs no gstreamer.
- **B4 — real-time streaming** (done): live-sink clock pacing (`InsertConfig::realtime`
  → `appsrc is-live` + sink `sync`) and live-source extraction (`udp:`/`srt:` src)
  ending on a `udpsrc` idle timeout (no EOS crosses the wire). `gst_stream_test`:
  udp loopback (receiver `extract()` on a thread, realtime `push` on main) →
  byte-exact, 6 packets in ~165 ms (clock-paced). ADR
  [`0017`](./decisions/0017-realtime-streaming.md), fork 15. **B0–B4
  complete.**
- **B5 — video passthrough on the insert path** (done): `InsertConfig::video_source`
  adds a `filesrc ! parsebin` branch to the same `mpegtsmux`, so one
  `open_insert` writes video + KLV. Parsed, never decoded (codec-agnostic); the
  source's audio/KLV pads are dropped; KLV must carry real PTS on the video's
  timeline. ADR [`0020`](./decisions/0020-video-passthrough.md), fork 18;
  `gst_video_insert_test`.

## Risks

- ~~**PMT rewrite is the hard part**~~ — **retired**: the B2 spike proved stock
  `mpegtsmux` (gst ≥ 1.20) emits `0x06`+KLVA, so `klvpmtrewrite` isn't built
  (ADR 0015). The single biggest planned risk is gone.
- **0x15 extraction gap** — stock tsdemux can't; custom demux is more work.
- **CI** — the gst backend needs the gst dev libs in CI; keep it a separate,
  skippable job so the core build stays light.

## Outcome

Built as **`misbklv-gst`** (`src/gst/gst_backend.cpp`, `src/gst/stream.cpp`): the
`MediaBackend`/`Inserter` implementation plus the `KlvStream`/`KlvSink` facade
(ADR [`0018`](./decisions/0018-high-level-api.md)), live `extract` cancellable via
a stop token (ADR [`0019`](./decisions/0019-extract-cancellation.md)). Extraction
+ insertion (KLV alone, or alongside a passed-through video stream — ADR
[`0020`](./decisions/0020-video-passthrough.md)), file + live (`udp`/`srt`), all
on **stock gstreamer — no custom element**. Consumable via `find_package(misbklv COMPONENTS gst)`.
