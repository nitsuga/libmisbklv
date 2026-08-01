---
type: Decision
title: High-level streaming errors
decision_status: accepted
tags: [decision, api, streaming, error, gstreamer, phase-3]
generated:
  by: claude/opus-5
  at: 2026-07-31T00:00:00Z
fork: 25
---

# Context

`KlvStream` turns the backend's blocking extraction callback into an ergonomic
range of owned `Message`s. That leaves an asynchronous failure channel: a backend
or `Message::parse` failure can occur after construction and while valid frames
are already queued. The facade must expose that failure without making normal
iteration noisy or silently losing the reason it stopped.

`KlvSink` likewise opens its backend session in a constructor, which cannot
return `Result`. An `open_insert` failure must survive construction and remain
observable with its original error rather than becoming a generic backend error.

# Decision

- Keep `KlvStream` as a range of `Message`s. After iteration ends, callers check
  its `optional<Error>` terminal status through `KlvStream::error()`; an empty
  status is success, and routine failures do not throw. `KlvStream` accepts
  `ExtractOptions` for its extraction.
- Valid frames queued before a backend failure continue to drain. Only after the
  queue is empty does iteration end and `error()` report that failure.
- A `Message::parse` failure is terminal at its position. The stream cancels the
  backend and drops later queued or arriving data rather than silently skipping
  the bad packet and continuing.
- Normal file EOS and cooperative cancellation are successful terminal states.
- `KlvSink` preserves and exposes the exact `open_insert` error. Its `emit()` and
  `close()` return that same error when opening failed; callers check the sink's
  opening status before emitting and check every `emit()` and `close()` result.
- Existing `Error` values are sufficient: parse errors, `Backend`, `Unsupported`,
  and `ResourceLimit` cover the outcomes. No enum expansion or numeric-code
  change is made.

# Alternatives considered

- **`iterator<Result<Message>>`** — puts an error wrapper in every range body,
  infecting the read/edit/write idiom and existing references for an asynchronous
  terminal condition. Rejected.
- **Exceptions** — conflict with the project's `Result` error model for routine
  failures. Rejected.
- **Factory-only `KlvStream`/`KlvSink` construction** — can report an immediate
  open failure, but cannot solve a source failure that occurs later on the
  extraction thread. It may be added as convenience later; it is not required
  for this contract. Rejected.
- **Silent logging or status loss** — lets a loop look successful after a backend
  or parse failure. Rejected.
- **A checked extraction callback** — useful at the lower backend boundary, but
  does not by itself preserve range-for ergonomics or provide the facade's
  terminal status. Rejected for this surface.

# Consequences

- The common range-for remains concise, with one required post-loop terminal
  check when source completeness matters.
- Consumers receive valid work already accepted before an asynchronous backend
  failure, while a malformed `Message` is never hidden by skipping it.
- Sink construction remains ergonomic but cannot make an open failure disappear;
  the original `Result` is available consistently at every write operation.

# Assumptions / open questions

- A future factory can supplement, not replace, constructors for callers that
  prefer immediate open-time branching.
- The `Result` API may gain combinators later; the terminal-check contract does
  not depend on them.

# Citations

[1] [`0007`](./0007-error-and-c-abi.md) — `Result` and no routine exceptions.
[2] [`0013`](./0013-media-backend-interface.md) — extraction callback and
    insertion session boundary.
[3] [`0018`](./0018-high-level-api.md) — range-for façade.
[4] [`0019`](./0019-extract-cancellation.md) — cooperative cancellation.
