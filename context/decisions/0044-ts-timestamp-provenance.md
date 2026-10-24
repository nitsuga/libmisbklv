---
type: Decision
title: ADR 0044 — Timestamp provenance from extract_ts_klv (issue #111)
description: extract_ts_klv_timed reports the source PTS, normalization origin, and wrap/discontinuity status alongside the unchanged pts_ns.
decision_status: accepted
tags: [decision, extraction, timing, pts]
generated:
  by: claude/sonnet-5-5
  at: 2026-10-24T00:00:00Z
sources:
  - id: libmisbklv-111
    resource: https://github.com/nitsuga/libmisbklv/issues/111
---

# Context

[`0021`](./0021-read-path-timestamps.md) gave `KlvPacket::pts_ns` (ns from the
earliest PTS in the buffer) but not the source-domain facts behind it, and left
the 33-bit wrap undetected ([`0039`](./0039-extraction-scope-and-error-policy.md)
item 6). A consumer combining KLV with independently decoded video had to
re-parse PES/TS headers or guess the origin.

# Decision

Add `extract_ts_klv_timed(ts, TsPacketHandler, TsTimeline*)`; `extract_ts_klv`
is a wrapper over it, so framing, errors and `pts_ns` are byte-for-byte
unchanged.

- **Per packet** (`TsKlvPacket`): the raw 33-bit `source_pts_90k` of the PES
  that carried the packet's first byte (-1 when untimed), carried through the
  framer rather than recovered from `pts_ns`, and `pts_epoch`, the wrap count
  of that PES in one file-order numbering across all PIDs.
  `source + epoch * 2^33` is the unrolled source time.
- **Per extraction** (`TsTimeline`): `origin_pts_90k` and `origin_pts_epoch`
  (the source tick `pts_ns == 0` is, and its epoch), `pts_wrap_observed`,
  `pts_regression_observed`, `discontinuity_indicator_observed`, and
  `Continuity` = `NoTimestamps` / `Continuous` / `Interrupted`. Unrolled packet
  offset from the origin is exact across a wrap and aligns with video on the
  same program clock.
- **`pts_ns` is not "fixed" across a wrap.** Compatibility was an explicit
  requirement; the flags make the existing limitation visible instead.
- **One scan, same parse as extraction.** The pre-pass reads each PES's PTS
  from the same bytes extraction does, including a header that spans TS
  packets, so `NoTimestamps` never describes a buffer that has a timed PES.
- **Conservative detection.** Wrap uses a half-range rule over PES in file
  order across all PIDs: a backward step over 2^32 ticks is a wrap, and a
  forward step that large is a PES reordered from before one. Both mark the
  buffer as straddling a wrap. A step back of more than 30 s that is not a wrap
  is a regression. `discontinuity_indicator` on any adaptation field marks
  `Interrupted`. Independent clocks in a multi-program buffer, or a muxer that
  sets the indicator on a PID's first packet, can trip these; both lean toward
  `Interrupted`. `Continuous` is scoped to the supplied buffer, and a forward
  splice or a backward one inside the tolerance is not detectable from PTS.
- **Mid-stream buffers** anchor at their own earliest PTS; `origin_pts_90k`
  names that source tick, so a consumer can reconcile two extractions.
- **`NoTimestamps` vs untimed PES.** `NoTimestamps` means no PES anywhere had a
  PTS; a single untimed PES in a timed stream is `kNoPts` with `Continuous`.

# Alternatives considered

- **Unroll `pts_ns` across the wrap.** Rejected here: changes existing output,
  against the issue's compatibility requirement. The epoch makes it a one-line
  consumer computation.
- **New fields on `KlvPacket`.** Rejected: the gstreamer backend shares the
  struct and cannot supply a source PTS, so the fields would mean "unknown" on
  that path with no way to say so.

# Consequences

- gstreamer-backed extraction gets no provenance; the offline extractor is the
  only path that can see raw PES headers.
- Tests: `ts_timeline` (synthetic non-zero origin, no-timestamp, split PES header, wrap,
  video-first wrap, reordered-near-wrap, regression, discontinuity indicator
  including a continuation packet, 0xFC cells, error path, mid-stream buffer).
