---
type: Decision
title: Cooperative extraction cancellation (stop token)
decision_status: accepted
tags: [decision, backend, api, streaming, phase-3]
generated:
  by: claude/opus-5
  at: 2026-07-19T08:00:00Z
fork: 17
---

# Context

The follow-on [`0017`](./0017-realtime-streaming.md) and
[`0018`](./0018-high-level-api.md) both deferred: `MediaBackend::extract` is
blocking and ends only at EOS (file) or the idle timeout (live). There was **no
way to end it early**. For `KlvStream`, breaking out of `for (Message& m : in)`
runs the destructor, which could only set the queue's `stop_` flag (making the
producer *discard*) — but the producer is inside `extract`, which for a live
source keeps running. For an **endless** live source that never idles, `extract`
never returns and the destructor's `join()` **hangs forever**. Early exit from a
live stream is a basic expectation, so it needs a real cancellation path.

# Decision

**Add a C++20 `std::stop_token` to `extract`; the backend polls it and tears the
pipeline down when stop is requested.**

- Interface (amends [`0013`](./0013-media-backend-interface.md)):
  `extract(source, on_packet, std::stop_token stop = {})`. A default-constructed
  token is never signaled — existing callers are unchanged and run to the natural
  end. Cancellation is **cooperative and asynchronous**: the token is signaled
  from another thread (the token is the right tool precisely because `extract`
  blocks the calling thread).
- `GstBackend`: the bus loop, previously an infinite `gst_bus_timed_pop_filtered`,
  now uses a **finite 100 ms poll** and checks `stop.stop_requested()` each pass;
  on request it breaks and `set_state(NULL)` (a cancelled extract is success, not
  an error). ≤100 ms stop latency — ample for "early exit".
- `MockBackend`: checks the token between replayed packets (so cancellation is
  testable without gstreamer).
- `KlvStream` owns a `std::stop_source`, passes its token to `extract`, and the
  destructor **both** (a) sets the queue `stop_` to unblock a `push_frame` waiting
  on a full queue — otherwise `set_state(NULL)`, which waits on the streaming
  thread, would deadlock — **and** (b) `request_stop()` to end the extract itself,
  then joins. Order matters: unblock the queue, then cancel, then join.

Verified: an endless mock source, read 3 messages then `break` → the destructor
returns in **~6 ms** (`stop_test`). Teeth: neutralizing `request_stop()` makes the
same test **hang** (the queue flag alone can't end an endless extract).

# Alternatives considered

- **Handler returns `bool`** (false = stop) — the option [`0017`](./0017-realtime-streaming.md)
  floated. Only stops *when a packet arrives and the handler runs*; it can't end a
  stalled-but-not-idle source, and threads the stop signal through every handler.
  A token cancels asynchronously regardless of packet flow — the better fit for
  "consumer on another thread wants out". (The two compose; the bool-handler can
  still be added for content-driven stops if needed.)
- **A raw `std::atomic<bool>*` / a custom `StopToken`** — works, but `std::stop_token`
  is the C++20-idiomatic vocabulary type (pairs with `std::stop_source`, and with
  `std::jthread` if we ever want auto-cancel), for free.
- **Post a custom message / EOS on the gst bus to wake the pop instantly** —
  removes the ≤100 ms poll latency but adds gst-specific plumbing; the poll is
  simpler and the latency is irrelevant for early exit. Revisit only if a use
  case needs sub-100 ms cancel.
- **Interrupt via pipeline teardown from outside** — `KlvStream` only holds the
  abstract `MediaBackend`; it can't touch the gst pipeline, so cancellation must
  travel through the interface. Confirms the `extract` signature is the seam.

# Consequences

- `KlvStream` early-`break` is now safe on any source — the destructor cancels and
  returns promptly (no wait for EOS / idle). The [`0018`](./0018-high-level-api.md)
  caveat is retired.
- `MediaBackend` implementers must thread `stop` through `extract` (both current
  ones do); the default token keeps all existing call sites working.
- Stop latency is ≤100 ms (the poll interval) — a non-issue for teardown.
- `extract_ts_klv` (the gst-free file/bytes extractor) is unchanged — it's bounded
  and fast; cancellation is a live-source concern.

# Assumptions / open questions

- **Cooperative, not preemptive**: a handler that blocks for a long time delays
  cancellation until it returns; handlers are expected to be short (they push to a
  queue).
- An **explicit `KlvStream::request_stop()`** (stop without destroying) is a
  trivial add if a caller wants to stop from a third thread; not exposed yet.

# Citations

[1] [`0013`](./0013-media-backend-interface.md) — the `extract` contract amended.
[2] [`0017`](./0017-realtime-streaming.md) — live idle-timeout end; deferred the
    stop token here. [`0018`](./0018-high-level-api.md) — `KlvStream` whose early
    break this fixes.
[3] C++20 `std::stop_token` / `std::stop_source` (`<stop_token>`).
