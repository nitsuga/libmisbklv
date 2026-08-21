---
type: Decision
title: Generate SEI matching absorbs the encoder DTS-headroom timeline shift
decision_status: accepted
tags: [decision, backend, api, insert, st0604]
generated:
  by: ox-alpha
  at: 2026-08-21T00:00:00Z
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
43d6ca85) — so their entire output timeline is shifted forward by 1000 hours
(3.6e15 ns) to guarantee the first DTS stays non-negative. `openh264enc` does
not opt in, so its output starts at PTS 0.

The effect on Generate is total and silent: every frame misses by 1000 hours ≫
200 ms, no SEI is injected (`SEI count 0 for 90 frames`), and the consume-side
eviction ([`0026`](./0026-bounded-live-klv-reassembly.md)'s bounded map, as
revised for issue #26 review) then wipes every key on the first frame because
they all sit below `frame_pts − tolerance`. Which encoders a machine has
installed decides whether Generate works at all — and CI never noticed, since
it does not install plugins-ugly and therefore only ever exercised
`openh264enc`. Issue #33 has the full reproduction.

# Decision

**The matcher detects the shift and absorbs it. On a direct-space miss whose
distance to the nearest key exceeds a 10 s threshold, the lookup retries in the
shifted space (`pts − kEncoderDtsHeadroomNs`); if that hits inside the normal
200 ms window, the shifted space is latched for the session — match and
eviction both read the translated key thereafter.**

- **Detection, not assumption.** A pipeline whose timelines already agree
  behaves exactly as before: a direct hit never consults the shifted space.
  Unconditional subtraction was rejected precisely because raw buffer PTS are
  meaningful on unshifted branches (file passthrough, openh264enc) and a blind
  rewrite of those would trade one broken population for another.
- **Every candidate obeys the same rule.** The retry is not a looser matcher:
  backward-only, same 200 ms window, same eviction semantics — just evaluated
  in the corrected timeline. No pairing heuristics; a wrong-timeline pipeline
  that matches nothing still matches nothing.
- **The 10 s gate** keeps ordinary unmatched frames (sparse KLV, startup skew)
  from ever reaching the retry: it sits two orders above the tolerance and five
  below the headroom constant.
- **Session-latched under `timestamp_mu`**: once detected, subsequent frames go
  straight to the shifted key; the probe runs on the streaming thread, so no
  new synchronization.

## Alternatives considered

- **Match in running-time/segment space** (`gst_segment_to_running_time`) —
  the GStreamer-canonical answer ("timestamps have no meaning without their
  segment"), and it would be invariant by construction. Rejected on measured
  evidence from this library's own plumbing (issue #33 investigation): at the
  probe pad the SEGMENT event is already rewritten by intervening live-branch
  elements — the *working* `openh264enc` branch carries `segment.start = 3 s`
  while its buffers start at PTS 0 — so running-time conversion breaks the
  currently-correct case to fix the shifted one.
- **Pin the test to a known-good encoder** — masks the product gap; any user
  with plugins-ugly installed gets silent 0-SEI output regardless of what the
  test asserts.
- **Document x264enc as unsupported for Generate** — the shift is invisible at
  the API surface and version-stable upstream, but "your encoder list decides
  whether timestamps exist" is exactly the kind of environment-dependent
  behavior the hardening phase exists to remove.

# Consequences

- `Generate` produces identical output regardless of which H.264 encoder is
  installed — verified per-encoder: the hermetic lagging-video test now runs
  once per available encoder (`x264enc`, `openh264enc`, `avenc_h264`) instead
  of whichever the old preference order picked first, asserting 90/90 SEI on
  each.
- CI installs `gstreamer1.0-plugins-ugly`, closing the blind spot that let the
  x264enc path ship broken (#33).
- If a future encoder shifts by a different constant, its stream simply fails
  the 10 s gate + exact-window retry and behaves as today (no SEI) rather than
  mispairing — the mechanism degrades loudly-in-tests, not silently-wrong.
- A caller who deliberately pushes KLV on a timeline offset from the video by
  ~1000 h and relies on frames *not* being stamped loses that (unspecified)
  behavior; no known consumer does this.

# Assumptions / open questions

- The headroom constant is stable upstream (unchanged 1.6 → 1.26, confirmed in
  source); if it ever changes, detection fails closed (no latch, current miss
  behavior), never mispairs.
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
