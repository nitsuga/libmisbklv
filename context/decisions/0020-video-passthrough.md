---
type: Decision
title: Video passthrough on the insert path
status: accepted
tags: [decision, backend, muxing, video, phase-3]
timestamp: 2026-07-25T18:00:00Z
fork: 18
---

# Context

The insert path built exactly one branch — `appsrc(meta/x-klv) ! mpegtsmux !
sink` ([`0013`](./0013-media-backend-interface.md), B2) — so its output TS
carried KLV **and nothing else**. That is enough for editing an existing stream
(read a `.ts`, edit, write a `.ts` of KLV) but not for *authoring* one: a
consumer that converts some other container into "video + MISB KLV" has real
video to carry.

The concrete driver is `parrot-to-klv`, which turns Parrot drone MP4s into
MPEG-TS with ST 0601 KLV. Its transform stage produces real 0601 packets and
wants to hand them to `KlvSink`; there was no way to also get the source's video
into the same output. Its options were to carry video *here*, or to rebuild the
mux pipeline itself — duplicating muxing knowledge this library already owns and
demoting libmisbklv to a packet encoder.

# Decision

**Carry video in the library that owns the muxer.** `InsertConfig` gains one
optional field, `video_source`; empty means today's pipeline byte-for-byte, so
the change is source- and ABI-compatible for existing callers. `KlvSink` gains
the matching defaulted constructor argument.

- **Passthrough only, never decode.** `filesrc ! parsebin` joins the existing
  `mpegtsmux`. `parsebin` auto-plugs the demuxer and parser for the container and
  codec and stops there, so the elementary stream reaches the muxer unchanged and
  our code branches on **no** codec at all. Verified end-to-end on H.264-in-TS,
  H.264-in-MP4 (`avc` → byte-stream conversion handled inside `h264parse`), and
  H.265-in-MP4 — one code path, three cases. The explicit
  `qtdemux ! h264parse|h265parse` fallback the spec allowed was not needed and
  would have hardcoded exactly the container/codec knowledge we are avoiding.
  **If this API ever grows a codec or encoder option, the feature has moved into
  the wrong repository.**
- **First video pad wins; everything else is dropped.** `parsebin`'s `pad-added`
  links the first `video/*` pad to a `mpegtsmux` request pad. Audio, subtitles,
  and any KLV the source already carries are ignored — the caller is supplying
  its own converted KLV, and forwarding the source's would put two metadata
  streams in the output. A second video pad is ignored and logged (`g_warning`),
  not an error.
- **`open_insert` waits for that pad before `PLAYING`.** The pipeline goes to
  `PAUSED`, where `parsebin` prerolls and exposes pads, and returns only once the
  video pad is linked. Without the wait, a caller pushing KLV immediately races
  the video branch and the muxer can write a **KLV-only PMT**. A source that
  yields no video pad (unparseable, or audio-only) fails with
  `Error::Unsupported`; the wait is bounded (10 s) and also breaks on a bus
  error.
- **One timeline, enforced.** Both branches must share the source's timeline or
  the KLV does not line up with the frames it describes. The video branch is
  timestamped from the file, running from zero; the caller must therefore push
  KLV with PTS on that same timeline. **`kNoPts` is rejected with
  `Error::Unsupported`** when a video source is set, rather than falling back to
  `push()`'s synthesized ~30 fps counter — that counter has no relation to the
  source's frame timing and would drift *silently*, producing output that looks
  right and is wrong.
- **`realtime` + `video_source` is rejected** (`Error::Unsupported`). Clock-paced
  output with a file-backed video branch is unexercised; better to refuse than to
  ship something untested. Revisit if a live-out consumer appears.
- **A missing or unreadable `video_source` is checked before anything is built**,
  so a failed `open_insert` leaves no partial output file (a `filesink` creates
  its file the moment the pipeline leaves `NULL`).

Audio is **dropped**, deliberately, not carried. Pad handling makes carrying it
nearly free, but "drop" is the choice that keeps the scope honest: this library
is about KLV in MPEG-TS, and the video branch exists only so the KLV has frames
to describe. Adding audio would start the slide toward a general remuxer, and it
is trivially revisited if a consumer needs it (link non-video pads too).

# Alternatives considered

- **Leave it to consumers — each rebuilds `appsrc ! mux ! sink` around its own
  video branch.** Rejected: it duplicates the muxing knowledge (`mpegtsmux`
  emitting `0x06` + `KLVA` — [`0015`](./0015-no-pmt-rewrite.md) — plus the sink
  grammar and the EOS/drain dance) in every consumer, and reduces libmisbklv to a
  packet encoder. Carrying a second pad through a muxer we already run is the
  smaller change and is placed where the knowledge lives.
- **A general "remux these streams" API** (stream selection, audio, codec
  options). Rejected as scope creep in the wrong repo — see above.
- **Explicit `qtdemux ! h264parse|h265parse`**, choosing the parser from the
  demuxed caps. Rejected: `parsebin` did the job with no container/codec
  branching, and the explicit form would need extending for every new input.
- **Synthesize KLV PTS from the video's running time** instead of rejecting
  `kNoPts` — the library could timestamp KLV off the video branch. Rejected for
  v1: it guesses at a mapping only the caller knows (which frame a metadata
  packet describes), and quietly guessing wrong is the exact failure mode this
  ADR is trying to make impossible.
- **Forward the source's own KLV** when it has one. Out of scope; a
  passthrough-KLV mode is a separate feature (and conflicts with the caller
  pushing its own on the same output).

# Consequences

- `open_insert({.sink=…, .video_source=…})` + `push()` + `finish()` yields a TS
  with a video PID and a KLV PID from one call; `KlvSink(sink, false, video)`
  gets the same at the facade level.
- Existing callers are untouched: empty `video_source` takes the original
  code path (straight to `PLAYING`, `kNoPts` still synthesized), and
  `gst_insert_test` passes unchanged as the regression guard.
- New `gst_video_insert_test` (CTest `gst_video_insert`, ~0.3 s) muxes
  `data/klv_metadata_test_sync.ts`'s video with `dayflight.klv` and asserts, over
  the output TS read back with its own small TS/PES reader: the PMT lists exactly
  two elementary streams (video + `0x06`/`KLVA`, so the source's own KLV was
  dropped), the KLV re-extracts byte-exact, the video elementary stream is
  **byte-identical** to the source's with the same frame count and codec, the
  pushed PTS intervals survive, both branches share one PTS origin, `kNoPts` is
  refused, and a missing source fails without creating an output file.
- The muxer applies its own constant offset to the TS clock (it starts the stream
  an hour in so early timestamps can't go negative), so absolute 90 kHz PTS are
  not the pushed nanoseconds — the invariants are the *intervals* and the
  *shared origin*.
- Backpressure is the intended behaviour, not a deadlock: the muxer waits on the
  slower pad and the KLV `appsrc` is `block=TRUE`, so a caller must push in
  increasing PTS order, interleaved with the video's progress, rather than
  dumping a whole file's KLV up front. Documented on `InsertConfig`.
- `MockBackend` is unaffected — it ignores `video_source` (it has no muxer), so
  video passthrough is a gstreamer-backend capability, not an interface promise.

# Assumptions / open questions

- **File in.** The video branch is `filesrc`-backed; a live video input (and
  `realtime` with video) is out of scope and currently rejected.
- **Only the first video stream is carried.** Multi-video sources are rare here;
  if one matters, the choice of *which* stream would need to enter the API.
- A source that is a valid file but has no video stream fails as
  `Error::Unsupported`, which is also what an unparseable file returns — the
  distinction isn't visible to callers. Fine until someone needs to tell them
  apart.

# Citations

[1] [`0013`](./0013-media-backend-interface.md) — the `InsertConfig` /
    `open_insert` contract this extends by one field.
[2] [`0015`](./0015-no-pmt-rewrite.md) — stock `mpegtsmux` emits `0x06` + `KLVA`;
    still true with a second pad on the muxer.
[3] [`0017`](./0017-realtime-streaming.md) — `InsertConfig::realtime`, the pairing
    rejected here.
[4] `planning/video-passthrough-spec.md` — the consumer-side spec written by
    `parrot-to-klv`, which this implements. Kept out of the repo deliberately:
    it is a fulfilled request, and this ADR is where its content now lives.
