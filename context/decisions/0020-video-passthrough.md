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

- **Passthrough only, never decode.** (*The chain is now `filesrc ! demuxer` —
  see "Mechanism superseded" at the end. What this bullet decides still holds;
  read `parsebin` below as "the thing that auto-plugged the chain".*)
  `filesrc ! parsebin` joins the existing `mpegtsmux`. `parsebin` auto-plugs the demuxer and parser for the container and
  codec and stops there, so the elementary stream reaches the muxer unchanged and
  our code branches on **no** codec at all. Verified end-to-end on H.264-in-TS,
  H.264-in-MP4 (`avc` → byte-stream conversion handled inside `h264parse`), and
  H.265-in-MP4 — one code path, three cases. The explicit
  `qtdemux ! h264parse|h265parse` fallback the spec allowed was not needed and
  would have hardcoded exactly the container/codec knowledge we are avoiding.
  **If this API ever grows a codec or encoder option, the feature has moved into
  the wrong repository.**

  *Amended 2026-07-28:* the MP4 audio-track fix put an unconditional `h264parse`
  on every video pad, which silently falsified the codec-agnostic claim above —
  an H.265 or MPEG-2 source could not link. The parser is now selected from the
  pad's caps (`h264parse` / `h265parse` / none), restoring one code path with a
  codec-shaped seam in it, and H.265 and MPEG-1/2 sources are covered by tests
  ([`0024`](./0024-sei-generation-opt-in.md)). The seam is a *parser* choice, not
  a codec or encoder option — the elementary stream still reaches the muxer
  without being decoded.
- **First video pad wins; everything else is dropped.** `pad-added` links the
  first `video/*` pad to a `mpegtsmux` request pad. Audio, subtitles, and any KLV
  the source already carries are dropped — the caller is supplying its own
  converted KLV, and forwarding the source's would put two metadata streams in
  the output. A second video pad is dropped too, and logged (`g_warning`), not an
  error.

  *Dropped does not mean unlinked* (amended 2026-07-28). Every pad the demuxer
  exposes must be linked or it errors the pipeline as not-linked, so each dropped
  stream goes to its own `queue ! fakesink`, `leaky=downstream`. The queue is
  load-bearing, not tidiness: a demuxer pushes all of its streams from one
  thread, and a sink in `PAUSED` prerolls one buffer then blocks that thread
  until `PLAYING` — so a bare `fakesink` on a dropped stream blocks the video
  behind it, the muxer never prerolls, `PLAYING` never arrives, and nothing
  unblocks. That deadlock hung CI on every run under gstreamer 1.24.
- **`open_insert` waits for that pad before `PLAYING`.** The pipeline goes to
  `PAUSED`, where `parsebin` prerolls and exposes pads, and returns only once the
  video pad is linked. Without the wait, a caller pushing KLV immediately races
  the video branch and the muxer can write a **KLV-only PMT**. A source that
  yields no video pad (unparseable, or audio-only) fails with
  `Error::Unsupported`; the wait is bounded (10 s) and also breaks on a bus
  error.
- **The PMT announces KLV first, video second — and that stands** (added
  2026-07-28, after an attempt to change it was reverted). `mpegtsmux` numbers
  the PMT by the order sink pads are *requested*, and the KLV `appsrc` is linked
  while the pipeline is still NULL, so it takes `sink_0`. The video pad can only
  be requested once the demuxer exposes its pad, which is necessarily later.

  Cosmetically this is wrong way round — a consumer doing `ffmpeg -map 0:0`
  expects video — but every way of reversing it that we have tried costs
  correctness, and correctness is not a trade we make for stream numbering. See
  *Stream order* below for what was tried and measured.
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
- **A failed `open_insert` leaves no output file**, in two layers. *(Extended by
  [`0022`](./0022-no-output-on-failure.md) to the whole insert session: a
  failing or never-called `finish()` cleans up the same way. A source whose
  video is declared but unparseable opens fine and fails at EOS, which reached
  the same leak one step later.)* A missing or
  unreadable `video_source` is caught before any element is built, so nothing can
  have been created. But a *readable* source with no video stream only reveals
  itself in `PAUSED` — and a `filesink` creates its file the moment the pipeline
  leaves `NULL` — so failures past that point **unlink the sink file, and only if
  this call created it**: the path is probed before the state change, and a file
  that was already there is left alone (it is the caller's). What that guarantee
  does *not* extend to is the old contents of such a file: opening a file sink
  truncates, on the success path too. Pre-flighting the video branch in a
  throwaway pipeline would avoid even that, and is more machinery than this
  needs.

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
- New `gst_video_insert_test` (CTest `gst_video_insert`) muxes
  `data/klv_metadata_test_sync.ts`'s video with `dayflight.klv` and asserts, over
  the output TS read back with its own small TS/PES reader: the PMT lists exactly
  two elementary streams (video + `0x06`/`KLVA`, so the source's own KLV was
  dropped), the KLV re-extracts byte-exact, the video elementary stream is
  **byte-identical** to the source's with the same frame count and codec, the
  pushed PTS intervals survive, both branches share one PTS origin, and `kNoPts`
  is refused. Both failure paths are covered — a missing source *and* a readable
  videoless one — each asserting no output file is left, plus that a pre-existing
  file at the sink path is not deleted.
- **Both demuxer paths are covered.** The test runs its whole battery a second
  time against an MP4 remuxed from the TS source (`tsdemux ! h264parse ! mp4mux`,
  skipped if `mp4mux` is absent), because `parsebin` auto-plugs `qtdemux` there —
  a different demuxer negotiating different caps into the muxer, and the path a
  consumer converting MP4s actually takes. The MP4 run asserts the same frame
  count as the TS run; its elementary stream is *not* byte-compared, since
  `avc` → byte-stream re-inserts parameter sets (2 774 895 vs 2 774 857 bytes for
  the same 418 frames). A non-TS source skips the source-comparison checks rather
  than failing them, so the binary is usable against a consumer's own file.
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
- **The video ES is no longer byte-identical (2026-07-27,
  [`0023`](./0023-st0604-sei-passthrough.md)):** this design's "parsed, never
  decoded" property still holds — no transcode — but H.264 passthrough now
  rewrites each access unit's NAL stream to strip Picture Timing SEI and inject
  a generated ST 0604 Precision Time Stamp SEI. Callers comparing output ES
  bytes against the source will see a difference; the picture data itself is
  untouched.

## Stream order: why KLV is `0:0` and why it stays there (2026-07-28)

Attempted, shipped, and reverted the same day. Recorded here because the cost
was real and the next person to notice `0:0` is the metadata stream will want to
fix it too.

**The constraint.** `mpegtsmux` assigns PMT position by the order sink pads are
*requested*. To put video first, its pad must be requested first — but the video
pad's caps are only known once the demuxer exposes it, which requires `PAUSED`.
The KLV `appsrc` has no such dependency and is linked while the pipeline is
still NULL. That asymmetry is the whole problem.

**Attempt 1 — defer the `appsrc` link until after the video-pad wait.** Gets the
order right and is the version that shipped. It is racy twice over:

- The wait returns as soon as the demuxer pad is *linked*, while preroll
  continues on the streaming threads. A muxer that reaches its first output
  before the app thread gets back to linking the `appsrc` writes a **video-only
  PMT and drops the KLV entirely** — a playable file, exit 0, no telemetry.
  Measured 4 failures in 60 runs of a downstream consumer's mux test under CPU
  load; 0 in 52 on the pre-change build.
- Independently, it destabilised **ST 0604 SEI timing**. Linking the KLV branch
  after the video branch has prerolled changes when its segment is established,
  and SEI generation matches KLV PTS to frame PTS within a tolerance. `linear
  time: SEI emitted`, `forward jump: Discontinuity reported`, `round trip:
  re-emitted` and `MP4 path: same frame count` began failing intermittently — 6
  runs in 25 on an *idle* box, versus 0 in 25 before the change.

  A block probe on the muxer's src pad, released once both sink pads exist,
  fixes the first problem completely and the second not at all: 0/60 on the PMT
  race, still 6/25 overall. Ordering and completeness are separate mechanisms,
  and the SEI damage is caused by the deferral itself, not by what the muxer
  emitted.

**Attempt 2 — reserve the video sink pad up front, before linking the `appsrc`.**
This is the shape that *should* work: both pads requested on one thread while the
pipeline is still NULL, no deferral, no timing change at all. It does not work,
and the reason is worth knowing. `mpegtsmux` refuses the later link onto that
reserved pad with `GST_PAD_LINK_NOFORMAT`, because once the pad has been
activated through the state change the link is checked against the parser's
*current* caps — still `avc`, straight from the demuxer — while the muxer accepts
only `byte-stream`. The **template** caps intersect perfectly; the current ones
do not. A freshly requested pad renegotiates on link, an activated reserved one
will not. Inserting the capsfilter explicitly, via
`gst_element_link_pads_filtered` onto the pad by name, fails the same way.

**Decision: leave the order alone.** Stream numbering is cosmetic; dropped
telemetry and wrong timestamps are not. A consumer that needs the video can
select it by stream type or codec rather than by index, which is what a PMT is
for. `gst_video_insert_test` now pins the current order so a future attempt is a
visible, deliberate change.

**The transferable lesson** is not about `mpegtsmux`. The shipped change was
green on its first suite run and had been reasoned about carefully; both
regressions were load- and timing-dependent, and one of them only showed up in a
*downstream* consumer. Any change to pipeline construction order needs repeated
runs under load before it is believed — a single green suite is close to no
evidence at all.
- A source that is a valid file but has no video stream fails as
  `Error::Unsupported`, which is also what an unparseable file returns — the
  distinction isn't visible to callers. The pipeline's own error text (e.g.
  "Could not determine type of stream") is logged via `g_warning` before being
  dropped, since consuming it off the bus means `finish()` can't report it later;
  that keeps the diagnosis available without widening the `Error` enum. Fine
  until someone needs to tell the two apart programmatically.

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

> **Mechanism superseded (2026-07-28).** The chain is no longer `filesrc !
> parsebin` — parsebin turned out to require a *decoder* in the registry before
> it will expose a parsed stream, which this library cannot depend on. It is now
> `filesrc ! demuxer` with the container sniffed up front. Everything this ADR
> decided — carry video through, never decode, codec-agnostic — still holds; only
> how the chain is assembled changed. See
> [`0025`](./0025-explicit-demuxer-passthrough.md).
