---
type: Decision
title: High-level API — Message + KlvStream/KlvSink facade
status: accepted
tags: [decision, api, usability, facade, phase-3]
timestamp: 2026-07-19T07:00:00Z
fork: 16
---

# Context

The core (parse/codec/builder) and the backend (extract/insert) work, but a user
must hand-wire them: `extract → parse_packet → registry.find → codec::decode`,
and reverse for edits. There's no cohesive read/edit/write surface. The
usability pass adds a **high-level facade** so the common job — open a stream,
read typed values, edit, write — is a few lines. The chosen ambition is a **full
facade** tying backend + core together (not just a thin read wrapper).

The core is deliberately borrow-based (ADR 0011 read-borrows), which is right for
zero-copy performance but hostile to a casual user who wants to hold and edit a
packet. So the facade is the **owns-for-convenience** layer above the borrow core.

# Decision

Two layers, split along the optional-gstreamer boundary (ADR 0014):

**`Message` — core lib (dependency-free).** An owned, editable view of one KLV
packet.
- `Message::parse(bytes) -> Result<Message>` copies the bytes and parses; the
  registry is auto-selected from the 16-byte UL key (`registry_by_key`).
- Typed read: `get<T>(tag) -> optional<T>` (decode via descriptor+codec, pull the
  matching `Value` alternative); iterate decoded items.
- Edit: `set(tag, Value)` stages an edit (encoded at set-time at the item's
  source width, or the descriptor width for a newly-added tag).
- `encode() -> Bytes` rebuilds via `LocalSetBuilder`: **untouched items pass
  through raw** (byte-exact), edited/added items via the typed codec, checksum
  re-emitted by `finalize`. So a parse→encode with no edits is byte-identical.
- **Owns its bytes** — decouples lifetime from any stream buffer, making
  edit/emit robust. (The core stays borrow-based; the facade owns. Altitude
  split.)

**`KlvStream` (read) + `KlvSink` (write) — misbklv-gst.** Built on `MediaBackend`
(ADR 0013), so they need gstreamer for live sources — hence they live in the gst
target, not the core.
- `KlvStream(source)` iterates owned `Message`s with a **range-for** (`for (auto&
  m : stream)`). The backend's `extract` is a blocking push-callback; the stream
  adapts it to pull via a **background thread + a bounded blocking queue** (the
  callback copies each packet in; the iterator pops). The bound gives
  backpressure — for a file it caps memory; for a live source it composes with
  the B4 `is-live` pacing. End of iteration follows queue drain; callers then
  check the terminal status.
- `KlvSink(sink)` wraps an `Inserter`; `emit(Message&)` pushes `m.encode()`;
  `close()` drains. Read and write are **separate endpoints** (a source you read
  and a sink you write are different things) — the canonical loop is
  `for (auto& m : in) { m.set(...); out.emit(m); }`.

Tags are plain `std::uint16_t` with a small set of named constants for the common
0601 items (enough for the example/docs); full generated tag enums are a possible
follow-on (the alternative not taken this pass).

> **Since accepted:** that follow-on landed — `gen_registry.py` emits a per-registry
> `enum class` (`misbklv::tags::Uas0601::SensorLatitude`), and `get`/`set` accept
> either the enum or the raw number. This ADR's decision is unchanged; only the
> "possible follow-on" above is now history.

# Alternatives considered

- **Thin read-only wrapper** — smaller, but doesn't deliver the edit/stream story
  the pass is for; rejected in favor of the full facade (user choice).
- **Borrowing `Message`** (spans into a caller buffer) — matches the core, but
  makes edit-and-emit lifetime-fragile; owning is worth the copy (~1 KB/packet).
- **Eager whole-file extraction into a vector** (no thread) — trivial, but buffers
  the entire source and breaks live/real-time (regresses B4). The bounded-queue
  pull preserves streaming for file and live alike.
- **Generated tag enums per registry** (extend `gen_registry.py`) — nice
  ergonomics, but more surface to lock in now; deferred (named constants suffice).
- **A single bidirectional `KlvStream` with `emit`** (the option sketch) —
  conflates source and sink; separate `KlvStream`/`KlvSink` is cleaner. *(Diverges
  from the illustrative preview.)*

# Consequences

- The common read/edit/write job is a few lines; `docs/` gets a real walkthrough
  + a compiled example (which tends to surface API gaps — like the spike did).
- `Message` is usable in the gst-free core (file bytes via `extract_ts_klv` →
  `Message::parse`); only the live streaming facade needs gstreamer.
- Owning copies cost ~1 KB/packet — negligible vs the media pipeline; the
  zero-copy path stays available at the core level for performance-critical use.
- A new concurrency surface (the stream's producer thread + queue) — kept small
  and behind the facade; the backend already owns threads (B4).

# Implementation refinement (2026-07-31)

`has()` reflects both source items and staged additions. `encode()` uses private
source membership to distinguish those cases: an unedited parsed `Message`
returns exactly its original packet extent, preserving noncanonical BER and
checksum placement while excluding trailing input bytes. `Message::create`
continues to build packets from staged items. `KlvStream` now accepts
`ExtractOptions` and retains range-for iteration with a post-loop `error()`
check; `KlvSink` preserves an opening failure for the caller. See
[`0027`](./0027-high-level-streaming-errors.md).

# Assumptions / open questions

- **`get<T>` type must match the descriptor's `ValueKind`** (e.g. `get<double>`
  for a mapped item); a mismatch returns `nullopt`. A checked/typed-tag scheme is
  the enum-codegen follow-on.
- **PTS on emit**: `Message` carries the extraction PTS (or `kNoPts`); the sink
  paces per ADR 0017 when realtime. *(Follow-on: until
  [`0021`](./0021-read-path-timestamps.md) the extraction PTS was **always**
  `kNoPts`, so `KlvStream` → `KlvSink` silently re-timed the stream it read —
  and, once ADR 0020 required a real PTS for a video branch, refused it. With
  0021 the two halves compose on one timeline.)*
- Whether `Message` should expose the nested 0903 sets as sub-`Message`s is
  deferred — v1 exposes nested items as raw; typed nested access is a follow-on.

# Citations

[1] [`0011`](./0011-encode-model.md) — the read-borrows/write-owns boundary this
    builds the owning facade above.
[2] [`0013`](./0013-media-backend-interface.md) — `MediaBackend`/`Inserter` the
    stream/sink wrap; [`0014`](./0014-backend-optional-dependency.md) — why the
    stream layer lives in misbklv-gst.
[3] [`0017`](./0017-realtime-streaming.md) — live pacing the bounded queue
    composes with.
