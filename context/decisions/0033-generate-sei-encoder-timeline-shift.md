---
type: Decision
title: Generate SEI matching absorbs the encoder DTS-headroom timeline shift
decision_status: accepted
tags: [decision, backend, api, insert, st0604, phase-5]
generated:
  by: openai/gpt-5
  at: 2026-08-21T23:45:58Z
fork: 30
---

# Context

Under `Sei0604::Generate` ([`0024`](./0024-sei-generation-opt-in.md)) each video
access unit is matched against the caller's KLV PTS — nearest key not ahead of
the frame, within a 200 ms window — and gets a Precision Time Stamp built from
that KLV item 2. The matcher reads raw `GST_BUFFER_PTS` off the parsed video
branch and compares it directly against the keys the caller pushed. That
comparison assumes both sides share one timeline.

They do not, for some encoders. `x264enc` (gst-plugins-ugly) and `avenc_h264`
(gst-libav) call
`gst_video_encoder_set_min_pts(GST_SECOND * 60 * 60 * 1000)` in their `start()`
— unconditionally, since GStreamer 1.6 (bug 731351; commits 698714fc,
43d6ca85) — so their output timeline is moved onto a 1000-hour minimum to
guarantee the first DTS stays non-negative. The actual adjustment is
`min_pts − first_input_pts`, and GStreamer carries it in the downstream TIME
segment. `openh264enc` does not opt in, so its output retains the input PTS.

For the common zero-starting source, the effect on Generate is total and
silent: every frame misses by 1000 hours ≫ 200 ms, no SEI is injected (`SEI
count 0 for 90 frames`), and the consume-side
eviction ([`0026`](./0026-bounded-live-klv-reassembly.md)'s bounded map, as
revised for issue #26 review) then wipes every key on the first frame because
they all sit below `frame_pts − tolerance`. Which encoders a machine has
installed decides whether Generate works at all — and CI never noticed, since
it does not install plugins-ugly and therefore only ever exercised
`openh264enc`. Issue #33 has the full reproduction.

# Decision

**The matcher detects the shift and absorbs it. On a direct-space miss whose
distance to the nearest key exceeds a 10 s threshold, the lookup converts the
buffer PTS through the downstream TIME segment to running time; if that hits
inside the normal 200 ms window, segment-derived running time is latched for
the segment — match and eviction both read the translated key thereafter. A
new SEGMENT event clears the latch and re-detects.**

- **Detection, not assumption.** A pipeline whose timelines already agree
  behaves exactly as before: a direct hit never consults the shifted space.
  Unconditional subtraction was rejected precisely because raw buffer PTS are
  meaningful on unshifted branches (file passthrough, openh264enc) and a blind
  rewrite of those would trade one broken population for another.
- **Use the observed adjustment, not the nominal minimum.** GStreamer's
  `time_adjustment` is `min_pts − first_input_pts`; subtracting the nominal
  1000-hour minimum only works when the source starts at PTS zero. The
  downstream segment reflects the actual adjustment, and converting the raw
  encoded PTS through it recovers the caller's running-time timeline for both
  zero- and non-zero-starting sources.
- **Every candidate obeys the same rule.** The retry is not a looser matcher:
  backward-only, same 200 ms window, same eviction semantics — just evaluated
  in the corrected timeline. No pairing heuristics; a wrong-timeline pipeline
  that matches nothing still matches nothing.
- **The 10 s gate** keeps ordinary unmatched frames (sparse KLV, startup skew)
  from ever reaching the retry: it sits two orders above the tolerance and five
  below the headroom constant.
- **Segment-latched under `timestamp_mu`**: once detected, subsequent frames go
  straight to segment-derived running time. A new TIME segment replaces the
  stored segment and clears the latch so a discontinuity or changed branch is
  detected afresh; the event and buffer probes share the same lock.

## Alternatives considered

- **Always match in running-time/segment space**
  (`gst_segment_to_running_time`) — the GStreamer-canonical answer, but rejected
  as the primary lookup on measured evidence from this library's own plumbing:
  at the probe pad the *working* `openh264enc` branch can carry
  `segment.start = 3 s` while its buffers start at PTS 0, so unconditional
  conversion breaks the currently-correct case. The accepted hybrid keeps the
  direct lookup first and uses segment-derived running time only after a large
  direct miss, combining the working behavior of both spaces.
- **Pin the test to a known-good encoder** — masks the product gap; any user
  with plugins-ugly installed gets silent 0-SEI output regardless of what the
  test asserts.
- **Document x264enc as unsupported for Generate** — the shift is invisible at
  the API surface and version-stable upstream, but "your encoder list decides
  whether timestamps exist" is exactly the kind of environment-dependent
  behavior the hardening phase exists to remove.

# Consequences

- `Generate` produces identical output with the tested H.264 encoders —
  verified per-encoder: the hermetic lagging-video test starts both video and
  KLV at a non-zero PTS and runs once per available encoder (`x264enc`,
  `openh264enc`, `avenc_h264`) instead of whichever the old preference order
  picked first, asserting 90/90 SEI on each. Once an encoder factory is found,
  failure to open its pipeline fails the test rather than silently skipping it.
- CI installs `gstreamer1.0-plugins-ugly`, closing the blind spot that let the
  x264enc path ship broken (#33).
- A future encoder adjustment represented correctly by its TIME segment needs
  no new hard-coded constant; it takes the same guarded fallback.
- A caller who deliberately pushes KLV on a timeline offset from the video by
  ~1000 h and relies on frames *not* being stamped loses that (unspecified)
  behavior; no known consumer does this.

# Assumptions / open questions

- The encoder exposes its adjustment through the downstream TIME segment, as
  `GstVideoEncoder` does from 1.6 through 1.26; an absent or unusable segment
  fails closed (no latch and no segment-space eviction), never mispairs.
- Frames arrive in decode order on the shifted branches exactly as on
  unshifted ones; the backward-only window tolerates small reorder either way.

# Citations

[1] [`0024`](./0024-sei-generation-opt-in.md) — Generate mode and the
    PTS-matching contract amended here.
[2] GStreamer bug 731351 → commit `698714fc` (x264enc shifts PTS+DTS),
    bug 740575 → commit `dc7b2548` (`gst_video_encoder_set_min_pts`),
    commit `43d6ca85` (x264enc switches to it); identical call in gst-libav's
    `avenc_h264`.
[3] Issue #33 — reproduction, encoder matrix, and the CI plugin-set blind spot.
