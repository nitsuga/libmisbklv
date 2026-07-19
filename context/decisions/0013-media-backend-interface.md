---
type: Decision
title: MediaBackend interface
status: proposed
tags: [decision, backend, interface, gstreamer, phase-3]
timestamp: 2026-07-19T00:00:00Z
fork: 11
---

# Context

Fork 11 (backend F-A). ADR [`0008`](./0008-media-backend-gstreamer.md) chose a
gstreamer backend, library-style, behind a kept **`MediaBackend`** interface (so
a mock and a future ffmpeg backend fit, and the KLV core stays media-agnostic).
This ADR fixes that interface: how the core gets KLV **out of** and **into** an
MPEG-TS stream, backend-agnostically.

The B0 extraction spike ([`backend-scope`](../../planning/backend-scope.md))
grounded the hard parts:

- `tsdemux ! meta/x-klv ! appsink` extraction is **byte-identical** to the
  ffmpeg `.klv` the core already round-trips — the core consumes backend output
  unchanged.
- appsink yields **sub-packet fragments** (203 buffers for a 6-packet stream), so
  the backend must **reassemble** a byte stream and **frame** whole KLV packets;
  it cannot treat one buffer as one packet.
- **PES PTS is unreliable** (`CLOCK_TIME_NONE`); time correlation should use the
  KLV Item 2 Precision Time Stamp ([`0009`](./0009-st0604-deferred.md)).

# Decision

An abstract **`MediaBackend`** with two operations — **extract** (pull KLV from a
source) and **open_insert** (push KLV to a sink) — plus a small `Inserter`
session for the push side. The interface is at the **byte level**: it moves whole
KLV *packets*, never decodes them (that is the core's job). Errors via
`Result<T>` ([`0007`](./0007-error-and-c-abi.md)).

```cpp
inline constexpr std::int64_t kNoPts = -1;

// One complete, framed KLV packet. `bytes` borrow the backend's reassembly
// buffer and are valid ONLY during the handler call — copy to retain (ADR 0011).
struct KlvPacket {
  std::span<const std::byte> bytes;   // parse_packet-able
  std::int64_t pts_ns = kNoPts;       // PES PTS if present, else kNoPts
};
using PacketHandler = std::function<void(const KlvPacket&)>;

struct InsertConfig {
  std::string sink;   // "file:out.ts" | "udp:host:port" | "srt:uri"
  // v1 signals 0x06 async (0x15 sync deferred, ADR 0008).
};

class Inserter {                        // real-time push, flow-controlled
 public:
  virtual Result<std::monostate> push(std::span<const std::byte> klv_packet,
                                      std::int64_t pts_ns) = 0;  // blocks on backpressure
  virtual Result<std::monostate> finish() = 0;                  // EOS + flush + close
  virtual ~Inserter() = default;
};

class MediaBackend {
 public:
  // Drive a demux pipeline; call `on_packet` for each complete KLV packet until
  // EOS (file) or stop. Blocking; the handler runs on the backend's thread.
  virtual Result<std::monostate> extract(std::string_view source,
                                         const PacketHandler& on_packet) = 0;
  virtual Result<std::unique_ptr<Inserter>> open_insert(const InsertConfig&) = 0;
  virtual ~MediaBackend() = default;
};
```

- **Extraction = push (callback), blocking.** `extract()` runs the pipeline and
  invokes `on_packet` per framed packet, blocking until EOS (file) or stop. The
  backend owns the reassembly buffer and frames packets using the core's BER
  length ([`packet`](../st0107.md)); the consumer runs `parse_packet`.
- **Insertion = a push session with backpressure.** `open_insert()` builds the
  mux pipeline; `push()` hands one encoded KLV packet (e.g. from
  `LocalSetBuilder::finalize()`, [`0011`](./0011-encode-model.md)) to `appsrc`
  and **blocks when appsrc is full** — the real-time ergonomic win (ADR 0008).
- **Ownership boundary** (ADR [`0011`](./0011-encode-model.md)): extraction
  **borrows** (spans into the backend's buffer, valid during the callback);
  insertion **owns** (the caller owns the bytes it pushes).
- **Two implementations:** `GstBackend` (v1) and `MockBackend` (in-memory, for
  testing the contract without gstreamer). The interface header is
  dependency-free; only `GstBackend` links gstreamer (fork 14 / F-D).

# Alternatives considered

- **Pull iterator (`next()`) for extraction** — the consumer owns the thread,
  but it fights gstreamer's push model (needs an internal bounded queue + thread
  hand-off). Rejected for v1; a pull adapter can wrap the callback later.
- **Yield parsed `Packet` objects** — convenient, but couples the backend to the
  core data model and its span lifetimes, and duplicates work when the consumer
  wants raw bytes. Rejected: keep the backend semantics-free (bytes only); the
  consumer calls `parse_packet`.
- **Yield raw appsink buffers (unframed)** — pushes reassembly/framing into every
  consumer, re-litigating the B0 fragmentation. Rejected: the backend frames.
- **Insertion as a `need-data` callback** — leaks gstreamer's flow-control model
  into the public API. Rejected: a blocking `push()` is the clean equivalent.
- **No interface (concrete `GstBackend` only)** — rejected per ADR 0008
  (mock/testability + future ffmpeg).

# Consequences

- `GstBackend` **links the KLV core** (it uses `ber`/`packet` to frame packets
  from the reassembly buffer). The interface header itself stays core-only.
- `MockBackend` makes the extract/insert contract unit-testable with no
  gstreamer — feed canned KLV packets, assert handler calls; capture pushes.
- **Threading:** `extract()` blocks and calls the handler on the backend thread;
  a consumer wanting concurrency runs `extract()` on its own thread. Documented,
  not hidden.
- **Lifetime:** borrowed `KlvPacket::bytes` are valid only during the handler;
  retaining requires a copy. This is the ADR 0011 read-borrows boundary made
  concrete at the backend edge.
- **Round-trip test shape:** extract from a `.ts` → `parse_packet` each unit →
  re-`finalize()` → `push()` into `open_insert` → re-extract → byte-exact, in
  both `GstBackend` and `MockBackend`.

# Assumptions / open questions

- **Live-extraction stop mechanism** (stop token vs `on_packet` returning
  `bool`) — settle in B1; file extraction ends at EOS regardless.
- **`InsertConfig` / source grammar** (URI shapes, signaling options) — settle in
  B1/B2; strings for now, may become structured.
- **Framing dependency**: the backend reuses the core's BER length to find packet
  boundaries; confirm no packet spans across a way that defeats incremental
  framing (parse_packet needs the whole packet — buffer until length satisfied).
- **0x15 extraction** (fork 12) rides this same interface (source-agnostic); only
  `GstBackend`'s internal demux path differs.
- A future **C ABI** ([`0007`](./0007-error-and-c-abi.md)) wraps this interface
  (handles + callbacks) — keep signatures wrapper-friendly.

# Citations

[1] [`0008`](./0008-media-backend-gstreamer.md) — gstreamer library-style;
    `MediaBackend` kept; real-time insertion via `appsrc`.
[2] [`0011`](./0011-encode-model.md) — read-borrows / write-owns ownership.
[3] [`0007`](./0007-error-and-c-abi.md) — `Result<T>`; future C ABI.
[4] [`0009`](./0009-st0604-deferred.md) — KLV Item 2 for time correlation.
[5] [`backend-scope`](../../planning/backend-scope.md) — B0 spike (reassembly,
    fragments, PTS) that grounds this interface.
