---
type: Decision
title: 0x15 KLV extraction via a gst-free TS demuxer
status: accepted
tags: [decision, backend, mpegts, extraction, phase-3]
timestamp: 2026-07-19T03:00:00Z
fork: 12
---

# Context

Fork 12 (backend F-B). Stock gstreamer `tsdemux` exposes `stream_type 0x06`+KLVA
as `meta/x-klv` (handled in B1), but **silently drops `0x15` metadata streams**
(scope finding; Cheyenne, `klv_metadata_test_sync`). A PES probe showed why they
differ: `0x15` carries KLV inside **SMPTE RP 217 metadata AU cells** (PES
`stream_id 0xfc`, a 5-byte cell header before the KLV), whereas `0x06` carries
KLV directly in the PES. Options were (a) a custom demux, (b) defer 0x15, (c) a
`tsdemux` property to unlock it (none found — tsdemux creates no pad at all).

# Decision

**A small gstreamer-free MPEG-TS KLV extractor, `extract_ts_klv`, in the core
library** (`ts.hpp`/`ts.cpp`). It finds the KLV elementary PID **by content** (a
PES payload starting with a SMPTE UL `06 0e 2b 34`), reassembles PES per PID, and
unwraps to KLV — handling **both** signaling types: `0x06` (KLV = PES payload)
and `0x15` (strip the metadata AU cell header). Framed via `packet_frame_length`.
Verified **byte-identical to ffmpeg** on `0x06` (Day Flight) and `0x15` (Cheyenne,
sync), and the output round-trips through the core (407/407 packets).

A bonus: **file/bytes KLV extraction now needs no gstreamer at all** — a
dependency-free core capability. The gstreamer `GstBackend` extraction (B1) stays
for live/network sources.

# Alternatives considered

- **Defer 0x15** — rejected: it appears in real third-party streams and was a
  stated goal.
- **Relabel 0x15→0x06 via a gst PMT rewrite** so `tsdemux` handles it — rejected:
  reintroduces the PMT-surgery we just retired (ADR 0015), for a worse result.
- **Full PSI (PAT/PMT) parsing to locate the KLV PID** — heavier (section
  reassembly, descriptors, CRC); content-detection (first PES starting with a UL)
  is simpler and equally robust for finding the KLV PID.

# Consequences

- **0x15 extraction supported in v1** (file/bytes), byte-exact vs ffmpeg.
- **gst-free file extraction** — consumers can pull KLV from a `.ts` buffer with
  zero dependencies; gstreamer is only needed for live sources + insertion.
- The extractor lives in the **core lib** (it's container parsing, dependency-free)
  — a reasonable placement given the value of no-dependency file extraction.

# Assumptions / open questions

- **Timestamps**: as of [`0021`](./0021-read-path-timestamps.md) this extractor
  also reports each packet's PES PTS as nanoseconds from the start of the source
  — so `0x15` streams, which `tsdemux` exposes no pad for at all, get timing
  here or nowhere.
- **Non-fragmented AU cells** assumed (each PES = one complete AU cell = one KLV
  packet — holds for all samples; `cell_fragmentation_indication = 11`). Cell
  fragmentation across PES is a follow-on if a stream needs it.
- **Content-based PID detection** (first PES starting with a UL). Robust for the
  single-KLV-PID case; PSI parsing can be added if multi-KLV-PID selection is ever
  needed.

# Citations

[1] [`backend-scope`](../backend-scope.md) — the 0x06-vs-0x15 finding
    and PES probe.
[2] [`0013`](./0013-media-backend-interface.md) — `PacketHandler`/`KlvPacket`
    reused by `extract_ts_klv`.
[3] SMPTE RP 217 / ISO 13818-1 — metadata-in-PES AU cells (`stream_id 0xfc`).
