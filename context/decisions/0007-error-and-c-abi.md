---
type: Decision
title: Error handling & C ABI
decision_status: accepted
tags: [decision, architecture, error, abi, phase-2]
generated:
  by: claude/opus-5
  at: 2026-07-17T19:30:00Z
fork: 4
---

# Context

Fork 4 (split). Two API-surface decisions: error returns and the C ABI. Error
handling is largely settled by
[`0001`](./0001-build-system-and-cpp-standard.md) (C++20, local `Result<T>`,
`std::expected` deferred); this ADR refines it and decides the C ABI.

# Decision (error)

**Local `Result<T, E>`** (C++20; `std::expected` deferred per
[`0001`](./0001-build-system-and-cpp-standard.md)). Parser/decode APIs return
`Result<T>` — a success value or an error enum. No exceptions across the public
API for routine failures (truncated buffer, unknown tag, out-of-range) — those
are `Result` errors. Exceptions reserved for unrecoverable conditions (e.g.
OOM). Error type: a small enum (`ParseError::Truncated`, `::UnknownTag`,
`::OutOfRange`, …) + optional context.

# Decision (C ABI)

**Defer.** The core is designed in clean C++ first; no C ABI in v1. Expose a
thin C-ABI wrapper later when the gstreamer backend (fork 5) or Python/ctypes
needs it. Keep public types POD-ish / opaque-handle-friendly so a future
wrapper is *possible*, but don't constrain v1 to it.

# Alternatives considered

## Error

- **Exceptions for all errors** — rejected: parse errors are routine, not
  exceptional; unwind cost in a parser hot path; cannot cross a future C ABI.
- **`std::expected` (C++23)** — deferred per
  [`0001`](./0001-build-system-and-cpp-standard.md) (needs libstdc++ 12; JetPack
  6 is 11). Local `Result` bridges.
- **`tl::expected` (third-party)** — avoids a hand-rolled Result but adds a
  dependency. Lean: local Result for now (no dep); revisit if maintenance bites.

## C ABI

- **C ABI in v1** — rejected (premature; constrains the core; YAGNI until fork
  5).
- **Never expose a C ABI** — rejected (we'll likely need it for gstreamer C
  plugins / ctypes per [`0003`](./0003-project-name.md)).

# Consequences

- `Result<T>` throughout the public API; the error enum becomes the basis of
  the (future) C-ABI error codes.
- Core stays C++-idiomatic; the future C wrapper translates `Result` → error
  codes + opaque handles.

# Assumptions / open questions

- Exact `Result` API (map / and_then / or_else, match-on-error) — settle in
  implementation.
- Error-enum granularity — refine as we hit cases.

# Citations

[1] [`0001`](./0001-build-system-and-cpp-standard.md) — C++20, `Result`,
    `std::expected` deferred.
[2] [`0003`](./0003-project-name.md) — C ABI likely (deferred).
[3] GStreamer C plugin ecosystem.
