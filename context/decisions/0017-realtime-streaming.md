---
type: Decision
title: Real-time streaming — live-sink pacing + live-source idle-timeout
decision_status: accepted
tags: [decision, backend, gstreamer, realtime, udp, srt, phase-3]
generated:
  by: claude/opus-5
  at: 2026-07-19T05:00:00Z
fork: 15
---

# Context

Backend phase B4 (the last of the [`backend-scope`](../backend-scope.md)
plan) and the real-time half of ADR [`0008`](./0008-media-backend-gstreamer.md)'s
goal: push KLV to, and pull KLV from, a live network endpoint (`udp`/`srt`), not
just a file. Two gaps remained after B1–B3:

- **Insertion** (B2) muxed to a `filesink` as fast as the sink drained — no
  notion of wall-clock pacing. A live sink must emit at stream rate.
- **Extraction** (B1) only drove `filesrc`, which ends at EOS. A live source
  never sends EOS, so a blocking `extract()` over `udp`/`srt` would hang forever
  with no way to know the stream is over.

# Decision

**Add a live streaming path in `GstBackend`, gated by scheme + an `InsertConfig`
flag; the `MediaBackend` interface is unchanged except for that one field.**

- **Live-sink pacing** — `InsertConfig::realtime`. When set, `open_insert` marks
  the `appsrc` `is-live=TRUE` and the sink `sync=TRUE`, so buffers render on the
  pipeline clock at their PTS (the encoder already stamps ~30 fps, ADR
  [`0011`](./0011-encode-model.md)). `push()` already blocks on `appsrc`
  backpressure (`block=TRUE`); with `is-live` that backpressure is clock-driven —
  the ergonomic "push blocks at real-time rate" win. Default `false` keeps the
  fast filesink round-trip (B2).
- **Live-source termination via idle timeout** — `extract()` parses the source:
  a bare path / `file:` uses `filesrc` (ends at EOS, unchanged); `udp:host:port`
  / `srt:uri` use `udpsrc`/`srtsrc` and are *live*. `udpsrc timeout` posts a
  `GstUDPSrcTimeout` ELEMENT message when the socket idles; the bus loop treats
  that as end-of-stream **only after at least one whole KLV packet has been
  delivered** — so an early timeout during receiver startup (before the sender is
  up) is ignored and cannot truncate the stream. Timeout = 500 ms (> the ~33 ms
  inter-packet gap, so it never trips mid-stream).

Verified by `gst_stream_test` (B4): a one-process udp loopback — `extract()` on
a receiver thread, realtime `open_insert`/`push` on the main thread — recovers
the KLV **byte-exact**, and the 6-packet send takes ~165 ms (5 × 33 ms),
confirming the output is clock-paced, not fast-pushed.

# Update (2026-07-31)

The idle-timeout behavior applies to UDP: `udpsrc` emits the timeout message.
`srtsrc` has no equivalent idle termination, so SRT extraction ends through
cooperative cancellation or an external source termination/error. The deferred
stop-token alternative and the immediate-stop consequence above are superseded
by [ADR 0019](./0019-extract-cancellation.md).

# Alternatives considered

- **A stop token / `bool`-returning handler to end extraction** — a cleaner
  general API, but changes the ADR 0013 `PacketHandler` signature for every
  backend and still needs the caller to know when to stop. The idle timeout is
  self-contained in the live path and needs no interface change. A cooperative
  stop can be added later if a caller needs to end extraction early (mid-stream).
- **EOS over the wire** (rely on the sender's TS EOS) — udp carries no EOS; SRT
  can signal disconnect but not uniformly. An idle timeout works for both.
- **`sync=TRUE` always** (pace even filesink) — rejected: slows the file
  round-trip and couples it to the clock for no benefit; pacing only matters for
  live sinks, so it is opt-in.
- **RTP payloading** (`rtpmp2tpay`/`rtpmp2tdepay`) instead of raw TS-over-udp —
  deferred: raw MPEG-TS over udp is the common MISB carriage and round-trips
  byte-exact here; RTP is a later option if a consumer needs it.

# Consequences

- **Real-time streaming works end-to-end** over `udp` (and `srt`, same code path)
  — insertion clock-paced, extraction self-terminating — closing the ADR 0008
  real-time goal. B0–B4 complete.
- **The core interface barely moved** — one `InsertConfig` field; `MockBackend`
  ignores it. Read-borrows/`KlvPacket` boundary (ADR 0013) is unchanged.
- **A live `extract()` blocks up to one idle-timeout past the last packet**
  (500 ms) before returning — acceptable for a drain-to-end pull; a caller
  wanting immediate stop is the deferred stop-token follow-on.
- **Loss/reordering**: raw TS over udp is lossy; the loopback test is reliable
  and byte-exact, but a real lossy network can drop packets (SRT's ARQ mitigates
  this — it is wired via the same `srt:` scheme).

# Assumptions / open questions

- **udp loopback is lossless** for the test's volume (a few KB) — true in
  practice; the test binds the receiver before sending (700 ms startup wait) so
  the PAT/PMT-bearing first datagram isn't missed.
- **SRT** shares the code path but is exercised only indirectly (no CI SRT test —
  handshake/timing is harder to make hermetic); `udp` is the covered live path.
- **PTS on extraction** stays `kNoPts` (PES PTS unreliable, ADR
  [`0009`](./0009-st0604-deferred.md)); correlation uses KLV Item 2.
  **Superseded by [`0021`](./0021-read-path-timestamps.md)**: extraction reports
  nanoseconds from the start of the source, and `kNoPts` now means the stream
  carried no timestamp. Nothing else here changes — pacing still runs off the
  per-buffer PTS.

# Citations

[1] [`backend-scope`](../backend-scope.md) — B4 plan (real-time
    streaming) and the udp/srt element inventory.
[2] [`0008`](./0008-media-backend-gstreamer.md) — real-time insertion via
    `appsrc` is a stated v1 goal.
[3] [`0013`](./0013-media-backend-interface.md) — `MediaBackend`/`Inserter`
    contract this extends (one `InsertConfig` field).
[4] gstreamer `udpsrc` `timeout` property (posts `GstUDPSrcTimeout` ELEMENT
    message on idle); `appsrc` `is-live` + `GstBaseSink` `sync` for clock pacing.
