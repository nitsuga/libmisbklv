---
type: Decision
title: Build the passthrough chain explicitly instead of with parsebin
status: accepted
tags: [decision, video-passthrough, gstreamer, packaging, phase-3]
timestamp: 2026-07-28T10:15:00Z
fork: 23
---

# Context

[`0020`](./0020-video-passthrough.md) built video passthrough on `filesrc !
parsebin`, on the reasoning that parsebin auto-plugs demuxer and parser for
whatever the container holds and never decodes — codec-agnostic for free.

That reasoning was sound and the mechanism carries a hidden dependency.

**parsebin decides a stream is fully parsed by asking whether any *decoder* in
the registry accepts its caps.** It never instantiates that decoder. It only
needs one to exist, so that it has "final caps" to compare against. On any
ordinary desktop a decoder is always installed, so the dependency is invisible.

It became visible packaging a self-contained bundle for `parrot-to-klv`: ship
only the plugins this library actually uses, and passthrough fails with

```
No final caps set yet, delaying autoplugging
Skipping factory 'h264parse' because it was already used in this chain
error: no suitable plugins found:
Missing parser: H.264 (High Profile) (video/x-h264, …, parsed=(boolean)true)
```

— a "missing parser" for a stream `h264parse` had *already parsed correctly*.
Measured, not inferred:

- adding one H.264 decoder to that plugin set fixes it outright;
- adding an unrelated decoder (vorbis, opus) does **not** — the decoder's sink
  caps must match the stream;
- so a bundle needs decoders for every codec it might carry.

The decoders that satisfy this are the encumbered ones: `openh264` (H.264 patent
pool, Cisco's royalty coverage applies to Cisco's binaries, not to a
redistributed build), `libde265` and `faad2` (GPL). Shipping GPL libraries and
patent exposure inside an Apache-2.0 tool so that a decoder can sit in a
registry and never run is not a trade worth making.

**This is not only a packaging problem.** Requiring an H.264 decoder to be
installed in order to *not* decode H.264 is a latent defect for any minimal
deployment — a container image, an embedded root filesystem, a locked-down
appliance. Those are exactly the places a passthrough-only library is attractive.

# Decision

**Construct the passthrough chain explicitly: `filesrc ! demuxer`, with the
demuxer chosen from the container and each pad linked through a parser chosen
from the codec.**

The container is settled before the pipeline is built. `sniff_container()` reads
the first 16 KiB and calls `gst_type_find_helper_for_data`; a low-confidence
guess is treated as no answer. `demuxer_for_media_type()` maps the result:

| container | demuxer |
|---|---|
| `video/quicktime` | `qtdemux` |
| `video/mpegts` | `tsdemux` |
| `video/x-matroska` | `matroskademux` |

Anything else is refused at open with a message naming the container, rather
than failing generically later.

This keeps ADR 0020's actual promise — never decode, codec-agnostic across the
video codecs — and drops only the mechanism. What it costs is autoplug's open
set of containers, replaced by a table we maintain. That is the same shape as
the parser table beside it, and being explicit about what the library demuxes is
not obviously worse than inheriting whatever happens to be installed.

**The parser table had to grow.** This is the part parsebin was quietly doing:
it plugged a parser for *every* stream, including codecs absent from our table,
so whatever reached `mpegtsmux` was always framed. A bare demuxer does not. The
table now names every codec the muxer needs framed — MPEG-1/2, H.263, VP9 —
rather than only those needing a format conversion. Parsers are idempotent, so
naming a codec costs nothing when the demuxer already parsed it.

The MPEG-1/2 case in `gst_video_insert_test` caught this immediately, failing at
`finish()` rather than at link time. Without that test the regression would have
shipped as "H.26x still works".

# Alternatives considered

## Ship decoders in the bundle

Add `openh264`, `libde265`, `faad2` so parsebin has its final caps.

**Rejected.** ~100 KB of plugins and a licensing problem: GPL libraries and H.264
patent exposure inside an Apache-2.0 distribution, for code that never executes.
It also leaves the library broken on any minimal system that has not thought to
install decoders it does not want.

## Register an inert decoder factory

Register a do-nothing element with `Codec/Decoder/Video` klass and matching sink
caps, purely so parsebin's final-caps computation succeeds.

**Rejected**, though it is small and ships nothing encumbered. It encodes a
dependency on undocumented parsebin internals into this library permanently, and
a future GStreamer that computes final caps differently would break it in a way
that reads as a mystery. Preferring the explicit chain trades a trick for a
table.

## Keep parsebin and document the requirement

**Rejected.** "Install a decoder you will never use" is not a deployment note a
passthrough library should be writing.

# Consequences

- Containers are now an explicit, maintained list. A format outside it is
  refused with a clear message; previously autoplug would have tried and
  possibly succeeded. This is a real reduction in reach, taken deliberately.
- The parser table is load-bearing in a way it was not: a codec the muxer needs
  framed and that is missing here now fails at `finish()`. The comment there
  says so.
- The dynamic-pad callbacks were unchanged — they only ever read caps off an
  incoming pad, so a demuxer's pads look the same as parsebin's did.
- One fewer element in the graph, and container sniffing happens once, up front,
  instead of during preroll.
- A consumer can now bundle this library with the LGPL GStreamer plugins alone.
  `parrot-to-klv` converts H.264, H.265 and MP4-with-audio sources against a set
  of eight plugins with no decoder present.

# Assumptions / open questions

- Assumes MP4/MOV, MPEG-TS and Matroska cover the sources this library is asked
  to carry video from. MP4 and TS are what the tests exercise; Matroska is
  listed because it is trivial and commonly wanted, and is **untested**.
- Assumes the parser table is now complete for the codecs the muxer accepts. It
  is complete for what the tests cover (H.264, H.265, MPEG-1/2); a codec that
  needs framing and is not listed will fail at `finish()` rather than at link
  time, which is a poor error and worth improving if it ever happens.
- 16 KiB of sniffing is generous for the containers listed, but a format that
  identifies itself later would be refused. None of the three do.

# Citations

- [`0020`](./0020-video-passthrough.md) — the passthrough this changes the
  mechanism of, without changing its promise
- [`0024`](./0024-sei-generation-opt-in.md) — `Sei0604::Generate` still refuses
  non-H.264, unchanged by this
- `src/gst/gst_video.cpp` — `sniff_container`, `demuxer_for_media_type`,
  `parser_for_media_type`
- `test/gst_video_insert_test.cpp` — the MPEG-1/2 case that caught the parser
  table gap
