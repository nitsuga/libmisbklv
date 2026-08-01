---
type: Decision
title: Bounded live KLV frame reassembly
decision_status: accepted
tags: [decision, streaming, hardening, gstreamer, phase-3]
generated:
  by: claude/opus-5
  at: 2026-07-31T00:00:00Z
sources:
  - resource: ../../references/ST0903.6.pdf
    title: MISB ST 0903.6 §9.2 — VMTI LS item/count limits
  - resource: ../../references/ST0601.19.pdf
    title: MISB ST 0601.19 §8 — "Not Limited" maximum lengths
fork: 24
---

# Context

GStreamer extraction receives arbitrary fragments and must retain bytes until a
complete KLV frame is available, whether its source is a file or a live socket.
A declared BER length can otherwise make the incremental receiver retain
unbounded data. The MISB standards do not supply a universal KLV item maximum:
ST 0903 permits an unlimited number of VMTI LS items with no item size limit,
while ST 0601 marks some maximum lengths as "Not Limited" and notes that network
guards may use such bounds. The operational limit is therefore a library policy,
not a standards conformance limit.

# Decision

The incremental GStreamer reassembler has a configurable complete-frame cap,
`ExtractOptions::max_packet_bytes`, with a default of **16 MiB**. It is measured
across the entire frame: the 16-byte SMPTE UL, BER length, and declared value.

- Before a valid SMPTE UL, non-UL bytes are garbage and are discarded while the
  reassembler searches for the next UL, preserving a partial prefix across
  fragments.
- After a valid UL, an incomplete header or declared value is **incomplete**
  while input may still arrive. Natural termination with an actually incomplete
  frame is `Truncated`.
- A malformed BER length or frame declaration after a valid UL is
  **malformed** and fatal. A declared complete frame exceeding the cap is
  `ResourceLimit` and fatal. Neither condition resynchronizes inside the
  claimed payload, because doing so could reinterpret payload bytes as a new
  packet.
- Cancellation succeeds without treating retained incomplete input as an
  extraction error.
- The gst-free whole-buffer parser remains outside this default: offline parsing
  is bounded by its supplied input rather than this operational cap.

# Alternatives considered

- **No bound** — preserves every standards-permitted declaration but lets a
  peer hold live reassembly memory indefinitely. Rejected.
- **A fixed, non-configurable bound** — simple, but cannot fit deployments with
  different memory and frame-size budgets. Rejected.
- **Silently resynchronize after a bad or oversized declaration** — might find a
  later UL sooner, but can manufacture packets from bytes inside the claimed
  payload and hide a framing failure. Rejected.
- **A time-only limit** — fails to bound memory when fragments keep arriving,
  and confuses network timing with a declared frame's size. Rejected.

# Consequences

- Incremental extraction refuses any declared pending KLV frame above a ceiling
  configurable for deployment needs.
- Callers can distinguish incomplete input, malformed framing, policy refusal,
  and cooperative cancellation. The high-level surfacing of streaming errors is
  follow-on work, not redefined here.
- A valid frame is never abandoned merely because unrelated garbage preceded
  its UL, while a valid UL commits the parser to its declared payload boundary.

# Assumptions / open questions

- The default is an operational starting point, not a claim that ordinary KLV
  frames should approach 16 MiB; deployments with a different budget configure
  the cap.
- Error presentation through the high-level streaming facade remains to be
  settled separately from these framing semantics.

# Citations

[1] [`ST 0903.6`](../../references/ST0903.6.pdf) §9.2 — VMTI LS item count and
    item size are unlimited.
[2] [`ST 0601.19`](../../references/ST0601.19.pdf) §8 — "Not Limited" maximum
    lengths and network-guard guidance in the item-table conventions.
[3] [`ST 0107.5`](../st0107.md) §6 — KLV BER mechanics.
[4] [`0013`](./0013-media-backend-interface.md) — incremental backend framing;
    [`0019`](./0019-extract-cancellation.md) — cooperative cancellation.
