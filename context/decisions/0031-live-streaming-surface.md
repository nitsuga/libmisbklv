---
type: Decision
title: Vendor-neutral live streaming surface — file-or-live video source and multicast sink
decision_status: accepted
tags: [decision, streaming, gstreamer, backend, phase-3]
generated:
  by: claude/opus-5
  at: 2026-08-20T18:45:00Z
fork: 28
---

# Context

The insert path (`KlvSink` / `open_insert`) is file-shaped today:

- `InsertConfig::video_source` is a **file path** → `filesrc ! demuxer ! parser`,
  where the demuxer/parser come from a container sniff (ADR 0025).
- `realtime + video_source` is **rejected** (`Error::Unsupported`,
  [`gst_insert.cpp`](../../src/gst/gst_insert.cpp) — see ADR 0020 § Decision ("realtime + video_source is rejected")),
  so even a *file* source cannot be played out on a live clock against a UDP/SRT
  sink.
- The `udp:` sink is built with `host` + `port` only
  ([`gst_insert.cpp`](../../src/gst/gst_insert.cpp) `make_sink`), so a multicast
  destination works only by accident of the GStreamer defaults
  (`auto-multicast=TRUE`, `ttl-mc=1`, `g_socket_set_broadcast=TRUE`) with no
  knobs to set TTL, the egress interface, or loopback.

Two consumers of this library (parrot-to-klv and dji-to-klv — MP4 → KLV/MPEG-TS
converters) both need to go live: **(a)** ingest a live video source — the
drones stream over RTSP — and **(b)** output a live MPEG-TS over UDP multicast.
Their metadata decode differs per vendor, but the mux/pacing/multicast problem
they now face is identical, and it is exactly the problem this library already
centralizes (`gst_insert` / `gst_video` / ADR 0017 realtime pacing). The
vendor-specific live metadata is *not* in the video elementary stream either —
it rides in RTP packet-header extensions that the vendor project must capture
at the RTP layer (a plain `rtsp ! rtph264depay` drops them) — so the generic
library has no business touching it.

# Decision

Grow the insert path's video source and sink to be **live-capable and
vendor-neutral**. Specifically:

1. **`video_source` accepts a GStreamer URI/description, not only a file path.**
   Grammar (auto-detected; backward compatible):
   - `""` — KLV-only (current).
   - `file:PATH` or a bare path that exists — `filesrc ! demuxer ! parser`
     (current, via the sniff tables from ADR 0025). Unchanged.
   - `rtsp[s]:` — `rtspsrc location=URI ! rtph264depay ! h264parse` (or the
     `h265` analogues). Live branch.
   - `pipeline:<gst-launch desc>` — an explicit escape hatch (a `udpsrc`
     mpegts source, test fixtures, or an odd vendor shape), built inside a
     `GstBin` with a ghost source pad and linked to the reserved muxer pad.

2. **`udp:` sinks gain multicast/broadcast knobs.** New `InsertConfig` fields
   (`udp_ttl_mcast`, `udp_mcast_iface`, `udp_loop`) mapped onto `udpsink`'s
   `ttl-mc` / `multicast-iface` / `loop` (and `auto-multicast`). Defaults
   reproduce today's behavior exactly.

3. **Lift `realtime + video_source`.** For a `file:` source, `realtime` now
   means "replay the recording on the pipeline clock to a live sink." For an
   `rtsp:`/`pipeline:` live source, `realtime` is the normal mode — both
   branches share the pipeline clock. `kNoPts` stays rejected with a video
   source (ADR 0020 § one timeline); KLV PTS is the video branch's running time,
   which is the only correct live mapping (see ADR 0021).

What is **deliberately not** in scope:

- **No vendor metadata extraction.** No Parrot `P1/P2/P3/Pb` or DJI RTP
  header-extension capture, no `TelemetrySnapshot`, no `to_0601`, no RTCP
  SDES parser, no `GstRTPHeaderExtension` subclass, no new `protobuf`/`minimp4`
  dependency.
- **No `VendorDecoder` callback API.** `Inserter::push(bytes, pts_ns)` already
  *is* the interface — the consumer pushes whatever it decoded. A decoder hook
  would just re-invent `emit()` and force the library to own the RTP pipeline.
- **No new shared repo.** Generality is achieved through the library; vendor
  glue stays in each consumer.

# Alternatives considered

- **Add vendor capture to libmisbklv** (Parrot/DJI RTP + decode): rejected. It
  couples a published, versioned, vendor-neutral MISB/KLV library to one drone
  vendor's proprietary transport and format, forces a `protobuf` dependency into
  the core, and is a forcing function to add every other vendor the same way —
  the library becomes drone-vendor-compat. It is the exact move ADR 0020 warns
  about ("if this API ever grows a codec or encoder option, the feature has
  moved into the wrong repository").
- **A new shared repo (`drone-live-klv`) parameterized by a `VendorDecoder`**:
  rejected for now. Premature abstraction before either consumer has shipped a
  live path; a three-repo version matrix with nothing proven against it. The
  parameterized interface leaks — DJI live transport is not Parrot's RTP
  extensions, so the decoder abstraction doesn't hold at the transport layer.
  The genuinely shared code (the ~150 lines of mux construction and
  backpressure) already lives `gst_insert`/`gst_video`. Revisit only if two
  *shipped* live consumers converge on identical harness code this library can't
  host for neutrality.
- **Keep `video_source` file-only and keep `realtime + video` rejected**:
  rejected, because it is the very boundary the requirement asks to cross —
  both a live video input and a clock-paced multicast output with video need it
  lifted.
- **Deprecate `video_source` and expose a caller-held appsink instead**: a real
  option, but it throws away the pipeline construction (`prepare_video_branch`,
  stream-order pad reservation) this library already owns. The URI/description
  form reuses it; keep the appsink idea as a later option if a caller ever needs
  byte-level control of a live branch.

# Consequences

- `InsertConfig` grows (backward compatible); existing file-only callers and
  `gst_insert_test` are untouched.
- `prepare_video_branch()` forks on file vs live: the file path keeps its
  `PAUSED` pad-wait; the live path skips the container sniff, does not wait in
  `PAUSED` (live preroll never completes), and reports `Unsupported` only on a
  bus `ERROR`.
- Live preroll/timebase semantics (how KLV PTS maps to a live branch's running
  time; `mpegtsmux` + `sync` sink pacing with two live inputs) must be pinned
  during implementation, with the ADR 0020 lesson applied: pipeline-construction
  changes need repeated runs under load before they are believed, and a
  loopback-multicast + live-video-pacing hermetic test is required.
- Each consumer (parrot-to-klv, dji-to-klv, future) owns its own RTSP/vendor
  decode against this surface; the library stays vendor-neutral.
- `ts-udpsink` / `multiudpsink` are noted for live TS pacing and fanout but not
  decided here (the former is not installed by default).

# Assumptions / open questions

- This ADR is `accepted`; it records the shape and the placement. Implementation
  is separate work, now unblocked.
- Whether a "TS-in → TS-out with KLV inserted" remux API is wanted at all is a
  different fork (adjacent to the ROADMAP live-0x15 streaming-demux item), not
  decided here.

# Citations

- [ADR 0017](./0017-realtime-streaming.md) — the `realtime` pacing model this
  extends to a video branch.
- [ADR 0020](./0020-video-passthrough.md) — `video_source`, stream order, and
  the `realtime + video` rejection this lifts.
- [ADR 0021](./0021-read-path-timestamps.md) — the live-reception timeline that
  defines live KLV PTS.
- [ADR 0025](./0025-explicit-demuxer-passthrough.md) — the container-sniff
  table the `file:` path keeps.
- [`../../planning/ROADMAP.md`](../../planning/ROADMAP.md) — where fork 28 is
  decided.
