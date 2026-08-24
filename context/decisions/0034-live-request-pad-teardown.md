---
type: Decision
title: Live video request pads remain linked through EOS drain
decision_status: accepted
tags: [decision, backend, gstreamer, streaming, teardown, phase-5]
generated:
  by: openai/gpt-5
  at: 2026-08-24T00:49:35Z
fork: 31
---

# Context

The unbounded live-video insert path could not finish by waiting for its source
to end: RTSP and `pipeline:` sources without a finite buffer count never produce
EOS on their own. The original workaround ended the KLV `appsrc`, then unlinked
the video peer and released its `mpegtsmux` request pad while the pipeline was
still `PLAYING`.

That release raced `mpegtsmux`'s streaming thread. Issue #39 captured the near
failure under `MALLOC_PERTURB_`: `g_list_last` traversed a request-pad-list node
that `gst_element_release_request_pad` had just freed. Without allocator poison,
the same corruption surfaced later during thread teardown and was originally
tracked as the residual in parrot-to-klv#57 after the separate SEI-probe
lifetime fix ([ADR 0024](./0024-sei-generation-opt-in.md)).

The diagnostic branch for PR #40 avoided the release, but skipped the live drain
and declared success immediately. That produced empty output while hiding bus
errors. It also shortened the global drain guard and waited for `GST_STATE_NULL`
without a deadline, regressing the bounded/cancellable finish contract from
[ADR 0032](./0032-cancellable-insert-drain.md).

# Decision

**Keep the video request pad linked and owned by `mpegtsmux` until pipeline
destruction. End an unbounded live video branch by pushing EOS downstream from
its final source pad, then use the same bounded, error-aware drain as every
other insert session.**

The finish sequence is:

1. End the KLV `appsrc` and check whether it accepted EOS.
2. For an unbounded live video branch, wait up to one second for its final pad
   to negotiate caps and deliver a buffer. This covers a caller that closes
   immediately after opening while ensuring `mpegtsmux` has a real video stream
   handler. Failure to become usable is an insertion failure, not permission to
   silently emit KLV-only output.
3. Push EOS from that live source pad while it remains linked to the reserved
   muxer pad. Never call `gst_pad_unlink` or
   `gst_element_release_request_pad` during `PLAYING`.
4. Drain to a clean pipeline EOS, surface a bus error, or honor the caller's
   stop token. Retain the five-minute stall guard; it is not a performance
   budget and applies equally to bounded live, file, and KLV-only pipelines.
5. Sever the ST 0604 probes, request `GST_STATE_NULL`, and confirm NULL for at
   most five seconds. A failed or incomplete transition makes `finish()` fail;
   there is no unbounded state wait.
6. Preserve a newly created output file only after clean EOS and successful
   quiescence. Cancellation remains success but removes partial file output,
   per [ADR 0022](./0022-no-output-on-failure.md) and ADR 0032.

The reserved request pad needs no explicit release: destroying the NULL
pipeline destroys `mpegtsmux` and its pads together, after streaming activity
has stopped.

# Alternatives considered

- **Release the request pad while `PLAYING`** — rejected; this is the reproduced
  `mpegtsmux` list use-after-free.
- **Move unlink/release after the drain** — rejected as unnecessary. Leaving an
  endless live pad open prevents the muxer from reaching EOS, while releasing
  it adds lifecycle machinery that pipeline destruction already owns.
- **Skip the drain and treat live KLV EOS as success** — rejected. It can keep
  an empty file, lose queued network output, and suppress a terminal bus error.
- **Shorten the global drain timeout** — rejected. A short diagnostic deadline
  breaks legitimate bounded pipelines and changes unrelated behavior.
- **Wait indefinitely for NULL** — rejected. It bypasses the stop token and can
  hang `close()` or destruction forever; teardown confirmation is bounded.
- **Pause, release, resume, then drain** — rejected. It adds another state
  transition and request-pad mutation when a serialized EOS event provides the
  normal GStreamer end-of-stream mechanism.

# Consequences

- The request-pad UAF is removed without weakening output completeness or
  terminal-error reporting.
- A normal unbounded live close produces drained video plus byte-exact KLV; an
  already-requested cancellation returns promptly and leaves no file.
- Immediate close can wait briefly for the live video branch to become usable.
  A branch that cannot negotiate or deliver data fails rather than degrading to
  an undocumented KLV-only stream.
- The full insert surface keeps one drain contract and one stall guard. No live
  special case is allowed to manufacture success without pipeline EOS.

# Assumptions / open questions

- The one-second readiness bound is an operational guard for a branch already
  in `PLAYING`; it is separate from RTSP connection/open time. Increase it only
  with evidence that a valid connected source routinely negotiates more slowly.
- Allocator-stress validation remains valuable because the original corruption
  was timing-sensitive. The deterministic semantic regressions are covered by
  the hermetic live-video tests; downstream parrot-to-klv stress remains the
  integration witness for the original report.

# Citations

[1] [Issue #39](https://github.com/nitsuga/libmisbklv/issues/39) — allocator
    evidence and the `mpegtsmux` request-pad-list failure.
[2] [ADR 0024](./0024-sei-generation-opt-in.md) — the earlier, distinct
    SEI-probe lifetime fix from parrot-to-klv#57.
[3] [ADR 0022](./0022-no-output-on-failure.md) — output-file disposition.
[4] [ADR 0032](./0032-cancellable-insert-drain.md) — bounded, cooperative
    insert drain cancellation.
[5] [ADR 0031](./0031-live-streaming-surface.md) — the live video-source
    surface whose teardown is refined here.
