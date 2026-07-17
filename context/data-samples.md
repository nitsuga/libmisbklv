---
type: Sample Data
title: data/ — KLV MPEG-TS samples
description: The two canonical ffmpeg KLV samples (Day Flight, Night Flight IR); verified ST 0601 in MPEG-TS with 0x06+KLVA signaling.
tags: [sample-data, mpegts, st0601, test-vector, h264]
timestamp: 2026-07-17T14:00:00Z
resource: ../data
---

# What

Two MPEG-TS files in [`../data/`](../data/), the canonical KLV samples from
`samples.ffmpeg.org/MPEG2/mpegts-klv/` — the same files
[klvdata](/prior-art-klvdata.md)'s README uses. Ideal round-trip test vectors
for the read/write and demux paths.

# Verified structure (both)

Probed with `ffprobe` + a direct TS/PMT/UL scan.

| | Day Flight.mpg | Night Flight IR.mpg |
|---|---|---|
| Duration | 3:14.88 | 6:10.82 |
| Bitrate | 4187 kb/s | 3670 kb/s |
| Video | H.264 (Main), yuv420p, 1280×720, 60 fps | H.264 (Main), yuv420p, 1280×720 (fps unreported) |
| Program | single program 1 | single program 1 |
| PMT PID | 0x1E0 (480) | 0x1E0 (480) |
| Video PID | 0x1E1 (481) | 0x1E1 (481) |
| KLV PID | 0x1F1 (497) | 0x1F1 (497) |
| PCR PID | 0x1E1 (rides on video) | 0x1E1 (rides on video) |
| KLV stream_type | **0x06** (private data) | **0x06** |
| KLV reg. descriptor | **KLVA** | **KLVA** |
| Aux stream | stream_type 0xFE, PID 0x96E | stream_type 0xFE, PID 0x96E |

The auxiliary `0xFE` stream (PID 0x96E) is IPMP/ancillary PES private data,
not KLV — a robust demuxer should ignore it.

# KLV payload

The KLV elementary stream is genuine ST 0601:

- **UL key** `060e2b34020b01010e01030101000000` — matches the UAS Datalink LS
  key in [0601](/st0601.md) §6.2.
- **BER long-form length** (value 144–145 B → `0x81 0x90` / `0x81 0x91`),
  i.e. [0107](/st0107.md) long form in use.
- **First item tag `0x02`** = Precision Time Stamp, length 8 — the mandatory
  "timestamp first" rule of [0601](/st0601.md) §6.3, honored.

So each KLV PES is a well-formed 0601 packet — a clean parse target.

# Signaling note (load-bearing for the mux path)

The KLV PID uses **`stream_type = 0x06`** (PES private data) + a **`KLVA`
registration descriptor`** — *not* `0x15` (the ST 1402 metadata stream type).
This is exactly the pragmatic signaling
[gstklvplugin](/prior-art-gstklvplugin.md) chose so GStreamer `tsdemux` (and
`ffmpeg`) hand up raw KLV; `0x15` would expect metadata access-unit wrapping
these muxers don't emit. **Implication:** when libklv *inserts* a KLV stream
into MPEG-TS, signal `0x06 + KLVA` to match these samples and the dominant
demuxers. When *extracting*, accept both `0x06` and `0x15` (and identify the
KLV PID by the `KLVA` registration descriptor, not by stream_type alone).

# As test vectors

- **Day Flight** — H.264 720p60, 3 min. Good general parse/round-trip target.
- **Night Flight IR** — IR content, longer; relevant when we wire VMTI
  ([0903](/st0903.md)) target-detection/overlay, since IR is a common VMTI
  input.

# Relationships

Exercises [0601](/st0601.md) (UL + items) and [0107](/st0107.md) (BER, TLV);
the `0x06`+`KLVA` signaling ties to [gstklvplugin](/prior-art-gstklvplugin.md)
and the demux idiom to [klvdata](/prior-art-klvdata.md) (which extracts these
via `ffmpeg -map data-re`).

# Citations

[1] [`../data/`](../data/) — `Day Flight.mpg`, `Night Flight IR.mpg`
    (source: `samples.ffmpeg.org/MPEG2/mpegts-klv/`).
