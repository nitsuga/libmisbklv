# gstreamer backend — scope

Scoping for the media backend (ADR [`0008`](../context/decisions/0008-media-backend-gstreamer.md)):
the layer between the KLV core (parse/build) and MPEG-TS I/O. Library-style
gstreamer (link libs, drive pipelines; not shipped plugins). This doc grounds
that shape in the real environment and lays out components, open decisions, and
a phased plan. Empirical probes done 2026-07-19.

## Environment (verified)

- **gstreamer 1.20.3**; `gstreamer-app-1.0` (appsrc/appsink) present; all pipeline
  elements present (`tsdemux`, `tsparse`, `mpegtsmux`, `appsrc/appsink`,
  `filesrc/sink`, `udpsink`, `srtsink`, `souphttpsrc`).
- **`gstreamer-mpegts-1.0` 1.20.3 installed** (`libgstreamer-plugins-bad1.0-dev`)
  — the library for PMT / descriptor manipulation (`GstMpegtsSection`,
  registration descriptor), needed for the KLVA PMT-rewrite. Headers resolve.
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
4. **`klvpmtrewrite` element** — in-library `GstElement` (ADR 0008 option c),
   registered in-process at init; rewrites the PMT to add the KLVA registration
   descriptor + `stream_type` (0x06 async). Needs `gstreamer-mpegts-1.0`. Form is
   **fork F-C**. Reference: [`gstklvplugin`](../context/prior-art-gstklvplugin.md)
   `tspmtrewrite`.
5. **Mock backend** — in-memory `MediaBackend` for testing the core↔backend
   contract without gstreamer.
6. **CMake optional dependency** — `option(MISBKLV_GSTREAMER)`; core stays
   dependency-free; backend built only when gstreamer is found. **Fork F-D**.

## Open decisions (need ADRs before/*during* implementation)

- **F-A — `MediaBackend` interface**: pull vs push; callback vs iterator for
  extraction; **buffer ownership** — B0 showed appsink yields sub-packet
  fragments, so the backend owns a **reassembly buffer** (concatenated appsink
  data), runs `parse_packet` over it, and yields per-packet `KlvUnit`s whose core
  spans borrow into that owned buffer (the ADR 0011 read-borrows boundary); how
  PTS/timestamps surface (B0: PES PTS unreliable → prefer KLV Item 2). *The
  keystone; decide first.*
- **F-B — 0x15 extraction**: stock `tsdemux` won't surface it. Options: (a) a
  custom path — `tsparse` + a pad probe / manual PID demux by the KLV PID; (b)
  defer 0x15 *extraction* to post-v1 (v1 extracts 0x06 only); (c) investigate a
  `tsdemux` property / metadata_descriptor that unlocks it. Interacts with the
  deferred sync-KLV work (ADR 0008).
- **F-C — `klvpmtrewrite` form**: `GstBaseTransform` over the TS stream vs a pad
  probe rewriting `GstMpegtsSection`; how much of `gstreamer-mpegts` to lean on.
- **F-D — optional-dependency build**: `option()` + `find_package`/pkg-config;
  keep the core target dependency-free; a separate `misbklv-gst` target/component.

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
  [`0009`](../context/decisions/0009-st0604-deferred.md). (Confirm on more
  samples; may be a demux-config detail.)

## Phased plan

- **B0 — extraction spike**: done (above).
- **B1 — extraction (0x06) + interface + mock**: land `MediaBackend` (F-A),
  `GstBackend` extraction, the mock backend, and the optional-dep CMake (F-D).
  Test: extract from `Day Flight.mpg` → core, vs the committed `.klv`.
- **B2 — insertion + `klvpmtrewrite`**: `appsrc ! mpegtsmux ! klvpmtrewrite !
  filesink` (F-C); needs `gstreamer-mpegts`. Test: build a TS from core-encoded
  KLV, re-extract via B1, round-trip byte-exact; verify `0x06`+KLVA signaling.
- **B3 — 0x15 extraction** (per F-B outcome): custom PID path, or defer.
- **B4 — real-time streaming**: `udpsink`/`srtsink` + appsrc backpressure; the
  push-KLV API blocking on flow control (the ADR 0008 ergonomic win).

## Risks

- **PMT rewrite is the hard part** — `klvpmtrewrite` is real GstElement +
  `gstreamer-mpegts` work; the single biggest chunk (bounded by the gstklvplugin
  reference).
- **0x15 extraction gap** — stock tsdemux can't; custom demux is more work.
- **CI** — the gst backend needs the gst dev libs in CI; keep it a separate,
  skippable job so the core build stays light.

## First step

Install `libgstreamer-plugins-bad1.0-dev` (for `gstreamer-mpegts-1.0`), then
resolve **F-A** (interface) — write the ADR — and do **B0** (extraction spike)
to lock the buffer-ownership boundary before building `GstBackend`.
