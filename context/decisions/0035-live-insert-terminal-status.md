---
type: Decision
title: Nonblocking terminal status for asynchronous insert sources
decision_status: accepted
tags: [decision, api, backend, streaming, phase-5]
generated:
  by: claude/opus-5
  at: 2026-08-29T00:38:10Z
sources:
  - id: libmisbklv-58
    resource: https://github.com/nitsuga/libmisbklv/issues/58
    title: Expose terminal status for live insertion consumers
  - id: parrot-to-klv-94
    resource: https://github.com/nitsuga/parrot-to-klv/issues/94
    title: Live pipeline errors are not detected when no KLV is received
  - id: libmisbklv-59
    resource: https://github.com/nitsuga/libmisbklv/pull/59
    title: Review narrowing the terminal-status contract to failure only
fork: 32
---

# Context

The high-level insertion facade exposes `KlvSink::error()` for failures that
occur while opening a session, but it has no nonblocking way to observe a
terminal GStreamer `ERROR` after opening. `KlvSink::close()` is a
finalizing operation and can be reached only after the consumer's ingest loop
has already stopped. A live consumer that receives no KLV packets can
therefore wait for telemetry, an external stop, or the close drain before it
notices that its video pipeline failed.[^parrot-to-klv-94]

The downstream report and the upstream API discussion are tracked separately so
the library contract can be reviewed independently of parrot-to-klv's polling
loop.[^libmisbklv-58]

# Decision

Add a nonblocking terminal-failure query to the insertion interface:

- `Inserter::poll()` returns `ok()` while no terminal failure has been observed
  and an error when the backend reports one.
- `KlvSink::poll()` forwards that operation through the high-level facade.
- The base `Inserter` implementation returns `ok()`, so synchronous and custom
  backends do not need an asynchronous source merely to implement the
  interface.
- The GStreamer inserter consumes `ERROR` messages without blocking and latches
  the result. A later `finish()` returns the latched error instead of losing it
  during its normal drain.
- Callers serialize `poll()`, `push()`, and `finish()`; the operation is
  nonblocking, not a thread-safety guarantee.

**Clean EOS is deliberately not reported.** An earlier draft returned
`Result<bool>`, where `true` meant EOS had been observed. That state is
unreachable before `finish()`: `mpegtsmux` is a `GstAggregator`, so it forwards
EOS downstream only once *every* sink pad has it, the pipeline's single sink
posts `GST_MESSAGE_EOS` only on receiving that event, and the KLV appsrc pad is
EOS'd in exactly one place — `finish()` itself. A consumer therefore cannot
observe EOS through `poll()` at any point where the answer would still be
useful.[^libmisbklv-59]

Two experiments on GStreamer 1.24.2 confirm it. A bounded video branch feeding
the same muxer as a live branch produced no bus EOS in 20 seconds, though the
bounded branch ended in about one. The library's own shape — a `meta/x-klv`
appsrc that never pushes, alongside a bounded video branch — likewise produced
no bus EOS in 20 seconds, and the muxer emitted nothing at all, because it was
still waiting on the KLV pad.

So `poll()` answers only the question a live consumer can actually ask: has the
backend failed? `Result<std::monostate>` says exactly that and nothing more. The
GStreamer implementation pops `GST_MESSAGE_ERROR` alone, which also keeps
`poll()` from consuming the EOS that `finish()`'s drain waits for.

# Alternatives considered

- **Expose the GStreamer pipeline to parrot-to-klv** — rejected; it would make
  a consumer depend on the backend's private representation and lose the
  high-level facade's backend-neutral contract.
- **Change `KlvSink::error()` into a live terminal-state query** — rejected;
  the existing operation reports open-time failure and does not represent
  clean EOS. Changing its meaning would make callers distinguish two unrelated
  states through one operation.
- **Wait in `close()` or add a blocking `wait()`** — rejected; a live consumer
  needs to stop promptly while it is still ingesting and may not have any KLV
  packet to push.
- **Add callbacks or a background watcher** — rejected for this need; it adds
  thread and lifetime coordination when the consumer already owns a polling
  loop.
- **Make `poll()` pure virtual** — rejected; the default keeps synchronous and
  existing custom backends source-compatible while they have no terminal event
  to report.
- **Report clean EOS as `Result<bool>`** — rejected as unreachable; see
  **Decision**. Keeping it would have frozen a `true` case no caller can reach,
  along with the `finish()` fast path it gated — a path that, had it ever run,
  would have skipped both the appsrc EOS and the muxer drain while still
  keeping the output file.

# Consequences

- Live consumers can notice a backend error before the first KLV packet, rather
  than at `close()` or not at all.
- The public insertion interface gains a method. Subclasses inherit a safe
  default, but adding a virtual method is not binary-compatible with an already
  built pre-1.0 implementation.
- GStreamer `ERROR` messages are consumed by `poll()` and retained so a later
  `finish()` cannot turn an observed failure into apparent success. `EOS`
  remains untouched on the bus for `finish()` to drain.
- Synchronous backends report `ok()` forever unless they override the
  operation; they are not required to invent an asynchronous terminal state.
- The first version reports terminal backend failures as `Error::Backend`; it
  does not add a new status enum or expose backend-specific error text.

# Assumptions / open questions

- `poll()`, `push()`, and `finish()` are called from one serialized consumer
  context. A future thread-safe status monitor would be a separate API choice.
- `Error::Backend` is sufficient for the first consumer. If callers need
  backend error detail without consulting logs, the result type should be
  revisited rather than adding ad hoc accessors.
- EOS unreachability is a property of the current muxer-based insert pipeline.
  A backend whose sink can reach EOS independently of the KLV pad would make
  clean-EOS reporting meaningful again — that is a new fork, to be opened when
  such a backend and a caller for it both exist.
- The pre-1.0 compatibility tradeoff is acceptable; the first stability
  boundary should document the public virtual-interface change.

# Citations

[^parrot-to-klv-94]: [parrot-to-klv#94](https://github.com/nitsuga/parrot-to-klv/issues/94)
    — downstream live-ingest failure and the no-telemetry reproduction.
[^libmisbklv-58]: [libmisbklv#58](https://github.com/nitsuga/libmisbklv/issues/58)
    — upstream API request and review scope.
[^libmisbklv-59]: [libmisbklv#59](https://github.com/nitsuga/libmisbklv/pull/59)
    — review thread carrying the `gst-launch-1.0` runs and the `gst-inspect-1.0`
    class hierarchy behind the EOS-unreachability finding.

[1] [ADR 0013](./0013-media-backend-interface.md) — the backend interface
    whose insertion contract is extended.
[2] [ADR 0018](./0018-high-level-api.md) — the `KlvSink` facade this proposal
    keeps backend-neutral.
[3] [ADR 0032](./0032-cancellable-insert-drain.md) — the existing cooperative
    cancellation and bounded drain contract for insertion.
