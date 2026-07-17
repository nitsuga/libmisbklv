---
type: Prior Art
title: mkassimi98/gstklvplugin
description: GStreamer plugin suite for ST 0601.8 KLV — JSON↔KLV, per-frame injection, PMT signaling. Most relevant to the gstreamer path.
tags: [prior-art, c, gstreamer, st0601, stanag4609, st1402, mpegts]
timestamp: 2026-07-17T13:30:00Z
resource: https://github.com/mkassimi98/gstklvplugin
---

# What

GStreamer 1.x plugin suite (C11, **AGPL-3.0**, 5★, v1.0.0, updated 2026-06).
Implements SMPTE ST 336, ST 0601.8 (93 tags), STANAG 4609, ST 1402 (MPEG-TS
metadata signaling). Meson (primary) + CMake, gst-check tests, Doxygen.

# Elements

| Element | Role |
|---|---|
| `klvmetaenc` | JSON → KLV (`application/json` → `meta/x-klv`). |
| `klvmetadec` | KLV → JSON, INI-driven scaling. |
| `klvframeinject` | Per-frame KLV injection **synchronized to video** (`video/x-h264`,`video/x-h265` → `video_src` + `klv_src`). |
| `tspmtrewrite` | Rewrites the PMT to signal KLV metadata for `tsdemux`. |

# Relevant to libmisbklv — high value for the gstreamer path

- **Element decomposition maps onto our gstreamer backend:** encode, decode,
  frame-inject, and PMT-rewrite are exactly the four surfaces we need. Study
  before designing our gstreamer integration.
- **INI-driven tag registry** (`stanag4609_tags.ini`) — data-driven item
  definitions decoupled from code. Strong pattern for 0601.19's ~143 items (vs
  hardcoding). Adopt something like this.
- **PMT signaling detail (load-bearing):** uses `stream_type = 0x06` +
  `registration_descriptor = KLVA`, **not** `0x15`, because GStreamer
  `tsdemux` expects raw KLV on `0x06 + KLVA`; plain `0x15` would need
  metadata access-unit wrapping that `mpegtsmux` doesn't produce. This is a
  concrete gotcha for our TS mux/demux — and it's the signaling the real
  samples use (see [`data-samples`](/data-samples.md)).
- **JSON interchange format:** flat object, numeric string keys; nested local
  sets as `hex:`/`base64:`-prefixed strings. Useful as an optional tooling
  surface (our native API is C++).
- **Pipeline shapes** for SRT/UDP/file in Python and C++ — reference for our
  streaming vs file-based paths.
- **Avoid:** **AGPL-3.0** — strong copyleft; cannot lift code into a
  permissive lib. Design reference + independent reimplementation only. Covers
  0601.8 (93 tags), not 0601.19.

# Relationships

Profile of [0107](/st0107.md); items of [0601](/st0601.md) (at rev .8); ST 1402
for the MPEG-TS carriage (complements our [0107](/st0107.md) §6 carriage
knowledge). The [klvdata](/prior-art-klvdata.md) ffmpeg demux idiom is the
ffmpeg-path analog of this plugin's gstreamer demux.

# Citations

[1] [github.com/mkassimi98/gstklvplugin](https://github.com/mkassimi98/gstklvplugin) (README).
