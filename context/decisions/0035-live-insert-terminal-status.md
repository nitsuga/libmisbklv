---
type: Decision
title: Nonblocking terminal status for asynchronous insert sources
decision_status: proposed
tags: [decision, api, backend, streaming, phase-5]
generated:
  by: openai/gpt-5
  at: 2026-08-28T23:05:35Z
sources:
  - id: libmisbklv-58
    resource: https://github.com/nitsuga/libmisbklv/issues/58
    title: Expose terminal status for live insertion consumers
  - id: parrot-to-klv-94
    resource: https://github.com/nitsuga/parrot-to-klv/issues/94
    title: Live pipeline errors are not detected when no KLV is received
fork: 32
---

# Context

The high-level insertion facade exposes `KlvSink::error()` for failures that
occur while opening a session, but it has no nonblocking way to observe a
terminal GStreamer `EOS` or `ERROR` after opening. `KlvSink::close()` is a
finalizing operation and can be reached only after the consumer's ingest loop
has already stopped. A live consumer that receives no KLV packets can
therefore wait for telemetry, an external stop, or the close drain before it
notices that its video pipeline failed.[^parrot-to-klv-94]

The downstream report and the upstream API discussion are tracked separately so
the library contract can be reviewed independently of parrot-to-klv's polling
loop.[^libmisbklv-58]

# Decision

**Proposed:** add a nonblocking terminal-status query to the insertion
interface:

- `Inserter::poll()` returns `ok(false)` while no terminal event has been
  observed, `ok(true)` after EOS, and an error when the backend reports a
  terminal failure.
- `KlvSink::poll()` forwards that operation through the high-level facade.
- The base `Inserter` implementation returns `ok(false)`, so synchronous and
  custom backends do not need an asynchronous source merely to implement the
  interface.
- The GStreamer inserter consumes EOS and ERROR messages without blocking and
  latches the result. A later `finish()` preserves an error or already-observed
  EOS instead of losing it during its normal drain.
- Callers serialize `poll()`, `push()`, and `finish()`; the operation is
  nonblocking, not a thread-safety guarantee.

This proposal uses `Result<bool>` to fit the existing result/error vocabulary:
the boolean answers whether clean EOS has occurred, while the result error
answers whether the terminal event was a backend failure. The API remains
**proposed** until [libmisbklv#58](https://github.com/nitsuga/libmisbklv/issues/58)
and its pull request are reviewed.

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

# Consequences

- Live consumers can notice a backend error before the first KLV packet and can
  distinguish clean EOS from an ordinary running session.
- The public insertion interface gains a method. Subclasses inherit a safe
  default, but adding a virtual method is not binary-compatible with an already
  built pre-1.0 implementation.
- GStreamer terminal messages are consumed by `poll()` and retained so a later
  `finish()` cannot turn an observed failure into apparent success.
- Synchronous backends report `ok(false)` forever unless they override the
  operation; they are not required to invent an asynchronous terminal state.
- The first version reports terminal backend failures as `Error::Backend`; it
  does not add a new status enum or expose backend-specific error text.

# Assumptions / open questions

- `poll()`, `push()`, and `finish()` are called from one serialized consumer
  context. A future thread-safe status monitor would be a separate API choice.
- `Error::Backend` is sufficient for the first consumer. If callers need to
  distinguish EOS, ERROR details, and cancellation without consulting logs,
  the result type should be revisited rather than adding ad hoc accessors.
- The pre-1.0 compatibility tradeoff is acceptable; the first stability
  boundary should document the public virtual-interface change.

# Citations

[^parrot-to-klv-94]: [parrot-to-klv#94](https://github.com/nitsuga/parrot-to-klv/issues/94)
    — downstream live-ingest failure and the no-telemetry reproduction.
[^libmisbklv-58]: [libmisbklv#58](https://github.com/nitsuga/libmisbklv/issues/58)
    — upstream API request and review scope.

[1] [ADR 0013](./0013-media-backend-interface.md) — the backend interface
    whose insertion contract is extended.
[2] [ADR 0018](./0018-high-level-api.md) — the `KlvSink` facade this proposal
    keeps backend-neutral.
[3] [ADR 0032](./0032-cancellable-insert-drain.md) — the existing cooperative
    cancellation and bounded drain contract for insertion.
