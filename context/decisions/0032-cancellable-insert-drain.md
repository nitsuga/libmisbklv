---
type: Decision
title: Cooperative insert-drain cancellation (stop token on finish/close)
decision_status: accepted
tags: [decision, backend, api, streaming, insert]
generated:
  by: claude/opus-4-8
  at: 2026-08-21T00:00:00Z
fork: 29
---

# Context

[`0019`](./0019-extract-cancellation.md) made the **read** path cancellable — a
`std::stop_token` on `MediaBackend::extract`, polled every 100 ms, so a
`KlvStream` consumer can break out of an endless live source. The **write** path
got no equivalent, and it needs one for the same reason.

`Inserter::finish()` sends EOS on the KLV `appsrc`, then blocks in a bus loop
until the whole pipeline drains — `EOS`, an `ERROR`, or `kFinishDrainTimeout`
(5 min). For a **realtime file replay** (`realtime=true` + a `file:` video
source: the `-i file.mp4 -o udp:/srt:` bridge in [`0031`](./0031-live-streaming-surface.md)),
the video branch renders on the pipeline clock — it drains at **wall-clock
speed**. So once a caller has pushed its last KLV packet, `finish()` sits inside
that drain for the remaining real duration of the video, checking nothing but
the clock.

A caller cannot interrupt it. The concrete symptom, from a downstream consumer:
`parrot-to-klv -i file.mp4 -o udp://…`, Ctrl-C mid-replay does **nothing** — the
process ignores it until the source reaches EOS (or, if the video outlives its
metadata and stalls, up to the 5-minute drain timeout). The SIGINT is a
cooperative flag the caller's emit loop honors between KLV samples, but `finish()`
is downstream of that loop and never looks. Reproduced: a 10 s video with 0.27 s
of metadata, Ctrl-C at t=3 s, process exits at t=10 s — ~7 s of dead Ctrl-C.

# Decision

**Thread a `std::stop_token` through `finish()` and `KlvSink::close()`, polled in
the drain loop — the same shape [`0019`](./0019-extract-cancellation.md) gave
`extract()`.**

- Interface (amends [`0013`](./0013-media-backend-interface.md)):
  `Inserter::finish(std::stop_token stop = {})` and, at the facade,
  `KlvSink::close(std::stop_token stop = {})`. A default-constructed token is
  never signaled — **every existing caller is unchanged** and drains to the
  natural EOS.
- `GstInserter::finish`: the drain poll, previously capped at 1 s, is capped at
  **100 ms** (`kFinishDrainPoll`), and each pass checks `stop.stop_requested()`
  before polling. On request it breaks, `set_state(NULL)`, and — because the
  session did not complete — **discards any partial sink file** (ADR 0022: no
  output unless the session succeeded).
- **Cancellation returns `ok`**, not an error: it is a caller request, not a
  backend fault. This matches `extract()`'s cooperative-stop convention
  ([`0019`](./0019-extract-cancellation.md)) — a cancelled read returns ok too.
  A cancelled `file:` sink therefore leaves no half-written `.ts`; a cancelled
  live sink simply stops.
- `MockInserter::finish` takes the token (ignores it — it does no draining), so
  the interface change compiles across both backends.

Verified: a realtime replay of the 2 s synthetic TS source, one KLV packet
pushed, then `finish()` with an **already-requested** stop returns in **0 ms**
(vs the ~2 s a full drain takes) and leaves no output file
(`gst_video_insert_test`, "realtime drain cancellation"). Teeth: without the
`stop_requested()` check the same call blocks ~2 s and the "<1 s" assertion
fails.

# Alternatives considered

- **A raw flag / callback into `finish()`** — works, but `std::stop_token` is
  already the vocabulary type this library cancels with ([`0019`](./0019-extract-cancellation.md)),
  and pairs with `std::stop_source`/`std::jthread`. Using it keeps one
  cancellation idiom, not two.
- **Post an EOS/custom message on the bus to wake the poll instantly** — removes
  the ≤100 ms latency but adds gst-specific plumbing; teardown latency is
  irrelevant here, exactly as [`0019`](./0019-extract-cancellation.md) judged.
- **Fix it in the consumer (parrot-to-klv) by tearing the pipeline down on
  SIGINT** — the caller holds only the abstract `KlvSink`; it cannot reach the
  gst pipeline mid-`close()`. Cancellation must travel through the interface, the
  same conclusion [`0019`](./0019-extract-cancellation.md) reached for `extract`.
- **Shorten `kFinishDrainTimeout`** — a shorter cap would still ignore the caller
  for that whole cap, and truncates legitimate long drains. Orthogonal to
  responsiveness.

# Consequences

- A realtime replay is interruptible at every point: the emit loop already
  honored the caller's stop between samples; `close(stop)` now honors it during
  the drain too. The [`0031`](./0031-live-streaming-surface.md) file-replay bridge
  is stoppable end to end.
- `MediaBackend`/`Inserter` implementers gain a defaulted parameter — no existing
  call site changes; the mock and gst backends both thread it.
- Cancel latency is ≤100 ms (the poll interval), down from a 1 s poll — a non-
  issue for teardown, and it also tightens the pre-existing deadline check.
- A cancelled insert discards its partial output, consistent with a failed one
  (ADR 0022). A caller that wants to *keep* a partial file must drain to EOS
  (pass no token), which is the default.

# Assumptions / open questions

- **Cooperative, not preemptive**: the stop is observed at the drain poll and
  between the emit loop's samples. A single `push()` blocked on backpressure
  delays observation until it returns; pushes are expected to be short.
- Bridging a signal handler's async flag to a `std::stop_source` is the caller's
  job (a handler cannot call `request_stop()` async-safely). The intended pattern
  is a small watcher that forwards the flag to a `stop_source` whose token is
  passed to `close()`.

# Citations

[1] [`0013`](./0013-media-backend-interface.md) — the `Inserter` contract amended.
[2] [`0019`](./0019-extract-cancellation.md) — the read-path precedent: stop
    token, 100 ms poll, cancel-as-success.
[3] [`0031`](./0031-live-streaming-surface.md) — the realtime file-replay sink
    whose drain this makes interruptible. [`0022`](./0022-no-output-on-failure.md)
    — output disposition on a session that did not complete.
[4] C++20 `std::stop_token` / `std::stop_source` (`<stop_token>`).
