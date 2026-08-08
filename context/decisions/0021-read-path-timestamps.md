---
type: Decision
title: Timestamps on the read path (KlvPacket::pts_ns)
decision_status: accepted
tags: [decision, backend, extraction, timing, phase-3]
generated:
  by: claude/opus-5
  at: 2026-07-26T10:00:00Z
fork: 19
---

# Context

`KlvPacket::pts_ns` has been a documented field since
[`0013`](./0013-media-backend-interface.md) — "PES PTS if present, else
`kNoPts`" — and **neither extractor ever set it**. The gstreamer backend
constructed `KlvPacket{bytes, kNoPts}` unconditionally, dropping the buffer PTS
`tsdemux` had already computed; `extract_ts_klv` never looked at the PES header's
timestamp at all. Every consumer saw `-1` on every packet of every file.

The field was written that way on purpose. The B0 spike found "PES PTS is
unreliable (`GST_CLOCK_TIME_NONE`)" and [`0017`](./0017-realtime-streaming.md)
recorded that PTS on extraction "stays `kNoPts`", with correlation deferred to
the KLV's own Item 2 Precision Time Stamp. That conclusion came from one
historical sample: `data/Day Flight.mpg`, whose KLV PES genuinely carry **no**
PTS (`PTS_DTS_flags` clear — verified byte-wise, not inferred). It does not
generalize. The other historical captures, `falls.ts`, `Cheyenne.ts`, and
`klv_metadata_test_sync.ts`, carried a PTS on every KLV PES (including every
`0x15` PES in the latter two).

[`0020`](./0020-video-passthrough.md) turned the gap into a break. With a
`video_source`, `push()` **requires** a real PTS on the source's timeline and
rejects `kNoPts`. So the facade stopped composing: `KlvStream` → edit →
`KlvSink` silently re-timed the output to the synthesized ~30 fps counter, and
the same round trip with video was refused outright. The writer demanded exactly
what the reader threw away, and the flagship use case — read a video+KLV file,
edit an item, write it back — was not expressible.

# Decision

**Both extractors report a real timestamp, defined as nanoseconds from the start
of the source** — the same timeline `Inserter::push()` writes on. That shared
definition is the point: it is what makes read → edit → write preserve timing
instead of re-deriving it.

- **gstreamer backend**: the demuxer's **running time**
  (`gst_segment_to_running_time` on the sample's segment), not the raw buffer
  PTS. `tsdemux`'s segment is program-wide and starts at the earliest timestamp
  in the program, so running time is zero-based at the start of the source —
  and it is the same quantity `mpegtsmux` consumes on the insert path, including
  for the ADR 0020 video branch. An untimed buffer yields `kNoPts`.
- **`extract_ts_klv`** (gst-free, core): the PES header's 33-bit PTS, converted
  from 90 kHz to nanoseconds (exactly, ×100 000/9), minus the **earliest PTS
  anywhere in the buffer**. That origin is found by a cheap header-only pre-pass.
  It is the *minimum*, not the first in file order: with reordered video the
  first PES in the file is not the earliest presentation time, and anchoring
  there would shift every reported timestamp by the reorder delay.
- **A packet is timestamped by the unit its FIRST byte fell in.** Packets and
  carrying units do not line up — one PES (or appsink buffer) may hold several
  packets, and a packet may begin in one and end in the next. Both extractors
  therefore record a mark per timestamped unit against an absolute offset in the
  reassembled KLV byte stream and look up the mark in effect at each packet's
  start (`src/pts_marks.hpp`, private to the implementation; the two extractors
  share the logic rather than each getting it subtly wrong).
- **`kNoPts` stays meaningful**: it now means "this stream did not say", not
  "we did not look". Day Flight reports `kNoPts` throughout, correctly — for
  such a stream, correlation is still via Item 2
  ([`0009`](./0009-st0604-deferred.md)).

Scope note: `0x15` sync-KLV files are out of reach of the gstreamer path
entirely ([`0016`](./0016-ts-0x15-extraction.md) — `tsdemux` creates no pad for
them), so their timestamps come only from `extract_ts_klv`. Each path is
specified for what it can actually see.

# Alternatives considered

- **Report the raw PES PTS / raw buffer PTS, un-anchored.** Rejected: the values
  are on the stream's own clock (`falls.ts` starts near 49 773 s; `mpegtsmux`
  deliberately starts an hour in), so they are not the timeline `push()` wants
  and every consumer would have to re-derive an origin — the work this ADR
  exists to stop duplicating.
- **Anchor the standalone reader at the first *KLV* PTS**, so the first KLV
  packet is always 0. Simpler to explain, and wrong for a stream whose metadata
  starts after the video: the offset between the two is real information, and
  discarding it re-times the KLV against the frames it describes.
- **Anchor at the first PTS in file order** rather than the minimum. One less
  pass, but wrong by the video reorder delay on any file with B-frames.
- **Leave `pts_ns` unset and document it as reserved.** Honest, but it makes
  ADR 0020's insert path unusable from the library's own read path and pushes a
  PES parser into every consumer — `parrot-to-klv` had already written one.
- **Synthesize timestamps when the stream has none.** Rejected for the same
  reason ADR 0020 rejects it on the write side: a plausible-looking wrong
  timestamp is worse than an explicit `kNoPts`.

# Consequences

- `KlvStream` → edit → `KlvSink` **keeps the source's timing**, with or without
  a video branch; the round trip that ADR 0020 made impossible now works.
- `Message::pts()` is populated from the stream, so a consumer converting or
  editing an existing file no longer needs to supply timestamps of its own.
- `extract_ts_klv` timestamps `0x15` streams that gstreamer cannot even see —
  a capability the gst path has no equivalent for.
- Tests: `gst_video_insert_test` writes a TS with known pushed timestamps and
  now asserts that **both** extractors report them back (its own independent PES
  parser stays the witness that the timing is in the file), plus a
  `KlvStream` → `KlvSink` round trip over a video source that checks the timing
  survives re-muxing. `ts_extract_test` checks the generated project-owned
  timed and untimed `0x06`/`0x15` fixtures: timestamps are present on all
  packets or none, and are non-decreasing.
- `MockBackend` takes an optional per-packet `pts` vector, so the test double can
  express the contract instead of only the sentinel; omitted, it replays an
  untimed stream exactly as before.
- One behavioral change for existing callers: code that treated `pts()` as
  always `-1` now sees real values. The insert path's `kNoPts` fallback is
  unchanged, so a KLV-only sink fed untimed input behaves exactly as before.

# Assumptions / open questions

- **The two origins are established independently** and can differ by about one
  frame at the very start of a stream that begins mid-PES: on `falls.ts`,
  `tsdemux` discards the first buffer's timestamp and anchors its segment one
  frame later than the earliest PTS `extract_ts_klv` finds. Intervals are exact
  in both, and a round trip uses one reader throughout, so this is a documented
  edge rather than a defect to chase.
- **No 33-bit PTS wraparound handling** (~26.5 h of stream). A file that wraps
  would produce a negative jump; no sample does, and handling it means tracking
  discontinuities the standalone reader has no other reason to model.
- **`extract_ts_klv` wants the whole stream.** Its origin is the minimum over
  the buffer it is handed, so extracting from a mid-file chunk re-anchors the
  timeline to that chunk. Documented on the function.
- Live sources report running time from when the demuxer's segment opened, which
  for `udp`/`srt` is the start of *reception*, not of the sender's file. Nothing
  else is knowable at the receiver.

# Citations

[1] [`0013`](./0013-media-backend-interface.md) — where `pts_ns` was specified
    (and where "PES PTS is unreliable" was recorded from the Day Flight spike).
[2] [`0016`](./0016-ts-0x15-extraction.md) — the gst-free extractor this adds
    timestamps to, and why `0x15` is only reachable there.
[3] [`0017`](./0017-realtime-streaming.md) — "PTS on extraction stays `kNoPts`",
    superseded by this ADR.
[4] [`0020`](./0020-video-passthrough.md) — the write-side timeline this read
    side is defined against, and the reason the gap became a break.
