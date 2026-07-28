---
type: Decision
title: ST 0604 SEI generation for video passthrough
status: accepted
tags: [decision, 0604, sei, video-passthrough, h264, generation, phase-3]
timestamp: 2026-07-27T20:50:00Z
fork: 21
---

# Context

Fork 21. Video passthrough ([`0020`](./0020-video-passthrough.md)) was
implemented with the assumption that ST 0604 was deferred
([`0009`](./0009-st0604-deferred.md)) — no SEI generation, no 0604 timestamp
encode/decode, just codec-agnostic video pass-through.

**Real-world finding (2026-07-27):** A consumer (`parrot-to-klv`) converts
Parrot drone MP4s to TS with KLV via libmisbklv's video passthrough. The
consumer's downstream client actively extracts **both** ST 0601 timestamps
(from KLV metadata) and ST 0604 timestamps (from H.264 SEI via
the downstream consumer's SEI decoder). The client needs frame-accurate
timestamps directly in the video elementary stream, independent of the metadata
stream.

**Critical discovery:** Parrot MP4s don't embed ST 0604 SEI — they store
timestamps in a separate MP4 metadata track (`mett` codec,
`ParrotVideoMetadata` handler). parrot-to-klv converts this to ST 0601 KLV
(works fine), but **does not generate ST 0604 SEI** in the H.264 stream. The
downstream client can't extract ST 0604 because it was never generated.

**The fork:** This is a *generation* task, not preservation. Sources don't have
0604; consumers need it generated from the available timestamp data. This is
the generation side of the ST 0604 deferral in [`0009`](./0009-st0604-deferred.md).

# Decision

**Generate ST 0604 Precision Time Stamps as H.264 SEI User Unregistered
messages for all video passthrough operations using absolute Unix timestamps
from KLV sensorTimestamp.**

Implementation approach:
1. **Parse KLV in `push()`**: Extract ST 0601 `sensorTimestamp` (tag 2, absolute Unix µs)
2. **PTS → timestamp map**: Thread-safe `std::map<uint64_t, uint64_t>` in `VideoCtx`
   - Populated in push(), queried in video pad probe
   - No pruning — all entries persist for session lifetime (~16 bytes/entry)
3. **Fuzzy PTS matching**: Backward-only lookup with 200ms tolerance
   - Video pipeline can run ahead of KLV push() by a few frames
   - `upper_bound()` + step back ensures we never match future frames
   - Falls back to relative PTS if no match within tolerance
   - Observed lag typically submillisecond; 200ms = 6 frames @ 30fps headroom
4. **SEI generation** per ST 0604.6 §7:
   - UUID `MISPmicrosectime` (16 bytes)
   - Status byte (GPS locked, normal time)  
   - Modified Precision Time Stamp (absolute Unix µs + 0xFF emulation prevention)
5. **Picture Timing SEI stripping**: Remove type 1 SEI from source to prevent parser warnings
6. **Injection**: Insert before first slice NAL (types 1-5, 19-21)
7. Always enabled for video passthrough (no configuration needed)

Based on the downstream consumer's SEI encoder and
the downstream consumer's SEI decoder implementations.

# Alternatives considered

- **Generate only on request** — rejected; ST 0604 is useful for all consumers
  with video, and generation cost is minimal (~35 bytes/frame)
- **Preserve from source instead** — rejected; Parrot sources don't have SEI to
  preserve, they need generation from metadata
- **Put timestamps only in KLV** — rejected; downstream needs both KLV (metadata
  stream) and SEI (video stream) for different purposes
- **Wait for full ST 0604 implementation** — rejected; generation is simpler
  than full 0604 (no extraction, no validation), and the need is immediate

# Consequences

- **Video ES is larger** — ~35 bytes of SEI per frame (~14KB for 418 frames)
- **Test updated** — `gst_video_insert_test` now checks ES size >= source
  instead of byte-exact
- **Always enabled** — all video passthrough operations generate ST 0604,
  whether parrot-to-klv or other consumers
- **Downstream compatible** — existing the downstream consumer's SEI decoder
  extracts generated SEI with no changes needed
- **Timestamps from KLV sensorTimestamp** — absolute Unix microseconds from
  ST 0601 tag 2, matched to video frames by PTS via fuzzy lookup
- **H.264 only** — H.265 uses different UUID (Nano, ST 0604.6 §8), deferred

# Implementation Notes

**Files modified:**
- `src/gst/gst_backend.cpp` (~240 lines added):
  - `kPtsMatchToleranceNs` — 200ms tolerance constant for fuzzy matching
  - `VideoCtx::pts_to_sensor_timestamp` — thread-safe map (PTS ns → sensorTimestamp µs), no pruning
  - `GstInserter::push()` — parses KLV tag 2, populates map (all entries persist for session)
  - `generate_0604_sei_payload(uint64_t timestamp_microsec)` — creates SEI payload
  - `on_h264_buffer_inject_sei()` — pad probe with fuzzy PTS lookup, Picture Timing SEI stripping
  - `VideoCtx::generate_sei` flag — always true when video_source set
- `CMakeLists.txt` — added `gstreamer-codecparsers-1.0` dependency
- `test/gst_video_insert_test.cpp` — updated byte-exact check

**How it works:**
1. `open_insert()` sets `video->generate_sei = true` when video_source present
2. **KLV parsing in push()**: extracts tag 2 (sensorTimestamp), stores `map[pts_ns] = ts_µs`
3. `on_video_pad_added()` attaches pad probe to h264parse output if `generate_sei`
4. **Video probe fires**: performs backward-only fuzzy PTS lookup with 200ms tolerance
   - `upper_bound(pts_ns)` then step back to find closest entry ≤ pts_ns
   - Falls back to relative PTS if no match found
5. **Picture Timing SEI stripping**: scans for type 1 SEI, marks ranges for removal
6. `generate_0604_sei_payload()` encodes absolute Unix µs per ST 0604.6 §7
7. Probe finds first slice NAL (scan for 0x000001 start code, check type)
8. Creates new buffer: [original - Picture Timing ranges] + [ST 0604 SEI] injected before slice
9. SEI NAL structure: start code (3) + NAL header (1) + payload (30) + RBSP stop (1)

**Why it works:**
- **Absolute timestamps**: Reads actual sensorTimestamp from KLV, not derived from PTS
- **Backward-only matching**: Never matches video to future KLV (prevents wrong-frame mismatches)
- **200ms tolerance**: Handles gstreamer buffering (6 frames @ 30fps). Observed lag is typically submillisecond; 200ms provides ample headroom without obscuring timing expectations.
- **Thread-safe**: Map protected by mutex, accessed from push() (app thread) and probe (streaming thread)
- **No pruning**: Map persists all entries for session lifetime (memory cost minimal: ~16 bytes/entry, ~1KB/minute @ 30fps). Video pipeline can run behind KLV push() requiring lookups to old entries. Initial 300-entry prune limit caused failures after 10s when probe needed already-removed entries.
- **Picture Timing removal**: Eliminates parser warnings from source SEI without proper VUI
- Injection before slice (not before SPS/PPS) per the downstream consumer pattern
- Emulation prevention matches ST 0604.6 Table 2 exactly
- Always creates new buffer (doesn't mutate) — safe for gstreamer refcounting

# Validation & Troubleshooting

## Verifying SEI Generation

Check output has ST 0604 SEI:
```bash
hexdump -C output.ts | grep -A2 "4d 49 53 50"  # Look for "MISP" UUID
```

Expected pattern:
```
xxxxxx  00 01 06 05 1c 4d 49 53  50 6d 69 63 72 6f 73 65  |.....MISPmicrose|
xxxxxx  63 74 69 6d 65 1f XX XX  ff XX XX ff XX XX ff XX  |ctime...........|
```
- `00 00 01` - start code
- `06` - SEI NAL type
- `05 1c` - User Unregistered (type 5), length 28
- `4d 49 53 50...` - "MISPmicrosectime" UUID
- `1f` - status byte
- `XX XX ff XX XX ff XX XX ff XX XX` - timestamp with 0xFF emulation prevention

## If parrot-to-klv isn't picking it up:

1. **Verify parrot-to-klv rebuilt against updated libmisbklv:**
   ```bash
   cd parrot-to-klv && rm -rf build && cmake -B build && cmake --build build
   ```

2. **Check video passthrough is being used:**
   - parrot-to-klv must call `KlvSink` with video_source parameter
   - Look for: `KlvSink("file:" + output, false, input_video_path)`
   - Without video_source, no SEI is generated

3. **Verify output has video PID:**
   - SEI only generated when video is present
   - Check PMT has both KLV (0x06) and video (0x1b) PIDs

4. **Check downstream extraction:**
   - the downstream consumer's SEI decoder expects byte-stream format
   - Verify tsdemux → h264parse path preserves SEI
   - If tsdemux strips SEI, extraction must happen on raw TS

## Known Limitations

- **H.264 only** — H.265 uses different UUID (deferred)
- **Always enabled** — no way to disable SEI generation for video passthrough
- **Byte ordering** — implementation assumes little-endian host (x86_64)
- **Status byte fixed** — no support for flywheel/discontinuous time flags

# Assumptions / open questions

- **Timestamp source accuracy:** Frame PTS is nanoseconds from source start (ADR
  0021); converted to microseconds for ST 0604. Assumes PTS is accurate to
  within 1µs, which gstreamer provides.
- **Emulation prevention correctness:** Generated per ST 0604.6 §7.4 Table 2
  (0xFF at byte positions 3, 6, 9); downstream must de-stuff correctly.
- **End-to-end validation pending:** Verified via hexdump (UUID present), but
  actual extraction with the downstream consumer's SEI decoder needs
  testing with parrot-to-klv → downstream client.

# Citations

[1] [`st0604`](../st0604.md) — ST 0604.6 standard reference (§7 Precision Time
    Stamp structure, UUID, emulation prevention).
[2] [`0009`](./0009-st0604-deferred.md) — original ST 0604 deferral (this
    implements the generation side).
[3] [`0020`](./0020-video-passthrough.md) — video passthrough design; this
    extends it with SEI generation.
[4] the downstream consumer's SEI implementation — reference implementation
    for encode/decode that this matches.
