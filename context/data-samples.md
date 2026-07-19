---
type: Sample Data
title: data/ — KLV MPEG-TS samples
description: KLV MPEG-TS test vectors in data/ — the two canonical .mpg (Day Flight, Night Flight IR) + three larger .ts (Cheyenne, falls, klv_metadata_test_sync). All verified ST 0601; 4678 real packets round-trip byte-exact.
tags: [sample-data, mpegts, st0601, test-vector, h264]
timestamp: 2026-07-18T00:00:00Z
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
these muxers don't emit. **Implication:** when libmisbklv *inserts* a KLV stream
into MPEG-TS, signal `0x06 + KLVA` to match these samples and the dominant
demuxers. When *extracting*, accept both `0x06` and `0x15` (and identify the
KLV PID by the `KLVA` registration descriptor, not by stream_type alone).

# As test vectors

- **Day Flight** — H.264 720p60, 3 min. Good general parse/round-trip target.
- **Night Flight IR** — IR content, longer; relevant when we wire VMTI
  ([0903](/st0903.md)) target-detection/overlay, since IR is a common VMTI
  input.

# Additional `.ts` samples (added 2026-07-18)

Three larger MPEG-TS captures added to [`../data/`](../data/) (LFS). Characterized
by extracting the KLV ES (`ffmpeg -map 0:d:N -c copy -f data`) and walking every
packet with the libmisbklv parser. **All ST 0601** (as expected — no Item 74 /
VMTI in any of them; the 0903 read path stays on hand-authored + jmisb vectors).

| File | KLV ES | Packets | Distinct tags (max) | Notes |
|---|---|---|---|---|
| `Cheyenne.ts` | PID 0x102 | 407 | 37 (≤72) | H.264 + AAC + KLV (3 streams) |
| `falls.ts` (stream 1) | PID 0x1000 | 1953 | 35 (≤65) | basic 0601; has target-location 40/41/42, ground range 57 |
| `falls.ts` (stream 2) | PID 0x1002 | 1953 | (≤91) | **extended** 0601 — items 75–91 (some IMAPB), Security LS |
| `klv_metadata_test_sync.ts` | PID 0x44 | 365 | 36 (≤72) | KLV listed first; name implies frame-sync |

**All four KLV streams parse cleanly and round-trip byte-exact** (407/1953/1953/365
= 4678 packets), through the full parse→codec→builder→checksum path — the
strongest structural + codec validation to date, well beyond the tiny fixtures.

Findings worth acting on:

- **Registry-breadth candidates** (real items we don't yet decode): 3/4/10
  (strings: Mission ID / Platform Tail / Platform Designation), 26–33 (frame
  corner offset lat/lon points), 47/59/72, and `falls` stream-2's **90+ extended
  IMAPB items** (75/78/82–91). `falls` stream 2 is the natural target for the
  extended-item registry work.
- **Tag 48 = Security Local Set (ST 0102)** appears in Cheyenne, `falls`, and
  sync — a genuine *nested-LS* instance (like VMTI-under-74). 0102 is out of v1
  scope, so we carry it raw, but it's a real recursive-nesting sample if wanted.
- **Signaling (resolved 2026-07-19 via `tsdemux` debug):** all three use the
  `KLVA` registration descriptor, but split on `stream_type`: **`falls` = `0x06`**
  (like Day/Night Flight); **`Cheyenne` and `klv_metadata_test_sync` = `0x15`**
  (metadata). This matters for extraction — stock `tsdemux` exposes `0x06`+KLVA as
  `meta/x-klv` but **drops `0x15`** (see [`./backend-scope.md`](./backend-scope.md)).
- **Not yet exercised anywhere:** multi-byte BER-OID tags (≥128, e.g. Item 143
  MSID) — max tag across all samples is 91.

These are ideal large-scale **round-trip regression** targets. Not yet wired as
CTest cases (would need either committed `.klv` fixtures or build-time `ffmpeg`
extraction from the LFS `.ts`).

# Relationships

Exercises [0601](/st0601.md) (UL + items) and [0107](/st0107.md) (BER, TLV);
the `0x06`+`KLVA` signaling ties to [gstklvplugin](/prior-art-gstklvplugin.md)
and the demux idiom to [klvdata](/prior-art-klvdata.md) (which extracts these
via `ffmpeg -map data-re`).

# Citations

[1] [`../data/`](../data/) — `Day Flight.mpg`, `Night Flight IR.mpg`
    (source: `samples.ffmpeg.org/MPEG2/mpegts-klv/`); `Cheyenne.ts`, `falls.ts`,
    `klv_metadata_test_sync.ts` (added 2026-07-18, characterized above).
