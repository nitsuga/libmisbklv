---
type: Decision
title: Live video liveness uses a last-delivery timestamp
decision_status: accepted
tags: [decision, api, backend, streaming, phase-5]
generated:
  by: openai/gpt-5
  at: 2026-09-05T03:15:18Z
sources:
  - id: libmisbklv-60
    resource: https://github.com/nitsuga/libmisbklv/issues/60
    title: Expose video-source liveness for live consumers
fork: 34
---

# Context

A live insertion consumer needs to tell whether its video source is still
delivering while the KLV branch remains open. Pipeline-level EOS cannot answer
that question: `mpegtsmux` waits for EOS on every sink pad, and the KLV appsrc
does not receive EOS until `Inserter::finish()`.[^libmisbklv-60]

The video branch's EOS event can reach its reserved muxer sink pad independently
in some pipeline states, so fork 34 considered exposing that event, the time of
the most recent video delivery, or both.

An `appsrc` experiment matching the production topology settled the uncertain
case on GStreamer 1.24.2. A finite `videotestsrc` / H.264 branch alongside an
open KLV appsrc delivered video buffers to the reserved mux pad but did not
deliver its EOS within three seconds. The result was the same with no KLV and
with one timestamped KLV buffer followed by starvation. `mpegtsmux`
backpressure can therefore prevent the branch EOS needed by a liveness watcher.

# Decision

Expose only the reliable signal:

- `Inserter::last_video_delivery()` returns the monotonic
  `std::chrono::steady_clock::time_point` when the most recent video buffer
  reached the muxer, or `std::nullopt` before the first delivery and for a
  backend without video progress.
- `KlvSink::last_video_delivery()` forwards the same query through the
  high-level facade.
- The existing mux-pad delivery probe updates the timestamp for file, RTSP, and
  `pipeline:` sources and continues to latch first-buffer readiness for live
  close.
- Consumers choose their own idle threshold by comparing the returned value
  with `std::chrono::steady_clock::now()`.

Do not expose video-branch EOS. It is not reliable under the starvation state
the API exists to detect, and `finish()` deliberately pushes EOS through an
unbounded live video branch, which would make a bare EOS latch ambiguous
between source termination and caller-requested shutdown.

# Alternatives considered

- **Video-branch EOS only** — rejected because muxer backpressure can stop the
  finite branch before its EOS reaches the reserved pad. It cannot cover a
  stalled or starved source.
- **Timestamp plus video-branch EOS** — rejected because the timestamp is still
  required for the failure case, while EOS adds public surface and ambiguous
  shutdown semantics without covering another required state.
- **A fixed idle timeout inside the library** — rejected because acceptable
  silence depends on the source and consumer. The library reports progress;
  the caller owns policy.
- **Callbacks or a watcher thread** — rejected because consumers already have
  an ingest or polling loop, and the atomic timestamp needs no extra lifetime
  coordination.

# Consequences

- A live consumer can detect both a source that never starts delivering and one
  that later stalls, including when the KLV branch prevents EOS propagation.
- The returned time is monotonic process time, not media PTS or wall-clock time;
  it is useful only for elapsed-time comparisons in the current process.
- The accessor itself is safe to read while the GStreamer streaming thread
  updates it. Object lifetime must still outlive the call.
- All video-source kinds report progress consistently, including bounded file
  sources; KLV-only and default backend implementations return `nullopt`.
- The public virtual interface grows by one defaulted method. This preserves
  source compatibility for custom backends but is a pre-1.0 binary-interface
  change.

# Assumptions / open questions

- Buffer arrival at the reserved mux sink pad remains the definition of useful
  video delivery: it proves the source branch has negotiated and advanced as
  far as the muxer.
- If a future backend can distinguish source-originated EOS reliably and a
  consumer needs that distinction, it should open a new fork rather than
  changing this timestamp's meaning.

# Citations

[^libmisbklv-60]: [libmisbklv#60](https://github.com/nitsuga/libmisbklv/issues/60)
    — consumer cases, candidate signals, existing probe location, and the
    backpressure question resolved here.

[1] [ADR 0035](./0035-live-insert-terminal-status.md) — why `poll()` reports
    backend failure but cannot expose aggregate EOS before `finish()`.
[2] [ADR 0034](./0034-live-request-pad-teardown.md) — why the live video pad
    stays linked through EOS drain.
