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
(from KLV metadata) and ST 0604 timestamps (from H.264 SEI, via its own SEI
decoder). The client needs frame-accurate
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

Byte layout and injection point follow ST 0604.6 §7, cross-checked against the
encode/decode behaviour of the downstream consumer's existing SEI implementation
so the two interoperate without changes on their side.

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

- **Video ES is larger** — ~35 bytes of SEI per frame (~24 KB over the 699-frame
  parrot-to-klv clip), less whatever Picture Timing SEI was stripped
- **Test updated** — `gst_video_insert_test` checks ES size >= source instead of
  byte-exact, and decodes the emitted SEI back (see Validation below)
- **Always enabled** — all video passthrough operations generate ST 0604,
  whether parrot-to-klv or other consumers
- **Downstream compatible** — the consumer's existing SEI decoder extracts what
  we generate with no changes needed on their side
- **Timestamps from KLV sensorTimestamp** — absolute Unix microseconds from
  ST 0601 tag 2, matched to video frames by PTS via fuzzy lookup
- **H.264 only** — H.265 Precision uses the §7.2 UUID `a8687dd4-…` rather than
  the ASCII identifier; that and Nano (§8) stay deferred
- **A frame with no matching KLV gets no SEI** (revised 2026-07-28, see below) —
  rather than a timestamp derived from relative PTS

# Implementation Notes

Revised 2026-07-28 (see Revision below); this describes the current shape.

**Files:**
- `src/gst/gst_backend.cpp`:
  - `kPtsMatchToleranceNs` — 200 ms tolerance for the PTS match
  - `VideoCtx::pts_to_sensor_timestamp` — mutex-guarded map (PTS ns → sensorTimestamp µs), no pruning
  - `VideoCtx::h264_parser` — `GstH264NalParser` owned for the session, freed in `~VideoCtx`
  - `GstInserter::push()` — parses KLV item 2, populates the map
  - `generate_0604_sei_payload()` / `build_0604_sei_nal()` — the §7 payload and its NAL wrapper
  - `nal_is_vcl()` / `sei_nal_has_pic_timing()` — NAL classification via codecparsers
  - `on_h264_buffer_inject_sei()` — the pad probe
- `CMakeLists.txt` — `gstreamer-codecparsers-1.0`
- `test/gst_video_insert_test.cpp` — `decode_0604_seis()` plus the round-trip checks

**How it works:**
1. `open_insert()` sets `generate_sei` and creates the H.264 parser when `video_source` is present
2. `push()` extracts ST 0601 item 2 and stores `map[pts_ns] = ts_µs`
3. `on_video_pad_added()` attaches the probe to `h264parse`'s src pad
4. The probe matches the frame's PTS backward-only within tolerance; **no match means no SEI**
5. One pass with `gst_h264_parser_identify_nalu()` collects Picture Timing SEI NALs to drop
   (classified by `gst_h264_parser_parse_sei()`) and the first VCL NAL's offset
6. The access unit is rebuilt once: original bytes minus the dropped ranges, with the
   ST 0604 SEI spliced in before the first slice
7. SEI NAL: start code (3) + NAL header (1) + payload (30) + RBSP stop (1) = 35 bytes

**Why it works:**
- **Absolute timestamps** — the sensorTimestamp the KLV actually carried, not derived from PTS
- **Backward-only matching** — a frame is never given a timestamp from a later KLV packet
- **200 ms tolerance** — covers gstreamer buffering (~6 frames @ 30 fps); observed lag is
  typically submillisecond
- **Parsing is delegated** — start-code length, NAL boundaries and the SEI 0xFF-continuation
  syntax come from codecparsers rather than a hand-rolled scan
- **Offsets, not pointers** — all positions are offsets into a single mapping; pointers from
  one `gst_buffer_map()` are not valid across an unmap/remap, because mapping a multi-memory
  buffer can return a fresh merged allocation
- **Fails closed** — if the rebuilt size and the bytes written disagree, the original buffer
  passes through untouched
- **Thread-safe** — the map is mutex-guarded (`push()` on the app thread, probe on the
  streaming thread); the parser is touched only by the probe
- **No pruning** — the map persists for the session. The stated per-entry cost is a
  `std::map` node (tens of bytes, not the ~16 originally claimed), so a long session is a
  slow unbounded grower. An earlier 300-entry cap failed once the probe needed evicted
  entries; a time-based bound is the sane version if this ever matters.
- Always builds a new buffer rather than mutating — safe for gstreamer refcounting

# Wire format produced

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

SEI appears only when `video_source` is set — without it the insert path writes
KLV alone and there is no video ES to carry a timestamp.

# Revision — 2026-07-28

The decision is unchanged; the implementation was rewritten after review. The
original hand-rolled the H.264 byte scanning next to the `codecparsers` library
it had already added as a dependency but never called. Fixed:

- **Pointers reused across an unmap/remap.** The injection point and the
  Picture-Timing ranges were raw pointers into one mapping, compared against a
  second mapping's addresses; the correct `insertion_offset` was computed and
  never used. When the two mappings differ (multi-memory buffers) nothing
  matched, so a buffer sized for the edit was left partly unwritten — appending
  uninitialised heap to every frame, or overflowing when stripped ranges
  exceeded 35 bytes. Now offsets in a single mapping, with a size check that
  passes the original through if the arithmetic and the copy disagree.
- **SEI scanning was unbounded and mis-parsed the syntax.** It walked past the
  SEI NAL into slice data and read payload type/size as single bytes, which the
  0xFF-continuation encoding breaks. Now `gst_h264_parser_parse_sei()`.
- **The relative-PTS fallback invented timestamps.** On a lookup miss it emitted
  a well-formed ST 0604 timestamp near 1970 that a reader cannot tell from a
  real one. Now it emits nothing.
- **Endianness.** The timestamp was extracted by aliasing the `uint64_t`; now by
  shifting, so the wire format no longer depends on the host being little-endian.
- Minor: NAL walking is bounds-correct via `gst_h264_parser_identify_nalu()`,
  and the replacement buffer no longer leaks if its write mapping fails.

Verified against ST 0604.6 §7.1 Table 1 and §7.4 Table 2, requirements
ST 0604.4-10 / -12. Full CTest suite green, including under ASan+UBSan.

# Known limitations

- **H.264 only** — H.265 uses different UUID (deferred)
- **Always enabled** — no way to disable SEI generation for video passthrough
- **Status byte fixed** — no support for flywheel/discontinuous time flags, and
  its bit semantics come from ST 0603, which is not in `references/`, so the
  0x1F we emit is unverified against the standard that defines it
- **Picture Timing SEI is stripped from every source**, not just ones that warn —
  this deletes conformant data from the caller's video to quiet one reader. It
  predates the rewrite and is left as-is, but it is scope creep past "generate
  0604" and worth revisiting with the two-SEI question above.

# Assumptions / open questions

- **Timestamp source accuracy:** Frame PTS is nanoseconds from source start (ADR
  0021); converted to microseconds for ST 0604. Assumes PTS is accurate to
  within 1µs, which gstreamer provides.
- **Emulation prevention correctness:** Generated per ST 0604.6 §7.4 Table 2
  (0xFF at byte positions 3, 6, 9); downstream must de-stuff correctly.
- **Validation is manual; automated coverage is weak.** Verified by hand on a
  parrot-to-klv run (SEI present by hexdump, and extracted successfully by the
  downstream consumer's SEI decoder). `gst_video_insert_test` only
  asserts the output ES is *no smaller* than the source — it does not look for
  the `MISPmicrosectime` UUID, decode a timestamp, or check it against the KLV
  it came from. A regression that emitted malformed or misaligned SEI would
  still pass. Worth an assertion on the UUID and a round-trip of one frame's
  timestamp. **Closed 2026-07-28** — `gst_video_insert_test` now decodes every
  ST 0604 SEI out of the output ES and requires each one to be a timestamp the
  KLV actually carried.
- **Two ST 0604 SEIs per access unit on sources that already have one** (open).
  Writing this test showed `data/klv_metadata_test_sync.ts` carries 418 of its
  own `MISPmicrosectime` SEIs, which passthrough preserves — so frames we also
  match now hold *two* Precision Time Stamps, the source's and ours, with
  different values. A reader taking the first gets the source's; one taking the
  last gets ours. The premise in Context ("sources don't have 0604") holds for
  Parrot MP4s but not generally. Needs a decision: strip the source's when we
  generate, defer to the source's and skip generation, or leave it. Not resolved
  here because it changes what passthrough promises about the source's own data.

# Citations

[1] [`st0604`](../st0604.md) — ST 0604.6 standard reference (§7 Precision Time
    Stamp structure, UUID, emulation prevention).
[2] [`0009`](./0009-st0604-deferred.md) — original ST 0604 deferral (this
    implements the generation side).
[3] [`0020`](./0020-video-passthrough.md) — video passthrough design; this
    extends it with SEI generation.
[4] The downstream consumer's existing H.264 SEI encode/decode implementation —
    the interoperability target this was cross-checked against (not vendored;
    ST 0604.6 §7 is the normative source for the layout).
