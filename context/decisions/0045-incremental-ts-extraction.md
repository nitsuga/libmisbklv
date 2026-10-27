---
type: Decision
title: ADR 0045 — Incremental TS KLV extraction (issue #115)
description: TsKlvStream reads a transport stream once in arbitrary chunks, with memory bounded by pending PES bytes, and matches extract_ts_klv_timed packet for packet.
decision_status: accepted
tags: [decision, extraction, streaming, pts]
generated:
  by: claude/sonnet-5-5
  at: 2026-10-27T00:00:00Z
sources:
  - id: libmisbklv-115
    resource: https://github.com/nitsuga/libmisbklv/issues/115
---

# Context

`extract_ts_klv_timed` ([`0044`](./0044-ts-timestamp-provenance.md)) takes one span over the whole stream and
makes two passes: a header-only pre-pass that finds the earliest PTS (the origin) and the wrap epoch of every PES
start, then extraction. A consumer therefore holds the whole input (or maps it, which turns a file truncated
mid-read into SIGBUS), and neither pass can be interrupted or fed from a bounded window. klvplayer needs
multi-GiB local files (nitsuga/klvplayer#27).

# Decision

Add `TsKlvStream` (`feed()` in arbitrary chunks, `finish(TsTimeline*)`). The span overloads are unchanged.

- **One pass.** The origin is the minimum PTS over the whole stream and is unknown until the end, so streamed
  packets carry `pts_ns == kNoPts`; the raw `source_pts_90k`, `pts_epoch` and the final `TsTimeline` are exactly
  what `extract_ts_klv_timed` reports, and callers compute offsets after `finish()` with `unrolled_offset_90k`.
- **Pre-pass becomes accumulators.** Each PID gathers the 14 PES header bytes the span version looked ahead for
  (same rules: non-sync slots skipped, payload-less packets skipped, the PID's next PES start ends the gather).
  Starts resolve out of order but are observed by the wrap unroller in file order, so epochs match.
- **Delivery waits for the epoch.** A finished PES is framed once every earlier PES start has resolved, in order.
  This only matters when a PES header spans several packets or a start is damaged. The wait is bounded by a
  window: a start whose 14 header bytes have not arrived within 16,384 transport packets (about 3 MB) of its start
  is resolved as untimed. Without the window one packet with a PES start and no payload would pin the oldest
  unresolved start, defer every later delivery and, after enough starts, fail the stream where the span overload
  succeeds (found in review: 70,000 KLV PES after one such packet gave 0 packets and `ResourceLimit`). This is
  the one place the result can differ from the span overload, and the difference is not confined to that PES: a
  header whose remaining bytes arrive later than the window is timed there and untimed here, so its PTS never
  reaches the wrap unroller and can change the epochs of later packets and the timeline (wrap flag, origin); a KLV
  PES in that state is framed with `source_pts_90k == -1` rather than a PTS with a guessed epoch
  (`test/ts_stream_test.cpp` pins all three).
- **Epochs are retained only while needed.** A wrap epoch is kept for a PES start only while that PES may still
  be framed (before the KLV PID is chosen: any PID; after: the KLV PID) and is erased when it is framed or
  dropped, so a file that crosses a wrap does not grow state for the rest of the input (`Stats::epoch_entries`).
- **Errors.** A terminal framing failure is sticky: `feed()` returns it, extraction stops, and timestamp scanning
  continues so `finish()` still fills the timeline for the whole stream, as the span overloads do.
- **Why not chunk the span API.** Its header says a chunk re-anchors the timeline, and a KLV PES can straddle a
  chunk boundary; a stateful stream is the only form that keeps the result identical.

# Evidence

`test/ts_stream_test.cpp` compares the stream with `extract_ts_klv_timed` on hand-built cases, the committed
fixtures and 600 seeded random streams (wraps, regressions, discontinuity indicators, header-spanning and
incomplete PES, competing candidate PIDs, bad BER lengths, truncated tails), under nine fixed chunk sizes
(1 byte to 1 MB) and random jitter. Further cases pin the damaged-start stall (70,000 KLV PES after one
payload-less start are still delivered before `finish()`), bounded epoch state over a 220,000-PES run that crosses
a wrap, and the window difference above. Deliberate defects each failed the suite: running PES jobs without
waiting for earlier starts, dropping an incomplete header at the next start, disabling the window, and never
erasing epochs. On a 253 MiB synthetic stream (warm cache) the
stream took 0.11 s to read and extract; this is one machine and one stream, not a benchmark.

# Consequences

Memory is bounded by the pending-PES allowance, the header window and the retained epochs, not by the input; the
input can be read in windows with `pread`. Besides the header-window difference, a resource limit can trip at a
different point than in the span overload because deferred PES count toward the pending-byte allowance.
